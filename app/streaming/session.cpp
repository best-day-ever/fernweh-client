#include <QtGlobal>
#include <QScopeGuard>
#include <QDebug>
#include <QThread>
#include "monotonicclock.h"
#include "session.h"
#include "videopipelineprogress.h"
#include "backend/macsharedsession.h"
#include "backend/remotestreamsetup.h"
#include "sessionguidispatch.h"
#include "backend/collaboration.h"
#include "planksharinghud.h"
#include <QDeadlineTimer>
#include "streaming/clientframeflowtrace.h"
#include "backend/hostrecovery.h"
#include "backend/planknetwork.h"
#include "backend/plankbroker.h"
#include "settings/streamingpreferences.h"
#include "settings/plankclientpolicy.h"
#include <QReadLocker>
#include "streaming/avsynccontroller.h"
#include "streaming/plankdisplaymode.h"
#include "streaming/planktoolbar.h"
#include "backend/clientinstances.h"
#include "streaming/planksessionevents.h"
#include "streaming/input/plankmousemotion.h"
#include "streaming/input/penmetrics.h"
#include "streaming/streamutils.h"
#include "backend/clientdisplayprobe.h"
#include "backend/displayprofile.h"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#include "clipboardsync.h"
#ifdef PLANK_TRANSPORT
#ifdef Q_OS_WIN
#include "windowsfileclipboard.h"
using PlatformFileClipboard = WindowsFileClipboard;
#else
#include "macfileclipboard.h"
using PlatformFileClipboard = MacFileClipboard;
#endif
#endif
#endif
#include "backend/computermanager.h"
#include "backend/nvaddress.h"
#ifdef Q_OS_MACOS
#include "macapplication.h"
#endif
#ifdef Q_OS_DARWIN
#include "streaming/macwindow.h"
#include "streaming/macdisplayinfo.h"
#include "streaming/macdisplaygeometry.h"
#include "streaming/video/decodercaps.h"
#endif

#include <Limelight.h>
#include "utils.h"

#ifdef HAVE_FFMPEG
#include "video/ffmpeg.h"
#endif

#ifdef HAVE_SLVIDEO
#include "video/slvid.h"
#endif

#ifdef Q_OS_WIN32
// Scaling the icon down on Win32 looks dreadful, so render at lower res
#define ICON_SIZE 32
#else
#define ICON_SIZE 64
#endif

// HACK: Remove once proper Dark Mode support lands in SDL
#ifdef Q_OS_WIN32
#include <SDL3/SDL_system.h>
#include <dwmapi.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_OLD
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#endif


#define SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER 100
#define SDL_CODE_PLANK_RECONNECT 105
#define SDL_CODE_PLANK_BITRATE_APPLIED 106
#define SDL_CODE_PLANK_CURSOR 107
#define SDL_CODE_PLANK_TABLET_CURSOR 108
#define SDL_CODE_PLANK_CURSOR_POSITION 109
#define SDL_CODE_PLANK_REPLANK_COMPLETE 110
#define SDL_CODE_PLANK_CLIPBOARD 111
#define SDL_CODE_PLANK_CLIPBOARD_POLL 112
#define SDL_CODE_PLANK_FILE_CLIPBOARD_READY 113
#define SDL_CODE_PLANK_FILE_CLIPBOARD_PUBLISH 114
#define SDL_CODE_PLANK_DECODED_FRAME_SIZE 115
#define SDL_CODE_PLANK_APPLY_SCREENS 116
#define SDL_CODE_PLANK_COLLABORATION_PERMISSION 117
#define SDL_CODE_PLANK_POINTER_CAPTURE 118

#include <QtEndian>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QImage>
#include <QGuiApplication>
#include <QCursor>
#include <QWindow>
#include <QScreen>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSettings>

#ifdef PLANK_TRANSPORT
#include "plank_transport.h"
#include "plank_metrics.h"
#include "plank_transport_control.h"
#include "plank_transport_camera.h"
#include "plank_transport_event.h"
#include "plank_transport_input.h"
#include "plank_transport_setup.h"
#endif

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>
#ifdef PLANK_QT_SESSION
#include "qt/sessionruntime.h"
#include "qt/sessioninput.h"
#endif

CONNECTION_LISTENER_CALLBACKS Session::k_ConnCallbacks = {
    Session::clStageStarting,
    nullptr,
    Session::clStageFailed,
    nullptr,
    Session::clConnectionTerminated,
    Session::clLogMessage,
    Session::clConnectionStatusUpdate,
    Session::clRawHidControl,
    Session::clVideoBitrateApplied,
    Session::clCursorChunk,
    Session::clCursorPosition,
    nullptr, // Native transport publishes the paired FEC counters directly.
};

Session* Session::s_ActiveSession;
QSemaphore Session::s_ActiveSessionSemaphore(1);

void Session::clStageStarting(int stage)
{
    // LiStartConnection runs on the serialized session worker. Qt delivers
    // these signals to the GUI thread; callbacks never manipulate windows.
    emit s_ActiveSession->stageStarting(QString::fromLocal8Bit(LiGetStageName(stage)));
}

void Session::clStageFailed(int stage, int errorCode)
{
    if (s_ActiveSession->m_Lifecycle.recovering()) {
        qWarning("PLANK reconnect stage failed: %s (%d)",
                    LiGetStageName(stage), errorCode);
        return;
    }

    s_ActiveSession->m_Diagnostics.end(SessionDiagnostics::End::StartupFailed, errorCode);
    QString failingEndpoint;
    if (stage == STAGE_SESSION_NEGOTIATION || stage == STAGE_CONTROL_STREAM_START) {
        failingEndpoint = QStringLiteral("UDP %1").arg(
                    s_ActiveSession->m_Computer->activeAddress.port());
    }

    emit s_ActiveSession->stageFailed(QString::fromLocal8Bit(LiGetStageName(stage)),
                                      errorCode,
                                      failingEndpoint);
}

void Session::clConnectionTerminated(int errorCode)
{
    if (!s_ActiveSession) return;
    if (errorCode != 0) emit s_ActiveSession->diagnosticConnectionLost(errorCode);
    if (static_cast<std::uint32_t>(errorCode) ==
            PLANK_TRANSPORT_TERMINATION_SESSION_TAKEN_OVER) {
        s_ActiveSession->m_Diagnostics.end(SessionDiagnostics::End::SessionTakenOver, errorCode);
        s_ActiveSession->m_CanReconnect.store(false);
        s_ActiveSession->m_Lifecycle.stop();
        s_ActiveSession->m_UnexpectedTermination = false;
        emit s_ActiveSession->displayLaunchError(
                    tr("This BDE fernweh session was transferred to another client."));

        requestSessionQuit();
        return;
    }

    if (s_ActiveSession->m_Computer->plankAuthentication &&
            s_ActiveSession->m_CanReconnect.load() &&
            s_ActiveSession->m_Lifecycle.requestRecovery()) {
        qWarning("PLANK transport ended (%d); starting responsive reconnect",
                    errorCode);
#ifdef PLANK_QT_SESSION
        emit s_ActiveSession->qtReconnectRequested();
#else
        s_ActiveSession->m_ReconnectRequested.store(true);
        SDL_Event event {};
        event.type = SDL_EVENT_USER;
        event.user.code = SDL_CODE_PLANK_RECONNECT;
        SDL_PushEvent(&event);
#endif
        return;
    }

    // The early worker probe and transport termination can arrive together.
    // A queued reconnect is already owned by the Qt session worker, just like a
    // running one; duplicate callbacks must not enqueue a fatal quit.
    if (s_ActiveSession->m_Lifecycle.recoveryPending()) {
        qWarning("PLANK reconnect attempt ended: %d", errorCode);
        return;
    }

    s_ActiveSession->m_Diagnostics.end(SessionDiagnostics::End::ConnectionTerminated, errorCode);
    // Display the termination dialog if this was not intended
    switch (errorCode) {
    case ML_ERROR_GRACEFUL_TERMINATION:
        break;

    case ML_ERROR_NO_VIDEO_TRAFFIC:
        s_ActiveSession->m_UnexpectedTermination = true;

        emit s_ActiveSession->displayLaunchError(tr("No video received from host.") + "\n\n"+
                                                 tr("Check your firewall rules for UDP port %1.")
                                                 .arg(s_ActiveSession->m_Computer->activeAddress.port()));
        break;

    case ML_ERROR_NO_VIDEO_FRAME:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("Your network connection isn't performing well. Reduce your video bitrate setting or try a faster connection."));
        break;

    case ML_ERROR_PROTECTED_CONTENT:
    case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("Something went wrong on your host PC when starting the stream.") + "\n\n" +
                                                 tr("Make sure you don't have any DRM-protected content open on your host PC. You can also try restarting your host PC."));
        break;

    case ML_ERROR_FRAME_CONVERSION:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("The host PC reported a fatal video encoding error.") + "\n\n" +
                                                 tr("Try changing the streaming resolution or the Host display resolution."));
        break;

    default:
        s_ActiveSession->m_UnexpectedTermination = true;

        // We'll assume large errors are hex values
        bool hexError = qAbs(errorCode) > 1000;
        emit s_ActiveSession->displayLaunchError(tr("Connection terminated") + "\n\n" +
                                                 tr("Error code: %1").arg(errorCode, hexError ? 8 : 0, hexError ? 16 : 10, QChar('0')));
        break;
    }

    qCritical("Connection terminated: %d",
                 errorCode);

    // Push a quit event to the main loop
    requestSessionQuit();
}

void Session::openNewConnection()
{
    if (!ClientInstances::get().openNewConnection()) {
        emit displayLaunchWarning(tr("Could not open another Fernweh window. Wait for any application update to finish, then try again."));
    }
}

void Session::requestSessionQuit(SessionDiagnostics::End reason)
{
    if (!s_ActiveSession) return;
    if (reason != SessionDiagnostics::End::Unknown)
        s_ActiveSession->m_Diagnostics.end(reason);
#ifdef PLANK_QT_SESSION
    emit s_ActiveSession->qtQuitRequested();
#else
    SDL_Event event {};
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
#endif
}

void Session::requestDecoderReset()
{
    if (!s_ActiveSession) return;
#ifdef PLANK_QT_SESSION
    emit s_ActiveSession->qtDecoderResetRequested();
#else
    SDL_Event event {};
    event.type = SDL_EVENT_RENDER_DEVICE_RESET;
    SDL_PushEvent(&event);
#endif
}

void Session::clLogMessage(const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    const auto message = QString::vasprintf(format, ap);
    va_end(ap);
    qInfo().noquote() << message;
}

void Session::clConnectionStatusUpdate(int connectionStatus)
{
    if (!s_ActiveSession) return;
#ifdef PLANK_QT_SESSION
    if (s_ActiveSession->m_Lifecycle.recovering() || !s_ActiveSession->m_Preferences->connectionWarnings) return;
    if (connectionStatus == CONN_STATUS_POOR) {
        s_ActiveSession->setPlankReconnectStatus(s_ActiveSession->m_StreamConfig.bitrate > 5000 ?
            "Slow connection to PC\nReduce your bitrate" : "Poor connection to PC", true);
    } else if (connectionStatus == CONN_STATUS_OKAY) {
        s_ActiveSession->setPlankReconnectStatus("", false);
    }
#else
    if (s_ActiveSession->m_Reconnecting.load()) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Connection status update: %d",
                connectionStatus);

    if (!s_ActiveSession->m_Preferences->connectionWarnings) {
        return;
    }

    switch (connectionStatus)
    {
    case CONN_STATUS_POOR:
        s_ActiveSession->m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                                            s_ActiveSession->m_StreamConfig.bitrate > 5000 ?
                                                                "Slow connection to PC\nReduce your bitrate" : "Poor connection to PC");
        s_ActiveSession->m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
        break;
    case CONN_STATUS_OKAY:
        s_ActiveSession->m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
        break;
    }
#endif
}

void Session::clRawHidControl(const unsigned char* data, unsigned int length)
{
    if (!s_ActiveSession) return;
#ifdef PLANK_QT_SESSION
    s_ActiveSession->m_QtInputCallbacks.invoke([&](QtSessionInput& input) { input.handleRawHidControl(data, length); });
#else
    s_ActiveSession->m_InputCallbacks.invoke([&](SdlInputHandler& input) {
        input.handleRawHidControl(data, length);
    });
#endif
}

void Session::postPointerCaptureEvent()
{
#ifndef PLANK_QT_SESSION
    SDL_Event event {};
    event.type = SDL_EVENT_USER;
    event.user.code = SDL_CODE_PLANK_POINTER_CAPTURE;
    SDL_PushEvent(&event);
#endif
}

void Session::clVideoBitrateApplied(
        uint32_t requestedKbps, uint32_t appliedKbps, uint32_t peakKbps)
{
    if (s_ActiveSession == nullptr) {
        return;
    }

    s_ActiveSession->m_ConfirmedBitrateRequestKbps.store(
                static_cast<int>(requestedKbps), std::memory_order_relaxed);
    s_ActiveSession->m_ConfirmedBitrateAppliedKbps.store(
                static_cast<int>(appliedKbps), std::memory_order_relaxed);
    s_ActiveSession->m_ConfirmedBitratePeakKbps.store(
                static_cast<int>(peakKbps), std::memory_order_relaxed);

#ifndef PLANK_QT_SESSION
    SDL_Event event {};
    event.type = SDL_EVENT_USER;
    event.user.code = SDL_CODE_PLANK_BITRATE_APPLIED;
    SDL_PushEvent(&event);
#endif
}

void Session::clCursorChunk(const unsigned char* data, unsigned int length)
{
    if (!s_ActiveSession) return;
#ifdef PLANK_QT_SESSION
    s_ActiveSession->m_QtInputCallbacks.invoke([&](QtSessionInput& input) { input.handleCursorShape(data, length); });
#else
    s_ActiveSession->m_InputCallbacks.invoke([&](SdlInputHandler& input) {
        if (input.handleRemoteCursorChunk(data, length)) {
            SDL_Event event {};
            event.type = SDL_EVENT_USER;
            event.user.code = SDL_CODE_PLANK_CURSOR;
            SDL_PushEvent(&event);
        }
    });
#endif
}

void Session::clCursorPosition(const unsigned char* data, unsigned int length)
{
    if (!s_ActiveSession) return;
#ifdef PLANK_QT_SESSION
    s_ActiveSession->m_QtInputCallbacks.invoke([&](QtSessionInput& input) { input.handleCursorPosition(data, length); });
#else
    s_ActiveSession->m_InputCallbacks.invoke([&](SdlInputHandler& input) {
        if (input.handleRemoteCursorPosition(data, length)) {
            SDL_Event event {};
            event.type = SDL_EVENT_USER;
            event.user.code = SDL_CODE_PLANK_CURSOR_POSITION;
            SDL_PushEvent(&event);
        }
    });
#endif
}

void Session::destroyInputHandler()
{
    // Control callbacks keep running until deferred connection cleanup. Retire
    // their borrowed target before destroying the input handler on this thread.
    m_InputCallbacks.clear();
    delete m_InputHandler;
    m_InputHandler = nullptr;
}

void Session::notifyDecodedFrameSize(int width, int height)
{
    Session* session = s_ActiveSession;
    if (session == nullptr || width <= 0 || height <= 0) {
        return;
    }
    session->m_DecodedFrameSize.store(
                (static_cast<std::uint64_t>(width) << 32) |
                static_cast<std::uint32_t>(height),
                std::memory_order_relaxed);
    SDL_Event event = {};
    event.type = SDL_EVENT_USER;
    event.user.code = SDL_CODE_PLANK_DECODED_FRAME_SIZE;
    SDL_PushEvent(&event);
}

void Session::postTabletCursorActivationEvent()
{
    SDL_Event event = {};
    event.type = SDL_EVENT_USER;
    event.user.code = SDL_CODE_PLANK_TABLET_CURSOR;
    SDL_PushEvent(&event);
}

void Session::updateVideoFecLoss(VideoFecLossPercent loss)
{
    Session* session = s_ActiveSession;
    if (session == nullptr) {
        return;
    }

    const quint64 now = PlankClock::milliseconds();
    std::lock_guard<std::mutex> lock(session->m_VideoPacketLossSamplesLock);
    // Publish both ten-second peaks together. Toolbar and overlay share this
    // snapshot; frame drops and decoder/render queues are separate metrics.
    session->m_CurrentVideoFecLoss = {
        session->m_VideoPacketLossPeakWindow.addSample(now, loss.before),
        session->m_VideoPacketLossAfterFecPeakWindow.addSample(now, loss.after)
    };
}

bool Session::chooseDecoder(int videoFormat, int width, int height,
                            int frameRate, bool enableVsync, bool testOnly,
                            IVideoDecoder*& chosenDecoder, bool enableIdentityGbr,
                            DecoderCaptureSource captureSource,
                            DecoderEncoderBackend encoderBackend,
                            IFFmpegRendererFactory* presenter,
                            SDL_Window* window,
                            bool enableFramePacing,
                            DecoderSelectionMode selectionMode)
{
    DECODER_PARAMETERS params = {};

    // We should never have vsync enabled for test-mode.
    // It introduces unnecessary delay for renderers that may
    // block while waiting for a backbuffer swap.
    Q_ASSERT(!enableVsync || !testOnly);

    params.width = width;
    params.height = height;
    params.frameRate = frameRate;
    params.videoFormat = videoFormat;
    params.window = window;
    params.enableVsync = enableVsync;
    params.enableFramePacing = enableFramePacing;
    params.enableIdentityGbr = enableIdentityGbr;
    params.testOnly = testOnly;
    params.captureSource = captureSource;
    params.encoderBackend = encoderBackend;
    params.rendererFactory = presenter;
    params.selectionMode = selectionMode;
    params.presentationLayout = !testOnly && s_ActiveSession ?
                &s_ActiveSession->m_PresentationLayout : nullptr;

    qInfo("V-sync %s",
                enableVsync ? "enabled" : "disabled");

#ifdef HAVE_FFMPEG
    chosenDecoder = new FFmpegVideoDecoder(testOnly);
    if (chosenDecoder->initialize(&params)) {
        qInfo("FFmpeg-based video decoder chosen");
        return true;
    }
    else {
        qCritical("Unable to load FFmpeg decoder");
        delete chosenDecoder;
        chosenDecoder = nullptr;
    }
#endif

#ifndef HAVE_FFMPEG
#error No video decoding libraries available!
#endif

    // If we reach this, we didn't initialize any decoders successfully
    return false;
}

QString Session::decoderFailureMessage(int videoFormat)
{
    Q_UNUSED(videoFormat);
    return tr("This Client cannot decode and present the selected encoding profile with its exact resolution, bit depth and color format.");
}

bool Session::isIdentityGbrEnabledForFormat(int videoFormat) const
{
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) return false;
    return (videoFormat == VIDEO_FORMAT_H264_HIGH8_444 ||
            videoFormat == VIDEO_FORMAT_H264_HIGH10_444 ||
            videoFormat == VIDEO_FORMAT_H265_REXT8_444 ||
            videoFormat == VIDEO_FORMAT_H265_REXT10_444) &&
           (m_Computer->serverCodecModeSupport & SCM_IDENTITY_GBR_444);
}

bool Session::selectAutomaticVideoProfile(SDL_Window* testWindow, std::optional<DecoderProbe>& probe)
{
    const bool savedChoice = m_Computer->plankSavedBenchmarkChoice;
    if (!m_Computer->plankAutomaticVideoProfile && !savedChoice) return true;

    RemoteStreamSetup::Capabilities caps;
    caps.known = true;
    caps.platform = RemoteStreamSetup::LinuxPlatform;
    caps.featureFlags = m_PlankLaunchFeatureFlags;
    caps.encodingModes = m_Computer->plankEncodingModes;
    const int width = m_StreamConfig.width;
    const int height = m_StreamConfig.height;
    const int fps = m_StreamConfig.fps;
    const auto original = m_PlankVideoProfile;
    auto profiles = RemoteStreamSetup::automaticNvencProfiles(caps);
    const auto& recommended = m_Computer->plankBenchmarkRecommendation;
    const int preferred = recommended.value("profile").toInt(-1);
    const int recommendedKbps = recommended.value("bitrate_kbps").toInt();
    const bool fitsRecommendation = profiles.contains(preferred) &&
            (!savedChoice || recommended.value("capture").toInt(-1) == m_PlankCaptureSource) &&
            recommended.value("width").toInt() == width &&
            recommended.value("height").toInt() == height &&
            recommended.value("fps").toInt() == fps &&
            recommendedKbps == StreamingPreferences::plankBitrateForProfile(
                                   savedChoice ? StreamingPreferences::plankDefaultProfileBitrates() :
                                                 m_Computer->plankProfileBitratesKbps, preferred);
    if (savedChoice && fitsRecommendation && !m_Computer->plankAutomaticVideoProfile) {
        profiles = {preferred};
    } else if (fitsRecommendation) {
        profiles.move(profiles.indexOf(preferred), 0);
    } else if (!m_Computer->plankAutomaticVideoProfile) {
        return true;
    }
    for (const int profile : profiles) {
        const auto* encoding = plank_encoding_profile(profile);
        const int format = encoding->video_format;
        const bool identity = isIdentityGbrEnabledForFormat(format);
        SupportedVideoFormatList candidate;
        candidate.append(format);
        if (!candidate.maskByServerCodecModes(m_Computer->serverCodecModeSupport) ||
                ((format & VIDEO_FORMAT_MASK_YUV444) && !identity) ||
                !plank_encoding_explicit_dimensions(encoding, width, height)) {
            continue;
        }
        IVideoDecoder* decoder = nullptr;
        if (chooseDecoder(format, width, height, fps, false, true, decoder,
                          identity, decoderCaptureSource(), DecoderEncoderBackend::NvencDirect,
                          m_RendererFactory, testWindow, false, DecoderSelectionMode::ExactHardwareOnly)) {
            const DecoderProbe tested {decoder->getDecoderCapabilities(), decoder->getDecoderColorspace(),
                                       decoder->getDecoderColorRange()};
            delete decoder;
            m_PlankVideoProfile = static_cast<StreamingPreferences::PlankVideoProfile>(profile);
            const bool appliedSavedChoice = savedChoice && fitsRecommendation && preferred == profile;
            m_PlankBitrateKbps = appliedSavedChoice ? recommendedKbps : StreamingPreferences::plankBitrateForProfile(
                        m_Computer->plankProfileBitratesKbps, profile);
            qInfo() << (appliedSavedChoice ? "Saved benchmark video profile:" : "Automatic video profile:")
                    << encoding->mode << m_PlankBitrateKbps << "kbps (hardware decoder verified)";
            if (m_PlankVideoProfile != original && !configurePlankLaunchGeometry()) return false;
            if (width == m_StreamConfig.width && height == m_StreamConfig.height && fps == m_StreamConfig.fps) {
                probe = tested;
            }
            return true;
        }
    }
    qInfo() << "Automatic video profile: keeping"
            << StreamingPreferences::plankEncodingMode(original)
            << "(no automatic profile passed hardware validation)";
    return true;
}

int Session::drSetup(int videoFormat, int width, int height, int frameRate, void *, int)
{
    s_ActiveSession->m_ActiveVideoFormat = videoFormat;
    s_ActiveSession->m_ActiveVideoWidth = width;
    s_ActiveSession->m_ActiveVideoHeight = height;
    s_ActiveSession->m_ActiveVideoFrameRate = frameRate;

    // The Qt session worker creates the decoder after stream negotiation.

    qInfo("Video stream is %dx%dx%d (format 0x%x)",
                width, height, frameRate, videoFormat);
    if (s_ActiveSession->isIdentityGbrEnabledForFormat(videoFormat)) {
        if (videoFormat == VIDEO_FORMAT_H264_HIGH8_444) {
            qInfo("Video precision: native 8-bit RGB -> "
                        "8-bit H.264 4:4:4 -> 8-bit RGB identity presentation");
        }
        else if (videoFormat == VIDEO_FORMAT_H265_REXT8_444) {
            qInfo("Video precision: native 8-bit NvFBC source -> "
                        "8-bit HEVC 4:4:4 -> 8-bit RGB identity presentation");
        }
        else {
            const bool native10 = s_ActiveSession->m_PlankCaptureSource ==
                    StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10;
            qInfo("Video precision: %s -> 10-bit %s 4:4:4 -> "
                        "10-bit RGB identity presentation",
                        native10 ? "native 10-bit X11/XShm" :
                                   "8-bit NvFBC source/up-converted",
                        videoFormat == VIDEO_FORMAT_H264_HIGH10_444 ? "H.264" : "HEVC");
        }
    }
    else if (videoFormat == VIDEO_FORMAT_H264_HIGH8_422 ||
             videoFormat == VIDEO_FORMAT_H264_HIGH10_422) {
        qInfo("Video precision: 8-bit source -> %s H.264 4:2:2 -> "
                    "BT.709 full-range YCbCr presentation",
                    videoFormat == VIDEO_FORMAT_H264_HIGH10_422 ? "10-bit" : "8-bit");
    }
    else if (StreamingPreferences::isPlankNvenc420Profile(s_ActiveSession->m_PlankVideoProfile)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Video precision: 8-bit NvFBC source -> %s 4:2:0 NVENC -> "
                    "BT.709 limited-range YCbCr (sRGB transfer) presentation",
                    videoFormat == VIDEO_FORMAT_H265_MAIN10 ? "10-bit HEVC Main10" : "8-bit H.264 High");
    }

#ifdef PLANK_TRANSPORT
    if (s_ActiveSession->m_PlankTransportEndpoint) {
        const auto* profile = plank_encoding_profile(s_ActiveSession->m_PlankVideoProfile);
        const char* capture = plank_capture_name(s_ActiveSession->m_PlankCaptureSource);
        if (profile && capture) {
            PlankMetricsStreamInfo info {};
            info.struct_size = sizeof(info); info.width = width; info.height = height;
            info.requested_fps = frameRate;
            info.capture_source = capture; info.encoding_profile = profile->mode;
            plank_transport_native_metrics_stream_info(s_ActiveSession->m_PlankTransportEndpoint, &info);
        }
    }
#endif
    return 0;
}

int Session::drSubmitDecodeUnit(PDECODE_UNIT du)
{
    // Reconnect/teardown owns the decoder while this lock is unavailable.
    std::unique_lock<std::mutex> lock(s_ActiveSession->m_DecoderLock, std::try_to_lock);
    if (!lock || !s_ActiveSession->m_VideoDecoder) return DR_OK;
    return s_ActiveSession->m_VideoDecoder->submitDecodeUnit(du);
}

DecoderCaptureSource Session::decoderCaptureSource() const
{
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
        return DecoderCaptureSource::ScreenCaptureKit;
    }
    return m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10 ?
                DecoderCaptureSource::NativeX11_10Bit : DecoderCaptureSource::Nvfbc8Bit;
}

DecoderEncoderBackend Session::decoderEncoderBackend() const
{
    if (StreamingPreferences::isPlankAppleProfile(m_PlankVideoProfile)) {
        return DecoderEncoderBackend::VideoToolbox;
    }
    return StreamingPreferences::isPlankNvencProfile(m_PlankVideoProfile) ?
                DecoderEncoderBackend::NvencDirect : DecoderEncoderBackend::SoftwareCuda;
}

void Session::populateDecoderProperties(const DecoderProbe& probe)
{
    m_VideoCallbacks.capabilities = probe.capabilities;
    if (m_VideoCallbacks.capabilities & CAPABILITY_PULL_RENDERER) {
        // It is an error to pass a push callback when in pull mode
        m_VideoCallbacks.submitDecodeUnit = nullptr;
    }
    else {
        m_VideoCallbacks.submitDecodeUnit = drSubmitDecodeUnit;
    }

    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT ||
            StreamingPreferences::isPlankNvenc420Profile(m_PlankVideoProfile)) {
        // These profiles have an exact, negotiated color contract. An environment
        // override must not reinterpret its YCbCr samples as full-range or RGB.
        // The Linux host refuses an NVENC 4:2:0 launch with any other value.
        m_StreamConfig.colorSpace = COLORSPACE_REC_709;
        m_StreamConfig.colorRange = COLOR_RANGE_LIMITED;
    }
    else {
        bool ok;

        m_StreamConfig.colorSpace = qEnvironmentVariableIntValue("COLOR_SPACE_OVERRIDE", &ok);
        if (ok) {
            qWarning("Using colorspace override: %d",
                        m_StreamConfig.colorSpace);
        }
        else {
            m_StreamConfig.colorSpace = probe.colorSpace;
        }

        m_StreamConfig.colorRange = qEnvironmentVariableIntValue("COLOR_RANGE_OVERRIDE", &ok);
        if (ok) {
            qWarning("Using color range override: %d",
                        m_StreamConfig.colorRange);
        }
        else {
            m_StreamConfig.colorRange = probe.colorRange;
        }
    }
}

Session::Session(NvComputer* computer, NvApp& app,
                 StreamingPreferences *preferences,
                 ComputerManager *computerManager)
    : m_Preferences(preferences ? preferences : StreamingPreferences::get()),
      m_IsFullScreen(m_Preferences->windowMode != StreamingPreferences::WM_WINDOWED || !WMUtils::isRunningDesktopEnvironment()),
      m_Computer(computer),
      m_PlankVideoProfile(static_cast<StreamingPreferences::PlankVideoProfile>(
              qBound(static_cast<int>(StreamingPreferences::PLANK_PROFILE_H264_10BIT_444),
                     MacSharedSession::videoProfile(computer->outputTopology, computer->plankVideoProfile, computer->plankSharedDesktop),
                     static_cast<int>(StreamingPreferences::PLANK_PROFILE_COUNT) - 1))),
      m_PlankCaptureSource(static_cast<StreamingPreferences::PlankCaptureSource>(
              qBound(static_cast<int>(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT),
                     computer->plankCaptureSource,
                     static_cast<int>(StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT)))),
      m_PlankBitrateKbps(
              StreamingPreferences::plankBitrateForProfile(
                  computer->plankProfileBitratesKbps,
                  computer->plankVideoProfile)),
      m_ComputerManager(computerManager),
      m_App(app),
      m_VideoDecoder(nullptr),
      m_AudioMuted(false),
      m_UnexpectedTermination(true), // Failure prior to streaming is unexpected
      m_CanReconnect(false),
      m_AsyncConnectionSuccess(false),
      m_OpusDecoder(nullptr),
      m_AudioRenderer(nullptr),
      m_AudioSampleCount(0),
      m_DropAudioEndTime(0),
      m_AudioMediaFramesReceived(0),
      m_AvSyncTelemetryEnabled(qEnvironmentVariableIntValue("PLANK_AV_SYNC_TELEMETRY") > 0),
      m_LastAudioTelemetryTime(0),
      m_CurrentRenderedFps(0.0f),
      m_CurrentVideoMbps(0.0f),
      m_CurrentNetworkRttMs(0)
{
    connect(this, &Session::displayLaunchError, this, [this] {
        m_Diagnostics.end(SessionDiagnostics::End::DisplayFailed);
    }, Qt::DirectConnection);
    if (m_Computer->plankAuthentication) {
        if (m_ComputerManager != nullptr) {
            m_ComputerManager->takePlankReconnectCredentials(
                        m_Computer, m_PlankUsername,
                        m_PlankPassword);
            m_CanReconnect.store(!m_PlankUsername.isEmpty() &&
                                 !m_PlankPassword.isEmpty());
        }
        PlankAvSync::resetVideoClock();

        // PLANK is a qualified workstation protocol, not a generic
        // game-streaming profile: it always streams at 60 fps (applied in
        // initialize() without touching the shared preferences). Its
        // stream size is selected after SDL video initialization from the
        // target client display or explicit override. The bookmark (or the
        // remote workstation's stream settings) owns one startup encoder
        // target for each exact encoding profile. The selected value is a
        // session-local copy, and the toolbar never writes changes back.
        // Decoder selection is internal and exact-profile constrained. Hardware
        // is accepted only after a test frame proves the requested bit depth,
        // chroma sampling, and identity mapping; otherwise the same profile
        // falls back to FFmpeg software decoding without changing formats.
    }
}

void Session::diagnosticPhase(SessionDiagnostics::Phase phase)
{
    m_Diagnostics.phase(phase);
    publishDiagnosticLifecycle();
}

void Session::publishDiagnosticLifecycle(bool final)
{
    emit diagnosticLifecycle(m_Diagnostics.snapshot(), final);
}

Session::~Session()
{
    m_Diagnostics.end(SessionDiagnostics::End::SessionDestroyed);
    publishDiagnosticLifecycle(true);
    stopPlankTransportDataPlane();
    clearPlankReconnectCredentials();
}

bool Session::startPlankTransportDataPlane(quint16 port,
                                      const QString& certificateSha256,
                                      const QString& token,
                                      quint16 quicUdpPayloadMtu)
{
#ifndef PLANK_TRANSPORT
    Q_UNUSED(port)
    Q_UNUSED(certificateSha256)
    Q_UNUSED(token)
    Q_UNUSED(quicUdpPayloadMtu)
    return false;
#else
    stopPlankTransportDataPlane();

    QHostAddress remoteHost(m_Computer->activeAddress.address());
    if (remoteHost.isNull()) {
        const QHostInfo resolved = QHostInfo::fromName(
                    m_Computer->activeAddress.address());
        for (const QHostAddress& candidate : resolved.addresses()) {
            if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
                remoteHost = candidate;
                break;
            }
        }
        if (remoteHost.isNull() && !resolved.addresses().isEmpty()) {
            remoteHost = resolved.addresses().first();
        }
    }
    if (remoteHost.isNull()) {
        qWarning() << "Unable to resolve the PLANK transport endpoint";
        return false;
    }

    const QString remoteAddress = remoteHost.protocol() == QAbstractSocket::IPv6Protocol ?
                QStringLiteral("[%1]:%2").arg(remoteHost.toString()).arg(port) :
                QStringLiteral("%1:%2").arg(remoteHost.toString()).arg(port);
    const QByteArray remoteAddressUtf8 = remoteAddress.toUtf8();
    const QByteArray serverNameUtf8("plank");
    const QByteArray certificateUtf8 = certificateSha256.toLatin1();
    const QByteArray tokenUtf8 = token.toLatin1();

    PlankTransportConfig config {};
    config.struct_size = sizeof(config);
    config.abi_version = PLANK_TRANSPORT_ABI_VERSION;
    config.mode = PLANK_TRANSPORT_MODE_CLIENT;
    config.handshake_timeout_ms = 10000;
    config.idle_timeout_ms = 30000;
    config.keep_alive_interval_ms = 5000;
    config.max_udp_payload_size = quicUdpPayloadMtu;
    config.file_clipboard_enabled = m_FileClipboardMode != QStringLiteral("off");
    qInfo() << "Using the negotiated fixed maximum QUIC UDP payload:"
            << config.max_udp_payload_size << "bytes";
    config.remote_address = remoteAddressUtf8.constData();
    config.server_name = serverNameUtf8.constData();
    config.certificate_sha256 = certificateUtf8.constData();
    config.session_token = tokenUtf8.constData();

    PlankTransportNativeEndpoint* endpoint = nullptr;
    PlankMetricsHandle* metrics = nullptr;
    int result = plank_transport_native_endpoint_create(&config, &endpoint);
    if (result == PLANK_TRANSPORT_OK) {
        const auto options = PlankClientPolicy().metrics();
        if (!options.error.isEmpty()) qWarning().noquote() << options.error;
        if (options.enabled) {
            QByteArray workstation, slot;
            {
                QReadLocker computerLock(&m_Computer->lock);
                // Discovered entries already use the server UUID. Manual
                // bookmarks have their own UUID plus the authenticated server ID.
                workstation = (m_Computer->manualBookmark ?
                               m_Computer->serverUuid : m_Computer->uuid).toUtf8();
                slot = m_Computer->uuid.toUtf8();
            }
            const auto version = QCoreApplication::applicationVersion().toUtf8();
            PlankMetricsConfig output {};
            output.struct_size = sizeof(output);
            output.port = options.port;
            output.interval_seconds = options.intervalSeconds;
            output.workstation_id = workstation.constData();
            output.slot_id = slot.constData();
            output.version = version.isEmpty() ? "unknown" : version.constData();
            if (plank_transport_native_metrics_configure(endpoint, &output, &metrics) != PLANK_TRANSPORT_OK)
                qWarning("PLANK metrics unavailable; continuing the session");
        }
        result = plank_transport_native_endpoint_start(endpoint);
    }
    if (result == PLANK_TRANSPORT_OK) {
        const auto deadline = PlankClock::milliseconds() + 12000;
        do {
            if (m_Lifecycle.stopping()) { result = PLANK_TRANSPORT_ERROR_INVALID_STATE; break; }
            result = plank_transport_native_endpoint_wait_ready(endpoint, 100);
        } while (result == PLANK_TRANSPORT_TIMEOUT && PlankClock::milliseconds() < deadline);
    }
    if (result == PLANK_TRANSPORT_OK && m_MicrophoneNegotiated) {
        result = plank_transport_native_microphone_enable_version(endpoint, m_MicrophoneSchema);
    }
    if (result == PLANK_TRANSPORT_OK && m_CameraNegotiated &&
            plank_transport_native_camera_enable(endpoint, m_MicrophoneNegotiated ? 1 : 0) != PLANK_TRANSPORT_OK) {
        qWarning() << "PLANK camera lane is unavailable; other session media remain available";
    }
    if (result != PLANK_TRANSPORT_OK) {
        QByteArray error(512, '\0');
        if (endpoint != nullptr) {
            plank_transport_native_endpoint_last_error(
                        endpoint, error.data(), static_cast<size_t>(error.size()));
            plank_transport_native_endpoint_stop(endpoint);
            plank_transport_native_endpoint_destroy(endpoint);
        }
        plank_metrics_destroy(metrics);
        qWarning() << "Experimental plank_transport handshake failed:" << error.constData();
        return false;
    }

    m_PlankTransportEndpoint = endpoint;
    m_PlankDisconnectQueued = false;
    {
        std::lock_guard<std::mutex> lock(m_MetricsMutex);
        m_MetricsHandle = metrics;
    }
    LiSetPlankNativeControlSender(
                &Session::plankTransportNativeControlSender, endpoint);
    LiSetPlankNativeInputSender(
                &Session::plankTransportNativeInputSender, endpoint);
    qInfo() << "Experimental native KyProto media, input, and data protocols are ready on UDP"
            << port;
    return true;
#endif
}

bool Session::negotiatePlankTransportSession(quint16 sessionPort, QString& errorMessage)
{
#ifndef PLANK_TRANSPORT
    Q_UNUSED(sessionPort)
    errorMessage = tr("Native BDE fernweh session negotiation is unavailable.");
    return false;
#else
    if (m_PlankTransportEndpoint == nullptr) {
        errorMessage = tr("The native BDE fernweh transport is not connected.");
        return false;
    }

    const int negotiatedVideoFormat = m_StreamConfig.supportedVideoFormats;
    int codec = 0;
    int chroma = 1;
    bool tenBit = false;
    switch (negotiatedVideoFormat) {
    case VIDEO_FORMAT_H264:
        chroma = 0;
        break;
    case VIDEO_FORMAT_H264_HIGH8_422:
        chroma = 2;
        break;
    case VIDEO_FORMAT_H264_HIGH8_444:
        break;
    case VIDEO_FORMAT_H264_HIGH10_422:
        chroma = 2;
        tenBit = true;
        break;
    case VIDEO_FORMAT_H264_HIGH10_444:
        tenBit = true;
        break;
    case VIDEO_FORMAT_H265_REXT8_444:
        codec = 1;
        break;
    case VIDEO_FORMAT_H265_REXT10_444:
        codec = 1;
        tenBit = true;
        break;
    case VIDEO_FORMAT_H265_MAIN10:
        codec = 1;
        chroma = 0;
        tenBit = true;
        break;
    default:
        errorMessage = tr("The selected BDE fernweh video profile has no native negotiation mapping.");
        return false;
    }

    int slicesPerFrame = (m_VideoCallbacks.capabilities >> 24) & 0xFF;
    if (slicesPerFrame == 0) {
        slicesPerFrame = 1;
    }
    const bool supportsReferenceFrameInvalidation = codec == 0 ?
                (m_VideoCallbacks.capabilities &
                 CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC) != 0 :
                (m_VideoCallbacks.capabilities &
                 CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC) != 0;
    const int referenceFrames = supportsReferenceFrameInvalidation ? 0 : 1;
    const int audioChannels = CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(
                m_StreamConfig.audioConfiguration);
    const int audioChannelMask = CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(
                m_StreamConfig.audioConfiguration);
    const bool highQualityAudio =
            m_StreamConfig.bitrate >= 15000 &&
            (m_AudioCallbacks.capabilities & CAPABILITY_SLOW_OPUS_DECODER) == 0 &&
            (audioChannels <= 2 ||
             !NvComputer::isVpnReachability(
                 m_Computer->getActiveAddressReachability()));
    const int packetDurationMs =
            (m_AudioCallbacks.capabilities & CAPABILITY_SLOW_OPUS_DECODER) != 0 ?
                10 : 5;

    const QJsonObject request {
        {QStringLiteral("video"), QJsonObject {
            {QStringLiteral("width"), m_StreamConfig.width},
            {QStringLiteral("height"), m_StreamConfig.height},
            {QStringLiteral("fps"), m_StreamConfig.fps},
            {QStringLiteral("fps_x100"), m_StreamConfig.clientRefreshRateX100},
            {QStringLiteral("slices_per_frame"), slicesPerFrame},
            {QStringLiteral("reference_frames"), referenceFrames},
            {QStringLiteral("encoder_csc_mode"),
             (m_StreamConfig.colorSpace << 1) | m_StreamConfig.colorRange},
            {QStringLiteral("codec"), codec},
            {QStringLiteral("ten_bit"), tenBit},
            {QStringLiteral("chroma"), chroma},
            {QStringLiteral("intra_refresh"), 0},
            {QStringLiteral("encoder_target_kbps"), m_StreamConfig.bitrate},
            {QStringLiteral("negotiated_format"), negotiatedVideoFormat},
        }},
        {QStringLiteral("audio"), QJsonObject {
            {QStringLiteral("channels"), audioChannels},
            {QStringLiteral("channel_mask"), audioChannelMask},
            {QStringLiteral("packet_duration_ms"), packetDurationMs},
            {QStringLiteral("high_quality"), highQualityAudio},
        }},
    };
    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    std::vector<unsigned char> packet(
                PLANK_TRANSPORT_SETUP_HEADER_SIZE + static_cast<size_t>(payload.size()));
    size_t packetSize = 0;
    constexpr uint32_t RequestId = 1;
    if (plank_transport_setup_encode(
                PLANK_TRANSPORT_SETUP_LAUNCH_REQUEST, 0,
                PLANK_TRANSPORT_SETUP_STATUS_OK, RequestId,
                reinterpret_cast<const uint8_t*>(payload.constData()),
                static_cast<size_t>(payload.size()),
                packet.data(), packet.size(), &packetSize) != 0 ||
            plank_transport_native_data_send(
                m_PlankTransportEndpoint, packet.data(), packetSize) !=
                PLANK_TRANSPORT_OK) {
        errorMessage = tr("The native BDE fernweh launch request could not be sent.");
        return false;
    }

    packet.resize(PLANK_TRANSPORT_SETUP_MAX_PACKET_SIZE);
    packetSize = 0;
    const auto deadline = PlankClock::milliseconds() + 12000;
    int receiveResult;
    do {
        if (m_Lifecycle.stopping()) { receiveResult = PLANK_TRANSPORT_ERROR_INVALID_STATE; break; }
        receiveResult = plank_transport_native_data_receive(
                    m_PlankTransportEndpoint, packet.data(), packet.size(), &packetSize, 100);
    } while (receiveResult == PLANK_TRANSPORT_TIMEOUT && PlankClock::milliseconds() < deadline);
    if (receiveResult != PLANK_TRANSPORT_OK) {
        errorMessage = tr("The host did not complete native session negotiation.");
        return false;
    }

    PlankTransportSetupPacket responsePacket {};
    if (plank_transport_setup_decode(packet.data(), packetSize, &responsePacket) != 0 ||
            responsePacket.request_id != RequestId ||
            (responsePacket.flags & PLANK_TRANSPORT_SETUP_FLAG_RESPONSE) == 0 ||
            (responsePacket.type != PLANK_TRANSPORT_SETUP_LAUNCH_RESPONSE &&
             responsePacket.type != PLANK_TRANSPORT_SETUP_ERROR)) {
        errorMessage = tr("The host returned an invalid native session response.");
        return false;
    }

    QJsonParseError parseError {};
    const QJsonDocument responseDocument = QJsonDocument::fromJson(
                QByteArray(reinterpret_cast<const char*>(responsePacket.payload),
                           static_cast<int>(responsePacket.payload_size)),
                &parseError);
    if (parseError.error != QJsonParseError::NoError ||
            !responseDocument.isObject()) {
        errorMessage = tr("The host returned malformed native session values.");
        return false;
    }
    const QJsonObject response = responseDocument.object();
    if (responsePacket.status != PLANK_TRANSPORT_SETUP_STATUS_OK ||
            responsePacket.type == PLANK_TRANSPORT_SETUP_ERROR) {
        const QString hostMessage = response.value(QStringLiteral("message")).toString();
        errorMessage = hostMessage.isEmpty() ?
                    tr("The host rejected native session negotiation.") : hostMessage;
        return false;
    }

    const QJsonObject audio = response.value(QStringLiteral("audio")).toObject();
    const QJsonArray mapping = audio.value(QStringLiteral("mapping")).toArray();
    const int responseVideoFormat = response.value(
                QStringLiteral("video_format")).toInt();
    const int sampleRate = audio.value(QStringLiteral("sample_rate")).toInt();
    const int responseChannels = audio.value(QStringLiteral("channels")).toInt();
    const int streams = audio.value(QStringLiteral("streams")).toInt();
    const int coupledStreams = audio.value(QStringLiteral("coupled_streams")).toInt();
    const int responsePacketDuration = audio.value(
                QStringLiteral("packet_duration_ms")).toInt();
    int hostFeatureFlags = response.value(QStringLiteral("host_feature_flags")).toInt();
    if (isCollaborator()) {
        hostFeatureFlags &= ~(LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND | LI_FF_PEN_TOUCH_EVENTS);
    }
    const int referenceFrameInvalidation = response.value(
                QStringLiteral("reference_frame_invalidation")).toInt(-1);
    const QString fileClipboardMode = response.value(
                QStringLiteral("file_clipboard_mode")).toString();
    if (responseVideoFormat != negotiatedVideoFormat || sampleRate != 48000 ||
            responseChannels != audioChannels || responseChannels <= 0 ||
            responseChannels > AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT ||
            streams <= 0 || streams > responseChannels ||
            coupledStreams < 0 || coupledStreams > streams ||
            responsePacketDuration <= 0 || responsePacketDuration > 120 ||
            (hostFeatureFlags & LI_FF_LOCAL_CURSOR) == 0 ||
            (referenceFrameInvalidation != 0 && referenceFrameInvalidation != 1) ||
            fileClipboardMode != m_FileClipboardMode ||
            mapping.size() != responseChannels) {
        errorMessage = tr("The host returned unsupported native audio or video values.");
        return false;
    }

    PLANK_NATIVE_SESSION_CONFIGURATION nativeConfiguration {};
    nativeConfiguration.structSize = sizeof(nativeConfiguration);
    nativeConfiguration.serviceFlags = PLANK_NATIVE_SERVICE_AUDIO |
            PLANK_NATIVE_SERVICE_INPUT | PLANK_NATIVE_SERVICE_LOCAL_CURSOR;
    nativeConfiguration.negotiatedVideoFormat = responseVideoFormat;
    nativeConfiguration.hostFeatureFlags =
            static_cast<unsigned int>(hostFeatureFlags);
    nativeConfiguration.audioPacketDurationMs = responsePacketDuration;
    nativeConfiguration.referenceFrameInvalidationSupported =
            static_cast<unsigned int>(referenceFrameInvalidation);
    nativeConfiguration.sessionPort = sessionPort;
    nativeConfiguration.opusConfiguration.sampleRate = sampleRate;
    nativeConfiguration.opusConfiguration.channelCount = responseChannels;
    nativeConfiguration.opusConfiguration.streams = streams;
    nativeConfiguration.opusConfiguration.coupledStreams = coupledStreams;
    for (int index = 0; index < responseChannels; ++index) {
        const int channel = mapping.at(index).toInt(-1);
        if (channel < 0 || channel >= responseChannels) {
            errorMessage = tr("The host returned an invalid native audio channel map.");
            return false;
        }
        nativeConfiguration.opusConfiguration.mapping[index] =
                static_cast<unsigned char>(channel);
    }
    if (LiSetPlankNativeSessionConfiguration(&nativeConfiguration) != 0) {
        errorMessage = tr("The native BDE fernweh session values were rejected locally.");
        return false;
    }

    qInfo() << "Native QUIC session negotiated: video format"
            << QString::number(responseVideoFormat, 16)
            << "host features" << QString::number(hostFeatureFlags, 16)
            << "audio channels" << responseChannels
            << "packet duration" << responsePacketDuration << "ms";
    return true;
#endif
}

void Session::publishPenMetrics(const PlankPenActivity& activity)
{
#ifdef PLANK_TRANSPORT
    std::unique_lock<std::mutex> lock(m_MetricsMutex, std::try_to_lock);
    if (!lock || !m_MetricsHandle) return;
    PlankMetricsPenActivity sample {};
    sample.struct_size = sizeof(sample);
    sample.observed = activity.observed ? 1 : 0;
    sample.motion_events = activity.motion;
    sample.tip_down_events = activity.tipDown;
    plank_metrics_pen_activity(m_MetricsHandle, &sample);
#else
    Q_UNUSED(activity)
#endif
}

bool Session::endStreamBenchmark()
{
    if (!m_MeasureStreamBenchmark || s_ActiveSession != this || m_Lifecycle.stopping()) return false;
    requestSessionQuit(SessionDiagnostics::End::QuitEvent);
    return true;
}

void Session::publishVideoMetrics(const VIDEO_STATS& window, bool hardwareDecoder, quint32 elapsedMs)
{
    if (!elapsedMs) return;
    const double seconds = elapsedMs / 1000.0;
    if (m_MeasureStreamBenchmark) {
        const auto fec = currentVideoFecLoss();
        emit streamBenchmarkSample(QJsonObject{
            {"seconds", seconds}, {"received", qint64(window.receivedFrames)},
            {"freshCaptures", qint64(window.framesWithHostProcessingLatency)},
            {"decoded", qint64(window.decodedFrames)}, {"presented", qint64(window.renderedFrames)},
            {"missing", qint64(window.networkDroppedFrames)}, {"bytes", qint64(window.receivedVideoBytes)},
            {"decodeMs", double(window.totalDecodeTime)}, {"renderMs", double(window.totalRenderTime)},
            {"rtt", qint64(currentNetworkRttMs())}, {"hardware", hardwareDecoder},
            {"fecBefore", double(fec.before)}, {"fecAfter", double(fec.after)}
        });
    }
#ifdef PLANK_TRANSPORT
    std::unique_lock<std::mutex> lock(m_MetricsMutex, std::try_to_lock);
    if (!lock || !m_MetricsHandle) return;
    PlankMetricsClientVideo sample {};
    sample.struct_size = sizeof(sample);
    sample.hardware_decoder = hardwareDecoder ? 1 : 0;
    sample.window_seconds = seconds;
    sample.received_frames = window.receivedFrames;
    sample.decoded_frames = window.decodedFrames;
    sample.rendered_frames = window.renderedFrames;
    sample.network_dropped_frames = window.networkDroppedFrames;
    sample.render_dropped_frames = window.renderQueueDroppedFrames;
    sample.overflow_dropped_frames = window.queueOverflowDroppedFrames;
    sample.decode_time_ms = window.totalDecodeTime;
    sample.render_queue_time_ms = window.totalPacerTime;
    sample.render_time_ms = window.totalRenderTime;
    sample.host_latency_samples = window.framesWithHostProcessingLatency;
    sample.host_latency_time_ms = double(window.totalHostProcessingLatency) / 10.0;
    // A busy or retired metrics sink only loses this observation, never video.
    plank_metrics_client_video(m_MetricsHandle, &sample);
#endif
}

void Session::queuePlankTransportDisconnect()
{
#ifdef PLANK_TRANSPORT
    if (m_PlankTransportEndpoint == nullptr || m_PlankDisconnectQueued) return;
    unsigned char packet[PLANK_TRANSPORT_CONTROL_MAX_PACKET_SIZE];
    size_t packetSize = 0;
    if (plank_transport_control_encode(PLANK_TRANSPORT_CONTROL_CLIENT_DISCONNECT, nullptr, 0,
                                       packet, sizeof(packet), &packetSize) == 0) {
        const int result = plank_transport_native_data_send(m_PlankTransportEndpoint, packet, packetSize);
        m_PlankDisconnectQueued = result == PLANK_TRANSPORT_OK;
        qInfo() << "Native disconnect queue result:" << result;
    }
#endif
}

void Session::stopPlankTransportDataPlane()
{
#ifdef PLANK_TRANSPORT
    {
        std::lock_guard<std::mutex> lock(m_MetricsMutex);
        plank_metrics_destroy(m_MetricsHandle);
        m_MetricsHandle = nullptr;
    }
    queuePlankTransportDisconnect();
    stopPlankTransportMediaReceivers();
    {
        std::lock_guard<std::mutex> guard(m_MicrophoneMutex);
        m_Microphone.reset();
    }
    {
        std::lock_guard<std::mutex> guard(m_CameraMutex);
        m_Camera.reset(); m_CameraRequested.store(false);
    }
    LiSetPlankNativeControlSender(nullptr, nullptr);
    LiSetPlankNativeInputSender(nullptr, nullptr);
    if (m_PlankTransportEndpoint != nullptr) {
        PlankTransportNativeStats stats {};
        stats.struct_size = sizeof(stats);
        if (plank_transport_native_endpoint_stats(m_PlankTransportEndpoint, &stats) ==
                PLANK_TRANSPORT_OK) {
            qInfo() << "PlankTransport native transport: video-frames="
                    << stats.video_frames_received
                    << "video-bytes=" << stats.video_bytes_received
                    << "video-receive-drops=" << stats.video_receive_drops
                    << "audio-packets=" << stats.audio_packets_received
                    << "audio-bytes=" << stats.audio_bytes_received
                    << "audio-receive-drops=" << stats.audio_receive_drops
                    << "input-sent=" << stats.input_packets_sent
                    << "data-sent=" << stats.data_packets_sent
                    << "data-received=" << stats.data_packets_received
                    << "QUIC-lost=" << stats.quic_packets_lost
                    << "QUIC-RTT-us=" << stats.quic_rtt_us
                    << "KyProto-drops=" << stats.kyproto_packets_dropped
                    << "video-FEC-source-symbols="
                    << stats.video_fec_source_symbols
                    << "video-FEC-source-symbols-missing="
                    << stats.video_fec_source_symbols_missing
                    << "video-FEC-source-symbols-unrecovered="
                    << stats.video_fec_source_symbols_unrecovered;
        }
        QByteArray lastError(512, '\0');
        plank_transport_native_endpoint_last_error(
                    m_PlankTransportEndpoint, lastError.data(),
                    static_cast<size_t>(lastError.size()));
        if (!lastError.isEmpty() && lastError.constData()[0] != '\0') {
            qWarning() << "PlankTransport native endpoint ended:"
                       << lastError.constData();
        }
        plank_transport_native_endpoint_stop(m_PlankTransportEndpoint);
        plank_transport_native_endpoint_destroy(m_PlankTransportEndpoint);
        m_PlankTransportEndpoint = nullptr;
        qInfo() << "Experimental native KyProto connection stopped";
    }
#endif
}

void Session::logDiagnosticVideoPipeline() const
{
    if (m_Reconnecting.load() || m_ReconnectRequested.load() ||
            m_LastDiagnosticStats.load() == 0 || diagnosticReceivedAgeMs() >= 5000 ||
            diagnosticStatisticsAgeMs() <= 10000) return;
    const auto now = PlankClock::milliseconds();
    const auto decode = PlankVideoPipeline::read(PlankVideoPipeline::decodeOperation, now);
    const auto render = PlankVideoPipeline::read(PlankVideoPipeline::renderOperation, now);
    // Called by the existing five-second diagnostics timer. No decoder, queue
    // or GPU locks, and no pointer to an object that reset may be destroying.
    qWarning() << "Client video pipeline stalled-statistics snapshot: decode="
               << PlankVideoPipeline::name(decode.stage) << "decode-age-ms=" << decode.ageMs
               << "decoded-frames=" << PlankVideoPipeline::decodedFrames.load(std::memory_order_relaxed)
               << "render=" << PlankVideoPipeline::name(render.stage) << "render-age-ms=" << render.ageMs
               << "renderer-returns=" << PlankVideoPipeline::rendererReturns.load(std::memory_order_relaxed)
               << "received-age-ms=" << diagnosticReceivedAgeMs()
               << "statistics-age-ms=" << diagnosticStatisticsAgeMs();
}

#ifdef PLANK_TRANSPORT
void Session::startPlankTransportMediaReceivers()
{
    stopPlankTransportMediaReceivers();
    if (m_PlankTransportEndpoint == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_VideoPacketLossSamplesLock);
        m_VideoPacketLossPeakWindow.reset();
        m_VideoPacketLossAfterFecPeakWindow.reset();
        m_CurrentVideoFecLoss = {};
    }
    m_CurrentNetworkRttMs.store(0, std::memory_order_relaxed);
    m_LastPlankVideoReceived.store(0);
    m_LastDiagnosticStats.store(0);
    m_CurrentRenderedFps.store(0, std::memory_order_relaxed);
    m_CurrentVideoMbps.store(0, std::memory_order_relaxed);
    m_PlankTransportReceiversStopping.store(false);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    startClipboardSync();
#endif
    m_PlankTransportVideoThread = std::thread([this]() {
        plankTransportVideoReceiveLoop();
    });
    if (LiGetPlankNativeServiceFlags() & PLANK_NATIVE_SERVICE_AUDIO) {
        m_PlankTransportAudioThread = std::thread([this]() {
            plankTransportAudioReceiveLoop();
        });
    }
    m_PlankTransportDataThread = std::thread([this]() {
        plankTransportDataReceiveLoop();
    });
}

void Session::stopPlankTransportMediaReceivers()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    stopClipboardPollTimer();
#endif
    m_PlankTransportReceiversStopping.store(true);
    if (m_PlankTransportVideoThread.joinable()) {
        m_PlankTransportVideoThread.join();
    }
    if (m_PlankTransportAudioThread.joinable()) {
        m_PlankTransportAudioThread.join();
    }
    if (m_PlankTransportDataThread.joinable()) {
        m_PlankTransportDataThread.join();
    }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    stopClipboardSync();
#endif
}

void Session::plankTransportVideoReceiveLoop()
{
    ClientFrameFlowTrace frameFlow("receive");
    constexpr size_t InitialFrameCapacity = 1024 * 1024;
    constexpr size_t MaximumFrameCapacity = 64 * 1024 * 1024;
    using VideoBuffer = std::unique_ptr<PLANK_VIDEO_BUFFER, decltype(&LiReleaseVideoBuffer)>;
    size_t capacity = InitialFrameCapacity;
    VideoBuffer frame(LiAllocateVideoBuffer(int(capacity)), LiReleaseVideoBuffer);
    VideoPacketLossInterval packetLossInterval;
    quint64 nextPacketLossSample = PlankClock::milliseconds();

    const auto sampleTransportTelemetry = [this, &packetLossInterval,
                                           &nextPacketLossSample]() {
        const quint64 now = PlankClock::milliseconds();
        if (now < nextPacketLossSample) {
            return;
        }
        nextPacketLossSample = now + 1000;

        PlankTransportNativeStats stats {};
        stats.struct_size = sizeof(stats);
        if (plank_transport_native_endpoint_stats(m_PlankTransportEndpoint, &stats) !=
                PLANK_TRANSPORT_OK) {
            return;
        }

        const std::uint64_t roundedRttMs =
                (stats.quic_rtt_us + 500) / 1000;
        m_CurrentNetworkRttMs.store(
                    static_cast<std::uint32_t>(std::min<std::uint64_t>(
                        roundedRttMs,
                        std::numeric_limits<std::uint32_t>::max())),
                    std::memory_order_relaxed);

        const auto packetLossPercent = packetLossInterval.addCumulative(
                    stats.video_fec_source_symbols,
                    stats.video_fec_source_symbols_missing,
                    stats.video_fec_source_symbols_unrecovered);
        if (packetLossPercent.has_value()) {
            updateVideoFecLoss(*packetLossPercent);
        }
    };

    while (!m_PlankTransportReceiversStopping.load()) {
        if (!frame) {
            qCritical("Unable to allocate encoded video buffer");
            Session::requestSessionQuit();
            return;
        }
        sampleTransportTelemetry();
        PlankTransportNativeVideoFrameInfo info {};
        info.struct_size = sizeof(info);
        size_t frameSize = 0;
        const int result = plank_transport_native_video_receive(
                    m_PlankTransportEndpoint, &info, LiVideoBufferData(frame.get()), capacity,
                    &frameSize, 50);
        if (result == PLANK_TRANSPORT_TIMEOUT) {
            continue;
        }
        if (result == PLANK_TRANSPORT_ERROR_BUFFER_TOO_SMALL &&
                frameSize > capacity &&
                frameSize <= MaximumFrameCapacity) {
            capacity = frameSize;
            frame.reset(LiAllocateVideoBuffer(int(capacity)));
            continue;
        }
        if (result != PLANK_TRANSPORT_OK) {
            if (!m_PlankTransportReceiversStopping.load()) {
                qWarning() << "Native KyProto video receive failed:" << result;
            }
            return;
        }
        if (frameSize > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                info.frame_number > std::numeric_limits<uint32_t>::max()) {
            qWarning() << "Native KyProto video frame metadata is out of range";
            LiRequestIdrFrame();
            continue;
        }

        m_Diagnostics.videoReceived();
        m_LastPlankVideoReceived.store(PlankClock::milliseconds());
        frameFlow.record(ClientFrameFlowTrace::Receive,
                         (info.pts / 90000) * 1000000 + (info.pts % 90000) * 1000000 / 90000,
                         info.frame_number,
                         (info.flags & PLANK_TRANSPORT_NATIVE_VIDEO_FLAG_KEY) != 0,
                         frameSize);
        const uint32_t flags =
                (info.flags & PLANK_TRANSPORT_NATIVE_VIDEO_FLAG_KEY) != 0 ?
                    PLANK_VIDEO_FRAME_FLAG_KEY : 0;
        const int submitResult = LiSubmitPlankVideoFrame(
                    frame.get(), static_cast<int>(frameSize),
                    static_cast<uint32_t>(info.frame_number), flags,
                    info.pts, info.host_processing_latency);
        // The assembler/decoder retain this frame. Never overwrite their bytes.
        capacity = std::max(InitialFrameCapacity, frameSize);
        frame.reset(LiAllocateVideoBuffer(int(capacity)));
        if (submitResult < 0) {
            qWarning() << "Native KyProto video frame submission failed:"
                       << submitResult << "frame" << info.frame_number;
            LiRequestIdrFrame();
        }
    }
}

void Session::plankTransportAudioReceiveLoop()
{
    constexpr size_t MaximumAudioPacketSize = 64 * 1024;
    std::vector<unsigned char> packet(MaximumAudioPacketSize);

    while (!m_PlankTransportReceiversStopping.load()) {
        PlankTransportNativeAudioPacketInfo info {};
        info.struct_size = sizeof(info);
        size_t packetSize = 0;
        const int result = plank_transport_native_audio_receive(
                    m_PlankTransportEndpoint, &info, packet.data(), packet.size(),
                    &packetSize, 50);
        if (result == PLANK_TRANSPORT_TIMEOUT) {
            continue;
        }
        if (result != PLANK_TRANSPORT_OK ||
                packetSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
            if (!m_PlankTransportReceiversStopping.load()) {
                qWarning() << "Native KyProto audio receive failed:" << result;
            }
            return;
        }

        const int submitResult = LiSubmitPlankAudioPacket(
                    packetSize == 0 ? nullptr : packet.data(),
                    static_cast<int>(packetSize), info.frame_samples,
                    info.missing_samples, info.pts);
        if (submitResult < 0) {
            qWarning() << "Native KyProto audio packet submission failed:"
                       << submitResult;
        }
    }
}

void Session::plankTransportDataReceiveLoop()
{
    std::vector<unsigned char> packet(PLANK_TRANSPORT_EVENT_MAX_PACKET_SIZE);
    while (!m_PlankTransportReceiversStopping.load()) {
        size_t packetSize = 0;
        const int result = plank_transport_native_data_receive(
                    m_PlankTransportEndpoint, packet.data(), packet.size(),
                    &packetSize, 50);
        if (result == PLANK_TRANSPORT_TIMEOUT) {
            continue;
        }
        if (result != PLANK_TRANSPORT_OK) {
            if (!m_PlankTransportReceiversStopping.load()) {
                qWarning() << "Native KyProto control receive failed:" << result;
                // This receiver owns termination for the shared QUIC connection.
                // Drain queued control records first (notably session takeover),
                // then use the existing duplicate-safe reconnect callback. A
                // closed receive path must not wait for a mouse/key send failure.
                clConnectionTerminated(result);
            }
            return;
        }

        if (packetSize < sizeof(uint32_t)) {
            qWarning() << "Rejected undersized native KyProto data record";
            LiNotifyPlankHostTermination(-1);
            return;
        }
        const uint32_t magic = plank_transport_event_read_u32(packet.data());
        if (magic == PLANK_TRANSPORT_CONTROL_MAGIC) {
            PlankTransportControlPacket control {};
            if (plank_transport_control_decode(
                        packet.data(), packetSize, &control) != 0) {
                qWarning() << "Rejected malformed native KyProto control packet";
                LiNotifyPlankHostTermination(-1);
                return;
            }
            switch (control.type) {
            case PLANK_TRANSPORT_CONTROL_COLLABORATION_PERMISSION: {
                const int permission = PlankCollaboration::wirePermission(control.payload, control.payload_size);
                if (!isCollaborator() || permission < 0) {
                    LiNotifyPlankHostTermination(-1);
                    return;
                }
                if (permission == PLANK_TRANSPORT_COLLABORATION_REVOKED) {
                    m_CollaborationPermission.store(PlankCollaboration::View);
                    m_CanReconnect.store(false);
                    LiNotifyPlankHostTermination(PLANK_TRANSPORT_TERMINATION_GRACEFUL);
                    return;
                }
                if (m_CollaborationPermission.exchange(permission) != permission) {
                    // The transport sender observes a downgrade immediately;
                    // SDL input capture and all presentation UI follow on the
                    // main event thread, including already queued input.
#ifdef PLANK_QT_SESSION
                    emit qtCollaborationChanged();
#else
                    SDL_Event event {};
                    event.type = SDL_EVENT_USER;
                    event.user.code = SDL_CODE_PLANK_COLLABORATION_PERMISSION;
                    SDL_PushEvent(&event);
#endif
                }
                break;
            }
            case PLANK_TRANSPORT_CONTROL_CAMERA_APPLIED:
            case PLANK_TRANSPORT_CONTROL_CAMERA_KEYFRAME: {
                std::uint64_t generation = 0; std::uint32_t value = 0;
                if (!m_CameraNegotiated || plank_camera_control_decode(&control, &generation, &value)) {
                    LiNotifyPlankHostTermination(-1); return;
                }
                std::lock_guard<std::mutex> guard(m_CameraMutex);
                if (m_Camera) {
                    if (control.type == PLANK_TRANSPORT_CONTROL_CAMERA_APPLIED) m_Camera->acknowledge(generation, value);
                    else m_Camera->requestKeyframe(generation);
                }
                break;
            }
            case PLANK_TRANSPORT_CONTROL_MICROPHONE_APPLIED: {
                if (!m_MicrophoneNegotiated || control.payload_size != 12 ||
                        plank_transport_control_read_u32(control.payload + 8) > PLANK_TRANSPORT_MICROPHONE_UNAVAILABLE) {
                    LiNotifyPlankHostTermination(-1); return;
                }
                const uint64_t generation = uint64_t(plank_transport_control_read_u32(control.payload)) << 32 |
                        plank_transport_control_read_u32(control.payload + 4);
                std::lock_guard<std::mutex> guard(m_MicrophoneMutex);
                if (m_Microphone) m_Microphone->acknowledge(generation, plank_transport_control_read_u32(control.payload + 8));
                break;
            }
            case PLANK_TRANSPORT_CONTROL_HOST_DESKTOP_HANDOFF:
                if (control.payload_size != 0 ||
                        !(m_Computer->plankFeatureFlags & NvOutputTopology::DesktopHandoffNoticeFeature)) {
                    LiNotifyPlankHostTermination(-1);
                    return;
                }
                // Advisory only. Do not reconnect until the transport actually
                // closes; a stale notice must not relabel an unrelated outage.
                m_DesktopHandoffNoticeDeadline.store(PlankClock::milliseconds() + 5000);
                qInfo() << "Host announced GDM-to-desktop handoff";
                break;
            case PLANK_TRANSPORT_CONTROL_VIDEO_BITRATE_APPLIED:
                if (control.payload_size != 3 * sizeof(uint32_t)) {
                    LiNotifyPlankHostTermination(-1);
                    return;
                }
                LiNotifyPlankVideoBitrateApplied(
                            plank_transport_control_read_u32(control.payload),
                            plank_transport_control_read_u32(control.payload + 4),
                            plank_transport_control_read_u32(control.payload + 8));
                break;
            case PLANK_TRANSPORT_CONTROL_HOST_TERMINATE:
                if (control.payload_size != sizeof(uint32_t)) {
                    LiNotifyPlankHostTermination(-1);
                    return;
                }
                LiNotifyPlankHostTermination(
                            plank_transport_control_read_u32(control.payload));
                return;
            default:
                qWarning() << "Rejected unexpected native KyProto control type"
                           << control.type;
                LiNotifyPlankHostTermination(-1);
                return;
            }
            continue;
        }

        PlankTransportEventPacket event {};
        if (magic != PLANK_TRANSPORT_EVENT_MAGIC ||
                plank_transport_event_decode(
                    packet.data(), packetSize, &event) != 0) {
            qWarning() << "Rejected malformed native KyProto event record";
            LiNotifyPlankHostTermination(-1);
            return;
        }
        switch (event.type) {
        case PLANK_TRANSPORT_EVENT_RAW_HID_WACOM:
            if (event.payload_size < sizeof(PLANK_RAW_HID_WIRE_HEADER) ||
                    event.payload_size > sizeof(PLANK_RAW_HID_WIRE_HEADER) +
                        PLANK_RAW_HID_MAX_PAYLOAD_SIZE) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
            LiNotifyPlankRawHidControl(
                        event.payload, event.payload_size);
            break;
        case PLANK_TRANSPORT_EVENT_CURSOR_SHAPE:
            if (event.payload_size < sizeof(PLANK_CURSOR_WIRE_HEADER) ||
                    event.payload_size > sizeof(PLANK_CURSOR_WIRE_HEADER) +
                        PLANK_CURSOR_MAX_CHUNK_SIZE) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
            LiNotifyPlankCursorChunk(
                        event.payload, event.payload_size);
            break;
        case PLANK_TRANSPORT_EVENT_CURSOR_POSITION:
            if (event.payload_size != sizeof(PLANK_CURSOR_POSITION_WIRE_MESSAGE)) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
            LiNotifyPlankCursorPosition(
                        event.payload, event.payload_size);
            break;
        case PLANK_TRANSPORT_EVENT_POINTER_CAPTURE:
#ifdef PLANK_QT_SESSION
            m_QtInputCallbacks.invoke([&](QtSessionInput& input) {
                input.handlePointerCapture(event.payload, event.payload_size);
            });
#else
            m_InputCallbacks.invoke([&](SdlInputHandler& input) {
                input.handlePointerCapture(event.payload, event.payload_size);
            });
#endif
            break;
        case PLANK_TRANSPORT_EVENT_CLIPBOARD_OFFER:
            if (event.payload_size < sizeof(PLANK_CLIPBOARD_WIRE_HEADER) ||
                    event.payload_size > sizeof(PLANK_CLIPBOARD_WIRE_HEADER) +
                        PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
            if (m_ClipboardSync && clipboardSyncEnabled() &&
                    !m_ClipboardSync->handleHostOffer(event.payload, event.payload_size)) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
#endif
            break;
        default:
            qWarning() << "Rejected unexpected native KyProto event type"
                       << event.type;
            LiNotifyPlankHostTermination(-1);
            return;
        }
    }
}

int Session::plankTransportNativeControlSender(void* context, uint32_t type,
                                          uint32_t value1, uint32_t value2)
{
    if (context == nullptr) {
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    uint16_t wireType = 0;
    uint32_t values[2] {value1, value2};
    size_t valueCount = 0;
    switch (type) {
    case LI_SC_NATIVE_CONTROL_REQUEST_IDR:
        wireType = PLANK_TRANSPORT_CONTROL_REQUEST_IDR;
        break;
    case LI_SC_NATIVE_CONTROL_INVALIDATE_REFERENCE_FRAMES:
        wireType = PLANK_TRANSPORT_CONTROL_INVALIDATE_REFERENCE_FRAMES;
        valueCount = 2;
        break;
    case LI_SC_NATIVE_CONTROL_SET_VIDEO_BITRATE:
        wireType = PLANK_TRANSPORT_CONTROL_SET_VIDEO_BITRATE;
        valueCount = 1;
        break;
    default:
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }

    unsigned char packet[PLANK_TRANSPORT_CONTROL_MAX_PACKET_SIZE];
    size_t packetSize = 0;
    if (plank_transport_control_encode(
                wireType, values, valueCount, packet, sizeof(packet),
                &packetSize) != 0) {
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    return plank_transport_native_data_send(
                static_cast<PlankTransportNativeEndpoint*>(context), packet,
                packetSize);
}

int Session::plankTransportNativeInputSender(void* context, uint8_t type,
                                        const unsigned char* payload,
                                        size_t payloadLength)
{
    if (context == nullptr || payload == nullptr || payloadLength == 0) {
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    if (s_ActiveSession != nullptr && !PlankCollaboration::allowsInput(
                static_cast<PlankCollaboration::Permission>(s_ActiveSession->m_CollaborationPermission.load()), type)) {
        return PLANK_TRANSPORT_OK;
    }
    return plank_transport_native_input_send(
                static_cast<PlankTransportNativeEndpoint*>(context), type,
                payload, payloadLength);
}
#endif

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
bool Session::clipboardSyncEnabled() const
{
    return m_Computer != nullptr &&
            !isCollaborator() && m_SessionClipboardEntitled &&
            (!(m_Computer->plankFeatureFlags & NvOutputTopology::FixedCaptureFeature) || m_MacClipboardNegotiated) &&
            (m_EffectiveFeatureFlags & NvOutputTopology::ClipboardSyncFeature) != 0;
}

void Session::createClipboardSync()
{
#ifdef PLANK_TRANSPORT
    if (!m_ClipboardSync) {
        m_ClipboardSync = std::make_unique<ClipboardSync>(
            [this](const std::uint8_t* payload, std::size_t size) {
                return plankTransportNativeInputSender(m_PlankTransportEndpoint,
                    PLANK_TRANSPORT_INPUT_CLIPBOARD_OFFER, payload, size) == PLANK_TRANSPORT_OK;
            },
            [this] { return anyPresentationWindowFocused(); },
            [this] { return clipboardSyncEnabled(); },
            [this] { queueClipboardEvent(SDL_CODE_PLANK_CLIPBOARD); return true; });
    }
#endif
}

void Session::startClipboardSync()
{
#ifdef PLANK_TRANSPORT
    if (!clipboardSyncEnabled() || m_PlankTransportEndpoint == nullptr) {
        return;
    }
    createClipboardSync();
    m_ClipboardSync->start();
#ifdef PLANK_TRANSPORT
    if (m_FileClipboardMode != QStringLiteral("off") && !fileClipboard()) {
        const auto generation = ++m_ClipboardEventGeneration;
        auto files = std::make_shared<PlatformFileClipboard>(
                    m_PlankTransportEndpoint,
                    m_FileClipboardMode.toStdString(),
                    [this, generation] {
                        queueClipboardEvent(SDL_CODE_PLANK_FILE_CLIPBOARD_READY, generation);
                    },
                    [this, generation] {
                        queueClipboardEvent(SDL_CODE_PLANK_FILE_CLIPBOARD_PUBLISH, generation);
                        return true;
                    }
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
                    , [this] { return anyPresentationWindowFocused(); }
#endif
                    );
        std::atomic_store(&m_FileClipboard, std::move(files));
    }
    if (const auto files = fileClipboard()) files->start();
#endif
    qInfo() << "Started PLANK clipboard sync";
#endif
}

void Session::stopClipboardSync()
{
    stopClipboardPollTimer();
    m_TextClipboardPastePending = false;
    ++m_ClipboardEventGeneration;
    if (m_ClipboardSync) {
        m_ClipboardSync->stop();
    }
#ifdef PLANK_TRANSPORT
    if (const auto files = std::atomic_exchange(&m_FileClipboard, std::shared_ptr<FileClipboard>()))
        files->stop();
#endif
}

#ifdef PLANK_TRANSPORT
bool Session::deferTextClipboardPasteOnMainThread()
{
    if (m_ClipboardSync && clipboardSyncEnabled() && anyPresentationWindowFocused() &&
            !m_ClipboardSync->pollLocalClipboardOnMainThread()) {
        m_TextClipboardPastePending = true;
        return true;
    }
    return false;
}

bool Session::beginFileClipboardPasteOnMainThread()
{
    const auto files = fileClipboard();
    return files && clipboardSyncEnabled() && anyPresentationWindowFocused() &&
            files->beginPasteOnMainThread();
}

void Session::injectRemoteFilePasteOnMainThread()
{
    constexpr short LeftControl = 0xA2;
    constexpr short V = 0x56;
    LiSendKeyboardEvent(LeftControl, KEY_ACTION_DOWN, MODIFIER_CTRL);
    LiSendKeyboardEvent(V, KEY_ACTION_DOWN, MODIFIER_CTRL);
    LiSendKeyboardEvent(V, KEY_ACTION_UP, MODIFIER_CTRL);
    LiSendKeyboardEvent(LeftControl, KEY_ACTION_UP, 0);
}
#endif

void Session::queueClipboardEvent(int code, std::uintptr_t generation)
{
    if (usesQtPresentation()) {
        QMetaObject::invokeMethod(this, [this, code, generation] {
            dispatchClipboardEvent(code, generation);
        }, Qt::QueuedConnection);
        return;
    }
    SDL_Event event {};
    event.type = SDL_EVENT_USER;
    event.user.code = code;
    event.user.data1 = reinterpret_cast<void*>(generation);
    SDL_PushEvent(&event);
}

void Session::pollClipboardOnMainThread()
{
    if (m_ClipboardSync) {
        m_ClipboardSync->applyPendingHostTextOnMainThread();
        const bool ready = m_ClipboardSync->pollLocalClipboardOnMainThread();
        if (m_TextClipboardPastePending) {
            if (!clipboardSyncEnabled() || !anyPresentationWindowFocused()) m_TextClipboardPastePending = false;
            else if (ready) {
                m_TextClipboardPastePending = false;
                injectRemoteFilePasteOnMainThread();
            }
        }
    }
#ifdef PLANK_TRANSPORT
    if (const auto files = fileClipboard()) {
        files->pollLocalClipboardOnMainThread();
        files->publishPendingHostFilesOnMainThread();
    }
#endif
}

void Session::dispatchClipboardEvent(int code, std::uintptr_t generation)
{
    if (code == SDL_CODE_PLANK_CLIPBOARD_POLL) { pollClipboardOnMainThread(); return; }
    if (code == SDL_CODE_PLANK_CLIPBOARD) {
        if (m_ClipboardSync) m_ClipboardSync->applyPendingHostTextOnMainThread();
        return;
    }
#ifdef PLANK_TRANSPORT
    if (generation != m_ClipboardEventGeneration) return;
    const auto files = fileClipboard();
    if (!files) return;
    if (code == SDL_CODE_PLANK_FILE_CLIPBOARD_READY && clipboardSyncEnabled() &&
        anyPresentationWindowFocused() && files->takeReadyPasteOnMainThread())
        injectRemoteFilePasteOnMainThread();
    if (code == SDL_CODE_PLANK_FILE_CLIPBOARD_PUBLISH) files->publishPendingHostFilesOnMainThread();
#endif
}

void Session::queueClipboardPollEvent()
{
    queueClipboardEvent(SDL_CODE_PLANK_CLIPBOARD_POLL);
}

void Session::startClipboardPollTimer()
{
    if (usesQtPresentation()) return; // The GUI-owned Qt session timer polls.
    if (m_ClipboardSync && !m_ClipboardPollTimer.start(SDL_CODE_PLANK_CLIPBOARD_POLL))
        qWarning() << "Failed to start clipboard poll timer:" << SDL_GetError();
}

void Session::stopClipboardPollTimer()
{
    m_ClipboardPollTimer.stop();
}

#endif

void Session::clearPlankReconnectCredentials()
{
    m_CanReconnect.store(false);
    m_PlankPassword.fill(QChar('\0'));
    m_PlankPassword.clear();
    m_PlankUsername.clear();
    m_PlankBrokerAdmission = nullptr;
}

void Session::setPlankBrokerAdmission(PlankBrokerAdmission admission)
{
    m_PlankBrokerAdmission = std::move(admission);
    if (m_Computer->plankAuthentication && m_PlankBrokerAdmission) {
        m_CanReconnect.store(true);
    }
}

void Session::setAuthenticatedGreeter(bool confirmed)
{
    m_AuthenticatedGreeter.store(confirmed);
}

bool Session::isPlankBrokered() const
{
    QReadLocker lock(&m_Computer->lock);
    return !m_Computer->brokerHostCertSha256.isEmpty();
}

bool Session::hasPlankCredentials() const
{
    if (isPlankBrokered()) {
        return static_cast<bool>(m_PlankBrokerAdmission);
    }
    return !m_PlankUsername.isEmpty() && !m_PlankPassword.isEmpty();
}

QString Session::authenticatePlank(NvHTTP& http, bool* greeterConfirmed)
{
    if (!isPlankBrokered()) {
        return http.authenticate(m_PlankUsername, m_PlankPassword, greeterConfirmed);
    }
    if (!m_PlankBrokerAdmission) {
        throw GfeHttpResponseException(401, "Reconnect to the remote workstation from the Remote list");
    }

    // Never reuse a broker token: each admission is a new broker connect,
    // which may also lease a different session port.
    PlankBroker::Admission admission;
    m_PlankBrokerAdmission(admission);
    NvAddress address;
    QString pin;
    {
        QReadLocker lock(&m_Computer->lock);
        address = m_Computer->activeAddress;
        pin = m_Computer->brokerHostCertSha256;
    }
    http.setAddress(address);
    http.setPinnedCertificateSha256(pin);
    return http.authenticateBroker(std::move(admission), greeterConfirmed);
}

bool Session::initialize(IFFmpegRendererFactory* presenter)
{
    SDL_Window* testWindow = nullptr;
    bool initializedSdlVideo = false;
    bool keepSdlVideo = false;
    const auto cleanupProbe = qScopeGuard([&] {
        if (testWindow) SDL_DestroyWindow(testWindow);
        if (initializedSdlVideo && !keepSdlVideo) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    });
    {
        QReadLocker lock(&m_Computer->lock);
        m_PlankLaunchFeatureFlags = m_Computer->outputTopology.launchFeatureFlags(
                    m_Computer->plankFeatureFlags);
    }
    m_RendererFactory = presenter;

    if (!StreamingPreferences::isPlankProfileValidForCaptureSource(
                m_PlankVideoProfile,
                m_PlankCaptureSource)) {
        const QString error = tr("The selected capture source and encoding profile are not compatible.");
        qCritical("%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT &&
            m_PlankVideoProfile ==
                StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444 &&
            (m_Computer->plankFeatureFlags &
             NvOutputTopology::NvfbcHevc10NvencFeature) == 0) {
        const QString error = tr("The host does not support NvFBC with the HEVC 10-bit NVENC profile.");
        qCritical("%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }
    if (StreamingPreferences::isPlankNvenc420Profile(m_PlankVideoProfile) &&
            (m_Computer->plankFeatureFlags &
             NvOutputTopology::NvfbcNvenc420Feature) == 0) {
        const QString error = tr("This workstation does not offer the NVENC 4:2:0 profiles yet. Choose another encoding profile or update BDE fernweh on the workstation.");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }
    if (!m_Computer->plankEncodingModes.isEmpty() &&
            !m_Computer->plankEncodingModes.contains(
                StreamingPreferences::plankEncodingMode(m_PlankVideoProfile))) {
        // The host advertises what it can encode; fail before launch instead
        // of substituting another profile.
        const QString error = tr("This workstation can't use the selected encoding profile (%1). Choose another encoding in its settings.")
                .arg(StreamingPreferences::plankEncodingMode(m_PlankVideoProfile));
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }

#ifndef PLANK_QT_SESSION
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        qCritical("SDL video initialization failed: %s", SDL_GetError());
        return false;
    }
    initializedSdlVideo = true;
    if (!snapshotClientDisplays()) {
        emit displayLaunchError(tr("Unable to read the Client display layout. Check the connected displays and try again."));
        return false;
    }
#endif

    LiInitializeStreamConfiguration(&m_StreamConfig);
    if (!configurePlankLaunchGeometry()) {
        return false;
    }

#ifndef PLANK_QT_SESSION
    int testX, testY, testWidth, testHeight;
    getWindowDimensions(testX, testY, testWidth, testHeight);
    testWindow = SDL_CreateWindow("", testWidth, testHeight,
                                  SDL_WINDOW_HIDDEN | StreamUtils::getPlatformWindowFlags());
    if (!testWindow) {
        // Preserve the plain hidden-window probe when an optional
        // platform-specific surface cannot be created by the backend.
        testWindow = SDL_CreateWindow("", testWidth, testHeight, SDL_WINDOW_HIDDEN);
    }
    if (!testWindow) {
        qCritical("Unable to create the video decode test window: %s", SDL_GetError());
        return false;
    }
#endif

    qInfo() << "PLANK host version:"
            << m_Computer->plankHostVersion;

    LiInitializeVideoCallbacks(&m_VideoCallbacks);
    m_VideoCallbacks.setup = drSetup;

    m_StreamConfig.fps = (m_Computer->plankAuthentication ||
                          m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) ?
                StreamingPreferences::PlankFramesPerSecond : m_Preferences->fps;
    std::optional<DecoderProbe> probe;
    if (!selectAutomaticVideoProfile(testWindow, probe)) {
        return false;
    }
    m_StreamConfig.bitrate = m_PlankBitrateKbps;

    qInfo("Video bitrate: %d kbps",
                m_StreamConfig.bitrate);

    switch (m_Preferences->audioConfig)
    {
    case StreamingPreferences::AC_STEREO:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
        break;
    case StreamingPreferences::AC_51_SURROUND:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;
        break;
    case StreamingPreferences::AC_71_SURROUND:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;
        break;
    }
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
        // The authenticated Mac contract is system audio, stereo 48 kHz.
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    }

    LiInitializeAudioCallbacks(&m_AudioCallbacks);
    m_AudioCallbacks.init = arInit;
    m_AudioCallbacks.cleanup = arCleanup;
    m_AudioCallbacks.decodeAndPlaySample = arDecodeAndPlaySample;
    m_AudioCallbacks.capabilities = getAudioRendererCapabilities(m_StreamConfig.audioConfiguration);

    qInfo("Audio channel count: %d",
                CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration));
    qInfo("Audio channel mask: %X",
                CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration));

    // PLANK advertises exactly the selected profile. Do not silently
    // substitute another bit depth or chroma format when probing fails.
    const auto* selectedProfile = plank_encoding_profile(static_cast<int>(m_PlankVideoProfile));
    if (!selectedProfile) {
        emit displayLaunchError(tr("The bookmark contains an invalid encoding profile."));
        return false;
    }
    const int selectedVideoFormat = selectedProfile->video_format;
    if (!(selectedVideoFormat & VIDEO_FORMAT_MASK_YUV444) ||
            isIdentityGbrEnabledForFormat(selectedVideoFormat) ||
            StreamingPreferences::isPlankAppleProfile(m_PlankVideoProfile)) {
        m_SupportedVideoFormats.append(selectedVideoFormat);
    }

    Q_ASSERT(m_SupportedVideoFormats.size() == 1);

    // Check for validation errors/warnings and emit
    // signals for them, if appropriate
    if (!validateLaunch(testWindow, probe)) {
        return false;
    }
    m_StreamConfig.supportedVideoFormats = m_SupportedVideoFormats.front();
    populateDecoderProperties(*probe);

    keepSdlVideo = initializedSdlVideo;
    return true;
}

void Session::emitLaunchWarning(QString text)
{
    // Emit the warning to the UI
    emit displayLaunchWarning(text);

    // Wait a little bit so the user can actually read what we just said.
    // This wait is a little longer than the actual toast timeout (3 seconds)
    // to allow it to transition off the screen before continuing.
    const quint64 start = PlankClock::milliseconds();
    while (!m_Lifecycle.stopping() && PlankClock::milliseconds() < start + 3500) {
        QThread::msleep(5);

    }
}

bool Session::validateLaunch(SDL_Window* testWindow, std::optional<DecoderProbe>& probe)
{
    m_SupportedVideoFormats.removeByMask(
                ~m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport));
    if (m_SupportedVideoFormats.isEmpty()) {
        emit displayLaunchError(tr("The selected BDE fernweh encoding profile is not supported by both this host and client."));
        return false;
    }

    // Automatic already proved the exact format at this geometry. For a
    // manual choice or geometry change, probe exact hardware then software.
    while (!probe && !m_SupportedVideoFormats.isEmpty()) {
        const int format = m_SupportedVideoFormats.front();
        IVideoDecoder* decoder = nullptr;
        if (chooseDecoder(format, m_StreamConfig.width, m_StreamConfig.height, m_StreamConfig.fps,
                          false, true, decoder, isIdentityGbrEnabledForFormat(format),
                          decoderCaptureSource(), decoderEncoderBackend(), m_RendererFactory, testWindow)) {
            probe = DecoderProbe{decoder->getDecoderCapabilities(), decoder->getDecoderColorspace(),
                                 decoder->getDecoderColorRange()};
            delete decoder;
        } else {
            m_SupportedVideoFormats.removeFirst();
        }
    }
    if (m_SupportedVideoFormats.isEmpty()) {
        emit displayLaunchError(tr("This client cannot decode the selected BDE fernweh encoding profile."));
        return false;
    }

    // Test if audio works at the specified audio configuration
    bool audioTestPassed = testAudio(m_StreamConfig.audioConfiguration);

    // Gracefully degrade to stereo if surround sound doesn't work
    if (!audioTestPassed && CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration) > 2) {
        audioTestPassed = testAudio(AUDIO_CONFIGURATION_STEREO);
        if (audioTestPassed) {
            m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
            emitLaunchWarning(tr("Your selected surround sound setting is not supported by the current audio device."));
        }
    }

    // If nothing worked, warn the user that audio will not work
    if (!audioTestPassed) {
        emitLaunchWarning(tr("Failed to open audio device. Audio will be unavailable during this session."));
    }

    return true;
}


class DeferredSessionCleanupTask : public QRunnable
{
public:
    DeferredSessionCleanupTask(Session* session) :
        m_Session(session) {}

private:
    virtual ~DeferredSessionCleanupTask() override
    {
        // Allow another session to start now that we're cleaned up
        Session::s_ActiveSession = nullptr;
        Session::s_ActiveSessionSemaphore.release();

        // Notify that the session is ready to be cleaned up
        emit m_Session->readyForDeletion();
    }

    void run() override
    {
        // The video decoder must already be destroyed, since it could
        // try to interact with APIs that can only be called between
        // LiStartConnection() and LiStopConnection().
        SDL_assert(m_Session->m_VideoDecoder == nullptr);

        // Finish cleanup of the connection state
        qInfo() << "Session connection cleanup started";
#ifdef PLANK_TRANSPORT
        m_Session->stopPlankTransportMediaReceivers();
#endif
        LiStopConnection();
        m_Session->stopPlankTransportDataPlane();
        qInfo() << "Session connection cleanup finished";
        // The launcher can close as soon as it is shown again. Keep its event
        // loop and broker lease alive until the stream has finished closing.
        m_Session->publishDiagnosticLifecycle(true);
#ifdef Q_OS_MACOS
        if (!m_Session->m_ApplicationExitRequested.load())
#endif
        emit m_Session->sessionFinished();
    }

    Session* m_Session;
};

int Session::getTargetDisplayIndex() const
{
    int displayIndex = 0;

    if (m_Window == nullptr && m_PlannedPrimaryDisplay != 0) {
        // Display arrangement: the stream opens on the plan's primary.
        const int plannedIndex = StreamUtils::getDisplayIndex(m_PlannedPrimaryDisplay);
        if (plannedIndex >= 0) {
            return plannedIndex;
        }
    }

    if (m_Window != nullptr) {
        displayIndex = StreamUtils::getDisplayIndex(SDL_GetDisplayForWindow(m_Window));
        if (displayIndex < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SDL_GetDisplayForWindow() failed: %s",
                        SDL_GetError());
            displayIndex = 0;
        }
    }
    // Create our window on the same display that Qt's UI
    // was being displayed on.
    else {
        if (m_QtWindow != nullptr) {
            if (m_QtTargetDisplayBounds.isValid()) {
                const QRect displayRect = m_QtTargetDisplayBounds;

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Qt UI screen is at (%d,%d)",
                            displayRect.x(), displayRect.y());
                for (int i = 0; i < StreamUtils::getDisplayCount(); i++) {
                    SDL_Rect displayBounds;

                    if (SDL_GetDisplayBounds(StreamUtils::getDisplayId(i), &displayBounds)) {
                        if (displayBounds.x == displayRect.x() &&
                            displayBounds.y == displayRect.y()) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                        "SDL found matching display %d",
                                        i);
                            displayIndex = i;
                            break;
                        }
                    }
                    else {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                    "SDL_GetDisplayBounds(%d) failed: %s",
                                    i, SDL_GetError());
                    }
                }
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Qt window is not associated with a QScreen!");
            }
        }
    }

    return displayIndex;
}

QVector<NvClientDisplay> Session::probeClientDisplays()
{
    return m_ThreadedExec ? m_GuiDisplaySnapshot.request() : ClientDisplayProbe::probe();
}

bool Session::snapshotClientDisplays()
{
    m_ClientDisplays.clear();
    const int targetIndex = getTargetDisplayIndex();
    m_TargetDisplayId = StreamUtils::getDisplayId(targetIndex);
    const int displayCount = StreamUtils::getDisplayCount();
    // Windows and macOS run the SDL session on the GUI thread. Use the same
    // monitor identities as display setup so saved on/off choices survive
    // the handoff from Qt's monitor list to SDL's.
#if defined(Q_OS_DARWIN) || defined(Q_OS_WIN32)
    const QVector<NvClientDisplay> probed = ClientDisplayProbe::probe();
#else
    // Captured on the GUI thread before SDL starts, refreshed by explicit
    // value handoffs after hotplug or Apply. QScreen stays on its owner thread.
    const QVector<NvClientDisplay> probed = m_ClientDisplayProbe;
#endif
    for (int index = 0; index < displayCount; ++index) {
        ClientDisplaySnapshot snapshot;
        snapshot.displayId = StreamUtils::getDisplayId(index);
        snapshot.primary = snapshot.displayId == SDL_GetPrimaryDisplay();
        if (snapshot.displayId == 0 ||
                !SDL_GetDisplayBounds(snapshot.displayId,
                                      &snapshot.logicalBounds)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Unable to snapshot client display %d: %s",
                         index, SDL_GetError());
            return false;
        }

#ifdef Q_OS_DARWIN
        {
            SDL_DisplayMode currentMode;
            SDL_Rect matchedBounds;
            if (!StreamUtils::getMacCurrentDisplayModeForBounds(snapshot.logicalBounds,
                    &currentMode, &matchedBounds, m_IsFullScreen &&
                    MacDisplayGeometry::useNativeFullscreen(displayCount))) return false;
            snapshot.macMatchedBounds = QRect(matchedBounds.x, matchedBounds.y,
                                             matchedBounds.w, matchedBounds.h);
            snapshot.macBackingSize = QSize(currentMode.w, currentMode.h);
            // Stream sizing and presentation must use the active backing-pixel
            // canvas for every Host/layout, not the panel's advertised native
            // mode. A virtual display's native mode can differ from its current
            // size. Keep the logical bounds separately for Mac Match Client.
            snapshot.nativeSize = snapshot.macBackingSize;
            // A Mac host creates a desktop of exactly these pixels.
            snapshot.matchTarget = snapshot.macBackingSize;
        }
#else
        SDL_DisplayMode nativeMode;
        SDL_Rect safeArea;
        if (!StreamUtils::getNativeDesktopMode(index, &nativeMode, &safeArea)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Unable to snapshot client display %d: %s",
                         index, SDL_GetError());
            return false;
        }
        snapshot.nativeSize = QSize(nativeMode.w, nativeMode.h);
#endif
        snapshot.probeView = ClientDisplayProbe::forSessionDisplay(
                    QRect(snapshot.logicalBounds.x, snapshot.logicalBounds.y,
                          snapshot.logicalBounds.w, snapshot.logicalBounds.h),
                    snapshot.nativeSize, probed);
#ifndef Q_OS_DARWIN
        if (snapshot.probeView.key.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Could not match SDL output %u to a display setup monitor",
                         snapshot.displayId);
            emit displayLaunchError(tr("Your displays changed during setup. Check the connected displays and try again."));
            return false;
        }
#endif
#ifdef Q_OS_DARWIN
        snapshot.matchTarget = snapshot.macBackingSize;
#else
        snapshot.matchTarget = NvOutputTopology::clientMatchTarget(snapshot.probeView);
#endif
        m_ClientDisplays.append(snapshot);
    }

    std::sort(m_ClientDisplays.begin(), m_ClientDisplays.end(),
              [](const auto& left, const auto& right) {
        return std::make_tuple(left.logicalBounds.x, left.logicalBounds.y) <
                std::make_tuple(right.logicalBounds.x,
                                right.logicalBounds.y);
    });
    // Remember multi-output capability even when the session starts windowed.
    // Fullscreen may be entered later without reconnecting; only the active
    // presentation layout, not display discovery, depends on that state.
    m_MultiDisplayPresentationAvailable =
            (strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0 ||
             strcmp(SDL_GetCurrentVideoDriver(), "cocoa") == 0) &&
            m_ClientDisplays.size() == 2;
    if (m_MultiDisplayPresentationAvailable) {
        const auto& left = m_ClientDisplays.at(0).logicalBounds;
        const auto& right = m_ClientDisplays.at(1).logicalBounds;
        const bool horizontal = left.x + left.w <= right.x;
        const bool overlapsVertically = left.y < right.y + right.h &&
                right.y < left.y + left.h;
        if (!horizontal || !overlapsVertically) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Two-output presentation requires client monitors arranged left to right; using the target output only");
            m_MultiDisplayPresentationAvailable = false;
        }
    }

    int canvasX = 0;
    int canvasHeight = 0;
    for (auto& display : m_ClientDisplays) {
        display.canvasRect = QRect(canvasX, 0,
                                   display.nativeSize.width(),
                                   display.nativeSize.height());
        canvasX += display.nativeSize.width();
        canvasHeight = qMax(canvasHeight, display.nativeSize.height());
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK client output %u: logical=%dx%d%+d%+d native=%dx%d desktop=%dx%d target=%dx%d canvas=%dx%d%+d%+d%s",
                    display.displayId,
                    display.logicalBounds.w, display.logicalBounds.h,
                    display.logicalBounds.x, display.logicalBounds.y,
                    display.nativeSize.width(), display.nativeSize.height(),
                    display.probeView.backingSize.width(), display.probeView.backingSize.height(),
                    display.matchTarget.width(), display.matchTarget.height(),
                    display.canvasRect.width(), display.canvasRect.height(),
                    display.canvasRect.x(), display.canvasRect.y(),
                    display.displayId == m_TargetDisplayId ? " primary" : "");
    }
    if (m_ClientDisplays.isEmpty()) {
        return false;
    }
    QSize targetNativeSize;
    for (const auto& display : std::as_const(m_ClientDisplays)) {
        if (display.displayId == m_TargetDisplayId) {
            targetNativeSize = display.nativeSize;
            break;
        }
    }
    if (!targetNativeSize.isValid()) {
        targetNativeSize = m_ClientDisplays.first().nativeSize;
        m_TargetDisplayId = m_ClientDisplays.first().displayId;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK client presentation capability: outputs=%lld canvas=%dx%d mode=%s",
                static_cast<long long>(
                    m_MultiDisplayPresentationAvailable ? m_ClientDisplays.size() : 1),
                m_MultiDisplayPresentationAvailable ? canvasX : targetNativeSize.width(),
                m_MultiDisplayPresentationAvailable ? canvasHeight : targetNativeSize.height(),
                m_MultiDisplayPresentationAvailable ? "multi-output" : "single-output");
    return true;
}

void Session::rebuildPresentationLayout()
{
    if (isCollaborator()) {
        rebuildCollaborationPresentation();
        return;
    }
    m_PresentationLayout = {};
    if (m_Window == nullptr) {
        return;
    }

    const bool multiOutputActive = m_UseMultiDisplayPresentation &&
            (m_PresentationFullscreen || strcmp(SDL_GetCurrentVideoDriver(), "cocoa") == 0) &&
            !m_SecondaryWindows.isEmpty();
    if (multiOutputActive) {
        int canvasWidth = 0;
        int canvasHeight = 0;
        int secondaryIndex = 0;
        const bool arrangement = !m_ResolvedArrangement.isEmpty();
        const bool plannedLayout = m_DisplayPlan.ok;
        NvOutputTopology topology;
        {
            QReadLocker lock(&m_Computer->lock);
            topology = m_Computer->outputTopology;
        }
        const QSize streamSize(m_StreamConfig.width, m_StreamConfig.height);
        // Source rectangles are in the host's capture (packed rows when the
        // desk is wider than the encoder), pointer positions in its desktop.
        const QSize captureSize = topology.captureSize();
        const QSize desktopSize(topology.desktopWidth, topology.desktopHeight);
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            if (plannedLayout && display.planIndex < 0) {
                // Stays local: no window (createSecondaryWindows skips it too).
                continue;
            }
            SDL_Window* window = display.displayId == m_TargetDisplayId ?
                        m_Window : nullptr;
            if (window == nullptr &&
                    secondaryIndex < m_SecondaryWindows.size()) {
                // Secondary windows are created in the same stable display
                // order as this snapshot. Keep intended presentation geometry
                // independent of asynchronous compositor placement state.
                window = m_SecondaryWindows.at(secondaryIndex++);
            }
            if (window == nullptr) {
                continue;
            }
            PlankPresentationOutput output {window, display.canvasRect, window == m_Window};
            if (plannedLayout) {
                // The host's own rectangles for this entry when it published
                // them (the truth after launch), else the plan's.
                const DisplayPlanner::Output& planned = m_DisplayPlan.outputs.at(display.planIndex);
                output.canvasRect = QRect(planned.position, planned.size);
                QRect desktopRect = output.canvasRect;
                QRect captureRect = planned.captureRect.isValid() ? planned.captureRect : output.canvasRect;
                QSize capture = m_DisplayPlan.capture.isValid() ? m_DisplayPlan.capture : m_DisplayPlan.canvas;
                QSize desktop = m_DisplayPlan.canvas;
                if (arrangement && topology.matchesRequestedArrangement(m_ResolvedArrangement)) {
                    for (const NvOutput& hostOutput : std::as_const(topology.outputs)) {
                        if (hostOutput.arrangementIndex == planned.arrangementIndex) {
                            desktopRect = QRect(hostOutput.x - topology.desktopX, hostOutput.y - topology.desktopY,
                                                hostOutput.width, hostOutput.height);
                            captureRect = hostOutput.captureRect();
                            capture = captureSize;
                            desktop = desktopSize;
                            break;
                        }
                    }
                }
                output.desktopRect = desktopRect;
                output.sourceRect = PlankPresentation::sourceRectInStream(captureRect, capture, streamSize);
                m_PresentationLayout.desktopSize = desktop;
            }
            m_PresentationLayout.outputs.append(output);
            canvasWidth = qMax(canvasWidth, output.canvasRect.right() + 1);
            canvasHeight = qMax(canvasHeight, output.canvasRect.bottom() + 1);
        }
        m_PresentationLayout.canvasSize = QSize(canvasWidth, canvasHeight);
        if (!arrangement && topology.layoutKind == NvOutputTopology::DualHorizontalHostLayout &&
                topology.outputs.size() == m_PresentationLayout.outputs.size() &&
                captureSize.isValid() && !captureSize.isEmpty()) {
            // Today's two-output layout: each window shows the host output
            // with the same place from the left, through its source_rect.
            QVector<NvOutput> hostOutputs = topology.outputs;
            std::sort(hostOutputs.begin(), hostOutputs.end(), [](const NvOutput& a, const NvOutput& b) {
                return std::make_tuple(a.x, a.y) < std::make_tuple(b.x, b.y);
            });
            for (int index = 0; index < hostOutputs.size(); ++index) {
                const NvOutput& hostOutput = hostOutputs.at(index);
                auto& output = m_PresentationLayout.outputs[index];
                output.desktopRect = QRect(hostOutput.x - topology.desktopX, hostOutput.y - topology.desktopY,
                                           hostOutput.width, hostOutput.height);
                output.sourceRect = PlankPresentation::sourceRectInStream(
                            hostOutput.captureRect(), captureSize, streamSize);
            }
            m_PresentationLayout.desktopSize = desktopSize;
        }
    }
    else {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(m_Window, &width, &height);
        m_PresentationLayout.canvasSize = QSize(qMax(1, width), qMax(1, height));
        PlankPresentationOutput output {m_Window, QRect(QPoint(0, 0), m_PresentationLayout.canvasSize), true};
        NvOutputTopology topology;
        {
            QReadLocker lock(&m_Computer->lock);
            topology = m_Computer->outputTopology;
        }
        bool packed = false;
        for (const NvOutput& hostOutput : std::as_const(topology.outputs)) {
            packed = packed || hostOutput.captureRect() !=
                    QRect(hostOutput.sourceX, hostOutput.sourceY, hostOutput.sourceWidth, hostOutput.sourceHeight);
        }
        if (!m_ResolvedArrangement.isEmpty() && topology.matchesRequestedArrangement(m_ResolvedArrangement) &&
                (packed || topology.captureSize() != QSize(topology.desktopWidth, topology.desktopHeight))) {
            // One window on a packed capture (left fullscreen): it would show
            // the rows, not the desk, so it shows the primary display, and the
            // pointer maps through it to the desktop.
            for (const NvOutput& hostOutput : std::as_const(topology.outputs)) {
                if (!hostOutput.primary) continue;
                output.desktopRect = QRect(hostOutput.x - topology.desktopX, hostOutput.y - topology.desktopY,
                                           hostOutput.width, hostOutput.height);
                output.sourceRect = PlankPresentation::sourceRectInStream(
                            hostOutput.captureRect(), topology.captureSize(),
                            QSize(m_StreamConfig.width, m_StreamConfig.height));
                m_PresentationLayout.desktopSize = QSize(topology.desktopWidth, topology.desktopHeight);
                break;
            }
        }
        m_PresentationLayout.outputs.append(output);
    }

    if (m_InputHandler != nullptr) {
        m_InputHandler->setPresentationLayout(m_PresentationLayout);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK render presentation: outputs=%lld canvas=%dx%d fullscreen=%s",
                static_cast<long long>(m_PresentationLayout.outputs.size()),
                m_PresentationLayout.canvasSize.width(),
                m_PresentationLayout.canvasSize.height(),
                m_PresentationFullscreen ? "yes" : "no");
}

void Session::updateCollaborationIndicator()
{
#ifdef PLANK_QT_SESSION
    emit qtCollaborationChanged();
    return;
#endif
    if (!isCollaborator()) return;
    const bool viewOnly = m_CollaborationPermission.load() == PlankCollaboration::View;
    const QByteArray text = (viewOnly ? tr("Sharing with %1 - View only") :
        tr("Sharing with %1 - Keyboard and mouse control")).arg(m_CollaborationOwner).toUtf8();
    m_OverlayManager.updateOverlayText(Overlay::OverlayCollaboration, text.constData());
    m_OverlayManager.setOverlayState(Overlay::OverlayCollaboration, true);
    const QImage hud = plankSharingHud(QString::fromUtf8(text), viewOnly);
    SDL_Surface* image = SDL_CreateSurfaceFrom(hud.width(), hud.height(), SDL_PIXELFORMAT_ARGB8888,
                                              const_cast<uchar*>(hud.constBits()), hud.bytesPerLine());
    if (image != nullptr) {
        m_OverlayManager.updateOverlaySurface(Overlay::OverlayCollaboration, SDL_DuplicateSurface(image));
        SDL_DestroySurface(image);
    }
    for (int index = 0; index < m_CollaborationOutputs.size(); ++index) {
        SDL_Window* window = index == 0 ? m_Window : m_SecondaryWindows.value(index - 1, nullptr);
        if (window == nullptr) continue;
        const QByteArray title = tr("%1 · Shared display %2 (%3)").arg(m_Computer->name).arg(index + 1)
            .arg(viewOnly ? tr("View only") : tr("Control")).toUtf8();
        SDL_SetWindowTitle(window, title.constData());
    }
}

void Session::applyCollaborationPermission()
{
    if (!isCollaborator()) return;
    m_InputHandler->raiseAllKeys();
    const bool viewOnly = m_CollaborationPermission.load() == PlankCollaboration::View;
    m_InputHandler->setViewOnly(viewOnly);
    m_InputHandler->setCaptureActive(!viewOnly && anyPresentationWindowFocused());
    updateCollaborationIndicator();
}

bool Session::createCollaborationWindows()
{
    NvOutputTopology topology;
    {
        QReadLocker lock(&m_Computer->lock);
        topology = m_Computer->outputTopology;
    }
    m_CollaborationOutputs = topology.outputs;
    if (!m_CollaborationOutputId.isEmpty()) {
        m_CollaborationOutputs.erase(std::remove_if(m_CollaborationOutputs.begin(), m_CollaborationOutputs.end(),
            [this](const NvOutput& output) { return output.id != m_CollaborationOutputId; }), m_CollaborationOutputs.end());
    }
    if (m_CollaborationOutputs.isEmpty()) {
        emit displayLaunchError(tr("The owner's captured display is no longer available. Join the session again."));
        return false;
    }
    std::stable_sort(m_CollaborationOutputs.begin(), m_CollaborationOutputs.end(),
        [](const NvOutput& left, const NvOutput& right) { return left.primary && !right.primary; });
    SDL_Rect usable {};
    if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(m_Window), &usable)) return false;
    QVector<QRect> desktopOutputs;
    for (const auto& output : m_CollaborationOutputs)
        desktopOutputs.append(QRect(output.x, output.y, output.width, output.height));
    const QVector<QRect> placement = PlankCollaboration::windowPlacement(
        desktopOutputs, QRect(usable.x, usable.y, usable.w, usable.h));
    if (placement.size() != m_CollaborationOutputs.size()) return false;
    m_IsFullScreen = false;
    m_PresentationFullscreen = false;
    m_UseMultiDisplayPresentation = false;
    m_HasWindowedPresentationGeometry = true;
    for (int index = 0; index < placement.size(); ++index) {
        const NvOutput& output = m_CollaborationOutputs[index];
        const QRect bounds = placement[index];
        const QByteArray title = tr("%1 · Shared display %2 (%3)")
            .arg(m_Computer->name).arg(index + 1)
            .arg(m_CollaborationPermission.load() == PlankCollaboration::View ? tr("View only") : tr("Control")).toUtf8();
        SDL_Window* window = m_Window;
        if (index == 0) {
            SDL_SetWindowFullscreen(window, false);
            SDL_SetWindowTitle(window, title.constData());
            SDL_SetWindowSize(window, bounds.width(), bounds.height());
        } else {
            window = SDL_CreateWindow(title.constData(), bounds.width(), bounds.height(),
                SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE | StreamUtils::getPlatformWindowFlags());
            if (window == nullptr) { destroySecondaryWindows(); return false; }
            m_SecondaryWindows.append(window);
        }
        SDL_SetWindowPosition(window, bounds.x(), bounds.y());
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Collaboration output %d: desktop=%dx%d%+d%+d local=%dx%d%+d%+d",
            index, output.width, output.height, output.x, output.y,
            bounds.width(), bounds.height(), bounds.x(), bounds.y());
    }
    return true;
}

void Session::rebuildCollaborationPresentation()
{
    m_PresentationLayout = {};
    if (m_Window == nullptr || m_CollaborationOutputs.isEmpty()) return;
    NvOutputTopology topology;
    {
        QReadLocker lock(&m_Computer->lock);
        topology = m_Computer->outputTopology;
    }
    m_PresentationLayout.desktopSize = QSize(topology.desktopWidth, topology.desktopHeight);
    m_PresentationLayout.canvasSize = m_PresentationLayout.desktopSize;
    for (int index = 0; index < m_CollaborationOutputs.size(); ++index) {
        SDL_Window* window = index == 0 ? m_Window : m_SecondaryWindows.value(index - 1, nullptr);
        if (window == nullptr) continue;
        const NvOutput& output = m_CollaborationOutputs[index];
        const QRect rectangle(output.x - topology.desktopX, output.y - topology.desktopY,
                              output.width, output.height);
        PlankPresentationOutput view {window, rectangle, index == 0};
        view.desktopRect = rectangle;
        view.sourceRect = m_CollaborationOutputId.isEmpty() ?
            PlankPresentation::sourceRectInStream(output.captureRect(), topology.captureSize(),
                QSize(m_StreamConfig.width, m_StreamConfig.height)) :
            QRectF(0, 0, m_StreamConfig.width, m_StreamConfig.height);
        m_PresentationLayout.outputs.append(view);
    }
    if (m_InputHandler != nullptr) m_InputHandler->setPresentationLayout(m_PresentationLayout);
}

bool Session::createSecondaryWindows(Uint32 defaultWindowFlags, const std::string& windowName)
{
    const bool cocoa = strcmp(SDL_GetCurrentVideoDriver(), "cocoa") == 0;
    for (const auto& display : std::as_const(m_ClientDisplays)) {
        if (display.displayId == m_TargetDisplayId ||
                (m_DisplayPlan.ok && display.planIndex < 0)) {
            continue;
        }

        SDL_PropertiesID properties = SDL_CreateProperties();
        // macOS: each window enters native fullscreen on its own display
        // later, one at a time (setPresentationWindowsFullscreen). Create it
        // windowed and hidden there first.
        const Uint32 flags = (defaultWindowFlags & ~SDL_WINDOW_FULLSCREEN) |
                (cocoa ? SDL_WINDOW_HIDDEN : 0) |
                StreamUtils::getPlatformWindowFlags();
        SDL_SetStringProperty(properties,
                              SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                              windowName.c_str());
        SDL_SetNumberProperty(properties,
                              SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                              display.logicalBounds.w);
        SDL_SetNumberProperty(properties,
                              SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                              display.logicalBounds.h);
        SDL_SetNumberProperty(properties,
                              SDL_PROP_WINDOW_CREATE_X_NUMBER,
                              SDL_WINDOWPOS_CENTERED_DISPLAY(display.displayId));
        SDL_SetNumberProperty(properties,
                              SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                              SDL_WINDOWPOS_CENTERED_DISPLAY(display.displayId));
        SDL_SetNumberProperty(properties,
                              SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                              flags);
        SDL_SetBooleanProperty(properties,
                               SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                               !cocoa);
        SDL_Window* secondary = SDL_CreateWindowWithProperties(properties);
        SDL_DestroyProperties(properties);
        if (secondary == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to create PLANK fullscreen surface for client output %u: %s",
                         display.displayId, SDL_GetError());
            emit displayLaunchError(
                tr("Unable to create a fullscreen surface for the second client monitor."));
            destroySecondaryWindows();
            return false;
        }
        if (!cocoa) {
            SDL_SetWindowFullscreenMode(secondary, nullptr);
            SDL_SetWindowFullscreen(secondary, true);
            if (!placeFullscreenWindowOnDisplay(secondary,
                                                display.displayId)) {
                SDL_DestroyWindow(secondary);
                emit displayLaunchError(
                    tr("Unable to place the second fullscreen surface on its client monitor."));
                destroySecondaryWindows();
                return false;
            }
        }
        else {
            // Borderless desktop fullscreen in its own Space, on this display.
            SDL_SetWindowFullscreenMode(secondary, nullptr);
            SDL_Rect usable;
            if (SDL_GetDisplayUsableBounds(display.displayId, &usable)) {
                const QSize source = m_DisplayPlan.ok && display.planIndex >= 0 ?
                            m_DisplayPlan.outputs.at(display.planIndex).size : display.nativeSize;
                const QRect bounds = PlankPresentation::windowedOutputRect(
                            QRect(usable.x, usable.y, usable.w, usable.h), source);
                SDL_SetWindowSize(secondary, bounds.width(), bounds.height());
                SDL_SetWindowPosition(secondary, bounds.x(), bounds.y());
            }
        }
        SDL_StopTextInput(secondary);
        m_SecondaryWindows.append(secondary);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Created PLANK %s surface for output %u",
                    cocoa ? "macOS" : "Wayland fullscreen", display.displayId);
    }
    return true;
}

void Session::destroySecondaryWindows()
{
    for (SDL_Window* window : std::as_const(m_SecondaryWindows)) {
        SDL_DestroyWindow(window);
    }
    m_SecondaryWindows.clear();
}

void Session::collapseToSingleWindow()
{
    if (m_SecondaryWindows.isEmpty()) {
        return;
    }
    // The renderer draws into every window: release it before they go.
    m_DecoderLock.lock();
    delete m_VideoDecoder;
    m_VideoDecoder = nullptr;
    m_DecoderLock.unlock();
    destroySecondaryWindows();
    m_UseMultiDisplayPresentation = false;
    rebuildPresentationLayout();
    SDL_Event resetEvent = {};
    resetEvent.type = SDL_EVENT_RENDER_DEVICE_RESET;
    SDL_PushEvent(&resetEvent);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK presentation collapsed to one scaled window after a screen change");
}

void Session::handleClientDisplaysChanged()
{
    if (isCollaborator()) return;
    const QVector<NvClientDisplay> probed = probeClientDisplays();
    m_ClientDisplayProbe = probed;
    const QString fingerprint = ClientDisplayProbe::fingerprint(probed);
    const bool changed = m_ClientDisplayChange.observe(probed);
    QSettings settings;
    const bool prompt = m_ClientDisplayChange.shouldPrompt(settings);
    if (!prompt && m_PlankToolbar) m_PlankToolbar->hideScreensPrompt();
    if (!changed) {
        // A mode or position notification that changed nothing we present.
        return;
    }
    // A window whose display went away shows nothing useful; one window,
    // scaled, until the user decides.
    collapseToSingleWindow();
    if (!snapshotClientDisplays()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PLANK could not read the client displays after a change");
        return;
    }
    QString layoutPolicy;
    {
        QReadLocker lock(&m_Computer->lock);
        layoutPolicy = m_Computer->plankHostLayout;
    }
    const bool followsClient = layoutPolicy == NvOutputTopology::MatchClientHostLayout &&
            m_PlankCaptureSource != StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK client screens changed: %lld display(s)%s",
                static_cast<long long>(probed.size()),
                fingerprint == m_PresentedDisplayFingerprint ? " (same monitors)" : " (new monitor set)");
    if (!followsClient || !m_PlankToolbar) {
        return;
    }
    if (m_ClientDisplayChange.matchesApplied() && m_DisplayPlan.ok) {
        // A transient change returned to the still-running Host layout.
        // Restore only our presentation; no Host reconnect or modeset is needed.
        applyArrangementPresentation();
        applyPendingPresentation(false);
        return;
    }
    if (!prompt) return;
    m_PlankToolbar->showScreensPrompt(
                fingerprint == m_PresentedDisplayFingerprint ?
                    tr("Screens rearranged: %1").arg(ClientDisplayProbe::label(probed)) :
                    tr("Screens changed: %1").arg(ClientDisplayProbe::label(probed)));
}

void Session::keepDisplayLayout()
{
    m_ClientDisplayChange.keep();
    m_PlankToolbar->hideScreensPrompt();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PLANK screens changed: keeping the current layout");
}

bool Session::requestDisplayLayoutApply()
{
    if (isCollaborator()) return false;
    if (m_Reconnecting.load() || m_ReconnectRequested.exchange(true) || !m_CanReconnect.load() ||
            !hasPlankCredentials()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PLANK cannot apply the display layout now");
        return false;
    }
    if (m_PlankToolbar) {
        m_PlankToolbar->hideScreensPrompt();
    }
    // The saved layout for the monitors connected now; the reconnect plans
    // and launches it (425 wait included) while the desktop keeps running.
    m_ClientDisplayProbe = probeClientDisplays();
    if (m_ClientDisplayProbe.isEmpty() || !snapshotClientDisplays()) {
        m_ReconnectRequested.store(false);
        return false;
    }
    collapseToSingleWindow();
    m_DisplayReconfigureRequested.store(true);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PLANK applying the display layout for the current screens");
    SDL_Event event = {};
    event.type = SDL_EVENT_USER;
    event.user.code = SDL_CODE_PLANK_RECONNECT;
    return SDL_PushEvent(&event);
}

void Session::applyDisplayLayout()
{
    if (isCollaborator()) return;
#ifdef PLANK_QT_SESSION
    emit qtDisplayLayoutApplyRequested();
    return;
#endif
    SDL_Event event = {};
    event.type = SDL_EVENT_USER;
    event.user.code = SDL_CODE_PLANK_APPLY_SCREENS;
    SDL_PushEvent(&event);
}

void Session::applyPendingPresentation(bool newLayout)
{
    if (!m_PresentationChangePending.exchange(false) || m_Window == nullptr) {
        return;
    }
    // The reconnect planned while the windows existed: one window per
    // display again, where the new plan puts them.
    collapseToSingleWindow();
    m_UseMultiDisplayPresentation = m_PendingMultiDisplayPresentation;
    m_TargetDisplayId = SDL_GetDisplayForWindow(m_Window);
    if (!PlankPresentation::placePrimaryDisplay(m_TargetDisplayId, m_PlannedPrimaryDisplay,
            [this](quint32 selected) {
        // Applying a layout may exclude the screen where the old main window
        // lives (for example the laptop panel). Move it to the selected primary
        // before creating the other windows; do not discard the new selection.
        return SDL_SetWindowFullscreen(m_Window, false) && SDL_SyncWindow(m_Window) &&
                SDL_SetWindowPosition(m_Window, SDL_WINDOWPOS_CENTERED_DISPLAY(selected),
                                      SDL_WINDOWPOS_CENTERED_DISPLAY(selected)) && SDL_SyncWindow(m_Window);
    })) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not move the presentation to its selected primary: %s", SDL_GetError());
    }
    if (m_UseMultiDisplayPresentation) {
        bool primaryShown = false;
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            primaryShown = primaryShown || (display.displayId == m_TargetDisplayId && display.planIndex >= 0);
        }
        const std::string windowName = QString(m_Computer->name).toStdString();
        if (!primaryShown || !createSecondaryWindows(SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE,
                                                     windowName)) {
            // The main window sits on a display the plan leaves local (or a
            // window failed): stay with one scaled window.
            destroySecondaryWindows();
            m_UseMultiDisplayPresentation = false;
        }
    }
    setPresentationWindowsFullscreen(m_PresentationFullscreen);
    const QVector<NvClientDisplay> presented = m_ClientDisplayProbe;
    m_PresentedDisplayFingerprint = ClientDisplayProbe::fingerprint(presented);
    if (newLayout) m_ClientDisplayChange.applied(presented);
    if (m_PlankToolbar) m_PlankToolbar->hideScreensPrompt();
    SDL_Event resetEvent = {};
    resetEvent.type = SDL_EVENT_RENDER_DEVICE_RESET;
    SDL_PushEvent(&resetEvent);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PLANK presentation after the new layout: %s",
                m_UseMultiDisplayPresentation ? "one window per display" : "one window");
}

bool Session::placeFullscreenWindowOnDisplay(SDL_Window* window,
                                             SDL_DisplayID displayId)
{
    if (!SDL_SyncWindow(window)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timed out synchronizing new fullscreen surface before output placement: %s",
                    SDL_GetError());
    }

    const int centered = SDL_WINDOWPOS_CENTERED_DISPLAY(displayId);
    if (!SDL_SetWindowPosition(window, centered, centered)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to request fullscreen surface placement on output %u: %s",
                     displayId, SDL_GetError());
        return false;
    }
    if (!SDL_SyncWindow(window)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timed out synchronizing fullscreen surface placement on output %u: %s",
                    displayId, SDL_GetError());
    }

    SDL_DisplayID actualDisplay = SDL_GetDisplayForWindow(window);
    if (actualDisplay == displayId) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Fullscreen surface assigned to requested client output %u",
                    displayId);
        return true;
    }

    // Some Wayland compositors retain the first fullscreen assignment until
    // the toplevel has completed one windowed configure. Re-seed SDL's target
    // display while windowed, then enter fullscreen again.
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Fullscreen surface landed on output %u instead of %u; retrying through a synchronized windowed configure",
                actualDisplay, displayId);
    if (!SDL_SetWindowFullscreen(window, false) ||
            !SDL_SyncWindow(window) ||
            !SDL_SetWindowPosition(window, centered, centered) ||
            !SDL_SyncWindow(window) ||
            !SDL_SetWindowFullscreen(window, true) ||
            !SDL_SyncWindow(window)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to reassign fullscreen surface to output %u: %s",
                     displayId, SDL_GetError());
        return false;
    }

    actualDisplay = SDL_GetDisplayForWindow(window);
    if (actualDisplay != displayId) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Window system kept fullscreen surface on output %u instead of requested output %u",
                     actualDisplay, displayId);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Fullscreen surface assigned to requested client output %u after synchronized retry",
                displayId);
    return true;
}

SDL_Window* Session::windowForEvent(Uint32 windowId) const
{
    SDL_Window* window = SDL_GetWindowFromID(windowId);
    if (window == m_Window || m_SecondaryWindows.contains(window)) {
        return window;
    }
    return nullptr;
}

bool Session::anyPresentationWindowFocused() const
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if (usesQtPresentation()) return m_QtClipboardFocused.load();
#endif
    if (m_Window != nullptr &&
            (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS)) {
        return true;
    }
    for (SDL_Window* window : m_SecondaryWindows) {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) {
            return true;
        }
    }
    return false;
}

void Session::setPresentationWindowsFullscreen(bool fullscreen)
{
    if (isCollaborator()) {
        // Each window continues to show its owner's output. Local fullscreen
        // never plans a different host arrangement or hides another output.
        m_PresentationFullscreen = fullscreen;
        SDL_SetWindowFullscreen(m_Window, fullscreen ? m_FullScreenFlag : 0);
        for (SDL_Window* window : m_SecondaryWindows) SDL_ShowWindow(window);
        rebuildPresentationLayout();
        return;
    }
    m_PresentationFullscreen = fullscreen;
#ifdef Q_OS_DARWIN
    const bool primaryChanged = MacWindow::setPresentationFullscreen(m_Window, fullscreen);
#else
    const bool primaryChanged = SDL_SetWindowFullscreen(m_Window, fullscreen ? m_FullScreenFlag : 0);
#endif
    if (!primaryChanged) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to set presentation fullscreen state: %s",
                    SDL_GetError());
    }
    if ((strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0 ||
         strcmp(SDL_GetCurrentVideoDriver(), "cocoa") == 0) &&
            !SDL_SyncWindow(m_Window)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timed out synchronizing presentation fullscreen state: %s",
                    SDL_GetError());
    }
    if (!fullscreen && !m_HasWindowedPresentationGeometry) {
        int x, y, width, height;
        getWindowDimensions(x, y, width, height);
        Q_UNUSED(x);
        Q_UNUSED(y);

        // The initial Wayland toplevel uses the complete output dimensions so
        // fullscreen absolute-pointer coordinates are correct from its first
        // configure. SDL also caches that size as the window's initial
        // floating geometry. Replace it on the first fullscreen exit so
        // libdecor can commit a complete, usable decoration frame immediately.
        if (!SDL_SetWindowSize(m_Window, width, height)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to establish initial windowed presentation geometry: %s",
                        SDL_GetError());
        }
        else {
            m_HasWindowedPresentationGeometry = true;
            if (strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0 &&
                    !SDL_SyncWindow(m_Window)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Timed out synchronizing initial windowed presentation geometry: %s",
                            SDL_GetError());
            }
        }
    }
    const bool cocoa = strcmp(SDL_GetCurrentVideoDriver(), "cocoa") == 0;
    if (cocoa && fullscreen && !m_SecondaryWindows.isEmpty() && !SDL_SyncWindow(m_Window)) {
        // Native fullscreen animates into a new Space per window; one at a
        // time or AppKit drops the later transitions.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timed out waiting for the primary fullscreen transition: %s",
                    SDL_GetError());
    }
    int secondaryIndex = 0;
    for (SDL_Window* window : m_SecondaryWindows) {
#ifdef Q_OS_DARWIN
        // A per-display selection owns a visible window in both modes. Native
        // fullscreen changes its placement, never its monitor/crop selection.
        if (cocoa) {
            if (!MacWindow::setPresentationFullscreen(window, fullscreen)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Failed to change secondary presentation state: %s", SDL_GetError());
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PLANK secondary window %d fullscreen=%s on display %u",
                        secondaryIndex++, fullscreen ? "yes" : "no", SDL_GetDisplayForWindow(window));
            continue;
        }
#endif
        if (fullscreen) {
            SDL_ShowWindow(window);
            if (!SDL_SetWindowFullscreen(window, m_FullScreenFlag)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Failed to set secondary presentation fullscreen state: %s",
                            SDL_GetError());
            }
            if (cocoa) {
                if (!SDL_SyncWindow(window)) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Timed out waiting for a secondary fullscreen transition: %s",
                                SDL_GetError());
                }
                // Which display it really landed on, for the log.
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK secondary window %d fullscreen on display %u",
                            secondaryIndex, SDL_GetDisplayForWindow(window));
            }
        }
        else {
            SDL_HideWindow(window);
        }
        ++secondaryIndex;
    }
    rebuildPresentationLayout();
}

void Session::minimizePresentationWindows()
{
    SDL_MinimizeWindow(m_Window);
    for (SDL_Window* window : m_SecondaryWindows) {
        SDL_MinimizeWindow(window);
    }
}

bool Session::configurePlankHostLayout()
{
    QString layoutPolicy;
    QString scalingMode;
    QString virtualMode1;
    QString virtualMode2;
    bool sharedMacDesktop = false;
    bool hostRejectsRequestedLayout = false;
    bool virtualPrimary = false;
    int hostFeatureFlags = 0;
    NvOutputTopology topologySnapshot;
    bool arrangementPublished = false;
    {
        QReadLocker lock(&m_Computer->lock);
        hostFeatureFlags = m_Computer->outputTopology.featureFlags;
        topologySnapshot = m_Computer->outputTopology;
        m_PlankLaunchFeatureFlags = topologySnapshot.launchFeatureFlags(m_Computer->plankFeatureFlags);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK launch capabilities: topology=0x%x discovery=0x%x negotiated=0x%x schema=%d",
                    topologySnapshot.featureFlags, m_Computer->plankFeatureFlags,
                    m_PlankLaunchFeatureFlags, topologySnapshot.schemaVersion);
        // Only when this launch negotiates it too.
        arrangementPublished = topologySnapshot.displayArrangementPublished() &&
                (m_PlankLaunchFeatureFlags &
                 NvOutputTopology::DisplayArrangementFeature) != 0;
        layoutPolicy = m_Computer->plankHostLayout;
        scalingMode = m_Computer->plankScalingMode;
        virtualMode1 = m_Computer->plankVirtualMode1;
        virtualMode2 = m_Computer->plankVirtualMode2;
        sharedMacDesktop = m_Computer->plankSharedDesktop;
        virtualPrimary = m_Computer->outputTopology.startupLayoutKind == NvOutputTopology::SingleHostLayout &&
            (m_Computer->outputTopology.featureFlags & NvOutputTopology::VirtualPrimaryConnectorFeature);
        const bool hostPolicyKnown = m_Computer->outputTopology.displayPolicyKnown();
        hostRejectsRequestedLayout = hostPolicyKnown &&
                !m_Computer->outputTopology.allowsBookmarkHostLayout(layoutPolicy);
    }

    if (hostRejectsRequestedLayout) {
        const QString error = tr("This workstation does not support the display layout selected by the bookmark.");
        qCritical("%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }

    m_ResolvedHostLayout.clear();
    m_ResolvedVirtualModes.clear();
    m_ResolvedPrimaryOutput = -1;
    m_ResolvedArrangement.clear();
    m_DisplayPlan = {};
    for (auto& display : m_ClientDisplays) {
        display.planIndex = -1;
    }
    if (m_Window == nullptr) {
        m_PlannedPrimaryDisplay = 0;
    }
    if (scalingMode != NvOutputTopology::NativeScalingMode &&
            scalingMode != NvOutputTopology::ScaledSpanMode) {
        const QString error = tr("The bookmark contains an unsupported client scaling mode.");
        qCritical("%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }
    m_ResolvedScalingMode = scalingMode;
    bool matchedExactly = true;
    if (layoutPolicy == NvOutputTopology::MatchClientHostLayout) {
        QVector<NvClientDisplay> displays;
        QVector<NvClientDisplay> probedDisplays;
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            const QRect logical(display.logicalBounds.x, display.logicalBounds.y,
                                display.logicalBounds.w, display.logicalBounds.h);
            displays.append({display.macMatchedBounds.isValid() ? display.macMatchedBounds : logical,
                             display.nativeSize, display.macBackingSize});
            probedDisplays.append(display.probeView);
        }

        QString error;
        if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
            int scale = 1;
            const QString mode = NvOutputTopology::resolveMacClientDisplayMode(displays, &error, &scale);
            if (mode.isEmpty() || !MacSharedSession::acceptsDisplay(topologySnapshot,
                    NvOutputTopology::macDisplayModeSize(mode), scale,
                    topologySnapshot.appleEncodingMode, sharedMacDesktop)) {
                emit displayLaunchError(mode.isEmpty() ? error : tr("Client displays changed during connection. Please reconnect to match the current display resolution."));
                return false;
            }
            m_ResolvedHostLayout = QStringLiteral("fixed");
        }
        else {
            // Linux host, planned from the display setup: with the display
            // arrangement extension every monitor at its own size where the
            // setup puts it; without it the primary and one side neighbour
            // from the qualified modes, as the setup previewed.
            if (!planDisplayArrangement(probedDisplays,
                                        arrangementPublished ? hostFeatureFlags :
                                                               hostFeatureFlags & ~NvOutputTopology::DisplayArrangementFeature,
                                        topologySnapshot, matchedExactly)) {
                return false;
            }
        }
    }
    else if (layoutPolicy == QStringLiteral("fixed") &&
             m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT &&
             m_Computer->outputTopology.featureFlags == NvOutputTopology::FixedCaptureFlags) {
        m_ResolvedHostLayout = layoutPolicy;
    }
    else if (layoutPolicy == NvOutputTopology::PhysicalHostLayout) {
        m_ResolvedHostLayout = NvOutputTopology::PhysicalHostLayout;
    }
    else if (layoutPolicy == NvOutputTopology::SingleHostLayout ||
             layoutPolicy == NvOutputTopology::DualHorizontalHostLayout) {
        if (!NvOutputTopology::qualifiedVirtualModes().contains(virtualMode1) ||
                (layoutPolicy == NvOutputTopology::DualHorizontalHostLayout &&
                 !NvOutputTopology::qualifiedVirtualModes().contains(virtualMode2))) {
            const QString error = tr("The bookmark contains an unsupported virtual monitor resolution.");
            qCritical("%s", qPrintable(error));
            emit displayLaunchError(error);
            return false;
        }
        m_ResolvedHostLayout = layoutPolicy;
        m_ResolvedVirtualModes.append(virtualMode1);
        if (layoutPolicy == NvOutputTopology::DualHorizontalHostLayout) {
            m_ResolvedVirtualModes.append(virtualMode2);
            if (virtualPrimary) {
                QVector<NvClientDisplay> displays;
                for (const auto& display : std::as_const(m_ClientDisplays)) {
                    NvClientDisplay probed = display.probeView;
                    probed.bounds = QRect(display.logicalBounds.x, display.logicalBounds.y,
                                          display.logicalBounds.w, display.logicalBounds.h);
                    probed.nativeSize = display.nativeSize;
                    probed.backingSize = display.macBackingSize;
                    probed.main = display.primary;
                    displays.append(probed);
                }
                // A manual bookmark remains usable with a different number
                // or arrangement of local monitors. Omit an ambiguous hint;
                // never send an index into unrelated local displays.
                m_ResolvedPrimaryOutput = NvOutputTopology::clientPrimaryIndex(
                    displays, m_ResolvedVirtualModes.size());
            }
        }
    }
    else {
        const QString error = tr("The bookmark contains an unsupported host display layout.");
        qCritical("%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }
    for (const QString& mode : std::as_const(m_ResolvedVirtualModes)) {
        if (!NvOutputTopology::hostAcceptsVirtualMode(mode, hostFeatureFlags)) {
            const QString error = tr("This workstation does not support %1 yet. Update BDE fernweh on the workstation or choose another resolution.")
                    .arg(QString(mode).replace(QLatin1Char('x'), QChar(0x00D7)));
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
            emit displayLaunchError(error);
            return false;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK host layout: policy=%s resolved=%s modes=%s arrangement=%s scaling=%s%s",
                qPrintable(layoutPolicy), qPrintable(m_ResolvedHostLayout),
                qPrintable(m_ResolvedVirtualModes.join(',')),
                qPrintable(m_ResolvedArrangement.isEmpty() ? QStringLiteral("-") : m_ResolvedArrangement),
                qPrintable(m_ResolvedScalingMode), matchedExactly ? "" : " fitted");
    return true;
}

DisplayProfile::Resolved Session::displayProfileFor(const QVector<NvClientDisplay>& displays) const
{
    QString hostId;
    {
        QReadLocker lock(&m_Computer->lock);
        hostId = m_Computer->brokerHostId;
    }
    // The display setup saves the profile before it connects; a LAN bookmark
    // (no workstation id) uses the global profile for these monitors.
    QSettings settings;
    DisplayProfile::Resolved resolved =
            DisplayProfile::resolveForHost(settings, hostId, ClientDisplayProbe::fingerprint(displays));
    if (resolved.source == DisplayProfile::Resolved::None) {
        resolved.profile = DisplayPlanner::proposal(displays);
    }
    return resolved;
}

bool Session::planDisplayArrangement(const QVector<NvClientDisplay>& displays, int hostFeatureFlags,
                                     const NvOutputTopology& topology, bool& matchedExactly)
{
    DisplayPlanner::HostInfo host;
    host.known = true;
    host.platform = 1;
    host.featureFlags = hostFeatureFlags;
    host.capabilities = topology.displayCapabilities;
    host.encodingMode = StreamingPreferences::plankEncodingMode(m_PlankVideoProfile);
    DisplayPlanner::Limits limits;
#ifdef Q_OS_DARWIN
    limits.separateSpaces = MacDisplayInfo::screensHaveSeparateSpaces();
    // Never an 8K stream this Mac would decode in software.
    limits.decoderMaximum = DecoderCaps::maximum(host.encodingMode);
#endif
#ifdef PLANK_QT_SESSION
    limits.separateWindows = true;
#else
    const char* driver = SDL_GetCurrentVideoDriver();
    // Cocoa can prepare hidden per-display windows even when starting windowed.
    limits.separateWindows = driver != nullptr &&
            (strcmp(driver, "cocoa") == 0 || (m_IsFullScreen && strcmp(driver, "wayland") == 0));
#endif
    DisplayProfile::Resolved resolved = displayProfileFor(displays);
    if (resolved.source == DisplayProfile::Resolved::Legacy) {
        // Keep the saved Match client request on the qualified older modes,
        // but still plan its local windows. The old direct resolver bypassed
        // presentation entirely and left Cocoa showing one combined surface.
        host.featureFlags &= ~NvOutputTopology::DisplayArrangementFeature;
        resolved.profile = DisplayPlanner::proposal(displays);
    }
    m_DisplayPlan = DisplayPlanner::plan(displays, resolved.profile, host, limits);
    for (const DisplayPlanner::Output& output : std::as_const(m_DisplayPlan.outputs)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK display plan: client=%dx%d%+d%+d exact=%dx%d on=%d entry=%d size=%dx%d%+d%+d backing=%s%s badge=%s%s",
                    output.clientBounds.width(), output.clientBounds.height(),
                    output.clientBounds.x(), output.clientBounds.y(),
                    output.exactSize.width(), output.exactSize.height(), output.on ? 1 : 0,
                    output.arrangementIndex, output.size.width(), output.size.height(),
                    output.position.x(), output.position.y(),
                    qPrintable(DisplayArrangement::backingName(output.backing)),
                    m_DisplayPlan.backingExpected ? " (expected)" : "",
                    qPrintable(output.badge), output.primary ? " primary" : "");
    }
    for (const DisplayPlanner::Warning& warning : std::as_const(m_DisplayPlan.warnings)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PLANK display plan warning: %s", qPrintable(warning.code));
    }
    if (!m_DisplayPlan.ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(m_DisplayPlan.error));
        emit displayLaunchError(m_DisplayPlan.error);
        return false;
    }
    if (m_DisplayPlan.legacy) {
        // No extension: today's layout request for the planned displays.
        m_ResolvedHostLayout = m_DisplayPlan.legacyHostLayout;
        m_ResolvedVirtualModes = m_DisplayPlan.legacyModes;
        m_ResolvedPrimaryOutput = DisplayPlanner::legacyPrimaryOutput(m_DisplayPlan,
                    topology.startupLayoutKind == NvOutputTopology::SingleHostLayout &&
                    (hostFeatureFlags & NvOutputTopology::VirtualPrimaryConnectorFeature) != 0);
        matchedExactly = !m_DisplayPlan.legacyFitted;
        if (m_DisplayPlan.legacyFitted && m_ResolvedScalingMode != NvOutputTopology::ScaledSpanMode) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PLANK match client fitted a closest supported mode; scaling to fit");
            m_ResolvedScalingMode = NvOutputTopology::ScaledSpanMode;
        }
        return true;
    }
    m_ResolvedHostLayout = QStringLiteral("arrangement");
    m_ResolvedArrangement = m_DisplayPlan.arrangement;
    // The stream is the workstation desktop 1:1; presentation fits it into
    // the windows when they differ.
    m_ResolvedScalingMode = NvOutputTopology::NativeScalingMode;
    matchedExactly = true;
    for (const DisplayPlanner::Output& output : std::as_const(m_DisplayPlan.outputs)) {
        matchedExactly = matchedExactly && !output.scaled;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK display arrangement: request=%s canvas=%dx%d capture=%dx%d%s presentation=%s profile=%s",
                qPrintable(m_ResolvedArrangement), m_DisplayPlan.canvas.width(), m_DisplayPlan.canvas.height(),
                m_DisplayPlan.capture.width(), m_DisplayPlan.capture.height(), m_DisplayPlan.packed ? " packed" : "",
                qPrintable(m_DisplayPlan.presentation),
                resolved.source == DisplayProfile::Resolved::HostCustom ? "workstation" :
                resolved.source == DisplayProfile::Resolved::Global ? "saved" : "proposal");
    return true;
}

void Session::applyArrangementPresentation()
{
    // Which client display shows which planned workstation display. The plan
    // was made from these very snapshots (probeView), so bounds identify them.
#ifdef PLANK_QT_SESSION
    const bool separateWindows = true;
#else
    const char* driver = SDL_GetCurrentVideoDriver();
    const bool separateWindows = driver != nullptr &&
            (strcmp(driver, "cocoa") == 0 || (m_IsFullScreen && strcmp(driver, "wayland") == 0));
#endif
    QVector<NvClientDisplay> displays;
    for (const auto& display : std::as_const(m_ClientDisplays)) displays.append(display.probeView);
    const auto targets = DisplayPlanner::presentationTargets(m_DisplayPlan, displays, separateWindows);
    int shown = 0;
    for (int index = 0; index < m_ClientDisplays.size(); ++index) {
        m_ClientDisplays[index].planIndex = targets.planIndices.at(index);
        shown += targets.planIndices.at(index) >= 0 ? 1 : 0;
    }
    const SDL_DisplayID primary = targets.primaryDisplay >= 0 ?
                m_ClientDisplays.at(targets.primaryDisplay).displayId : 0;
    const bool multi = targets.separateWindows;
    m_PlannedPrimaryDisplay = primary;
    if (m_Window != nullptr) {
        // The windows exist (in-session reconnect on another thread): the SDL
        // thread applies the new presentation when the reconnect finishes.
        m_PendingMultiDisplayPresentation = multi;
        m_PresentationChangePending.store(true);
    } else {
        m_UseMultiDisplayPresentation = multi;
        if (primary != 0) {
            m_TargetDisplayId = primary;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK arrangement presentation: displays=%d primary=%u windows=%s%s",
                shown, primary, multi ? "one per display" : "single",
                m_Window != nullptr ? " (after reconnect)" : "");
}

bool Session::topologyMatchesRequest(const NvOutputTopology& topology) const
{
    if (!m_ResolvedArrangement.isEmpty()) {
        return topology.matchesRequestedArrangement(m_ResolvedArrangement);
    }
    return topology.matchesRequestedHostLayout(m_ResolvedHostLayout, m_ResolvedVirtualModes);
}

QSize Session::arrangementStreamLimit() const
{
    QSize limit;
    const QString mode = StreamingPreferences::plankEncodingMode(m_PlankVideoProfile);
    NvOutputTopology topology;
    {
        QReadLocker lock(&m_Computer->lock);
        topology = m_Computer->outputTopology;
    }
    const auto entry = topology.displayCapabilities.encodingLimits.constFind(mode);
    if (entry != topology.displayCapabilities.encodingLimits.constEnd() && entry->maximum.isValid()) {
        limit = entry->maximum;
    }
#ifdef Q_OS_DARWIN
    // Never a stream this Mac would decode in software.
    const QSize decoder = DecoderCaps::maximum(mode);
    if (decoder.isValid()) {
        limit = limit.isValid() ? limit.boundedTo(decoder) : decoder;
    }
#endif
    return limit;
}

QSize Session::arrangementCaptureSize() const
{
    // The host's capture once it shows this arrangement (it may pack the
    // outputs into rows); until then the capture the plan expects.
    NvOutputTopology topology;
    {
        QReadLocker lock(&m_Computer->lock);
        topology = m_Computer->outputTopology;
    }
    if (topology.matchesRequestedArrangement(m_ResolvedArrangement)) {
        return topology.captureSize();
    }
    return m_DisplayPlan.capture.isValid() ? m_DisplayPlan.capture : m_DisplayPlan.canvas;
}

QSize Session::configurePlankDisplayMode()
{
    if (!m_ResolvedArrangement.isEmpty()) {
        // The host's capture, 1:1: the desktop, or its outputs packed into
        // rows. The plan already fitted it to the host's canvas, its encoder
        // and this client's decoder.
        QString error;
        const QSize capture = arrangementCaptureSize();
        const QSize selected = PlankDisplayMode::resolveClient(capture, arrangementStreamLimit(), &error);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK client physical resolution: arrangement desktop=%dx%d capture=%dx%d%s selected=%dx%d resolution-policy=capture",
                    m_DisplayPlan.canvas.width(), m_DisplayPlan.canvas.height(),
                    capture.width(), capture.height(), m_DisplayPlan.packed ? " (packed)" : "",
                    selected.width(), selected.height());
        if (!selected.isValid()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
            emit displayLaunchError(error);
        }
        return selected;
    }

    // The selected match targets, not every connected panel: discovery also
    // retains monitors excluded by a bookmark or a saved display profile.
    const QSize detectedResolution = PlankDisplayMode::detectedClientResolution(
        m_ClientDisplays, m_TargetDisplayId, m_UseMultiDisplayPresentation, m_DisplayPlan.ok);

    QSize nativeCanvasResolution;
    {
        QReadLocker lock(&m_Computer->lock);
        if (m_ResolvedHostLayout == NvOutputTopology::PhysicalHostLayout ||
                m_ResolvedHostLayout == QStringLiteral("fixed")) {
            nativeCanvasResolution = QSize(m_Computer->outputTopology.desktopWidth,
                                           m_Computer->outputTopology.desktopHeight);
        }
    }
    if (m_ResolvedHostLayout != NvOutputTopology::PhysicalHostLayout &&
            m_ResolvedHostLayout != QStringLiteral("fixed")) {
        nativeCanvasResolution = NvOutputTopology::virtualCanvasSize(
                    m_ResolvedHostLayout, m_ResolvedVirtualModes);
    }

    QSize selectedResolution;
    if (m_ResolvedScalingMode == NvOutputTopology::NativeScalingMode ||
            m_ResolvedHostLayout == QStringLiteral("fixed")) {
        if (!nativeCanvasResolution.isValid()) {
            const QString error = tr("Native scaling requires a valid host desktop pixel size.");
            qCritical("%s", qPrintable(error));
            emit displayLaunchError(error);
            return QSize();
        }
        selectedResolution = nativeCanvasResolution;
    }
    else {
        selectedResolution = PlankDisplayMode::resolve(
            detectedResolution, nativeCanvasResolution);
    }
    qInfo("PLANK client physical resolution: detected=%dx%d host-native=%dx%d selected=%dx%d scaling=%s resolution-policy=%s",
                detectedResolution.width(), detectedResolution.height(),
                nativeCanvasResolution.width(), nativeCanvasResolution.height(),
                selectedResolution.width(), selectedResolution.height(),
                qPrintable(m_ResolvedScalingMode),
                m_ResolvedScalingMode == NvOutputTopology::NativeScalingMode ?
                    "host-native" : "client-native");
    return selectedResolution;
}

bool Session::configurePlankLaunchGeometry(bool resolveProfile)
{
    if (isCollaborator()) return true;
    if (resolveProfile && m_Computer->plankAuthentication &&
            !configurePlankHostLayout()) {
        return false;
    }

    if (m_DisplayPlan.ok) {
        // Preserve the selected subset, primary and crops. The older counter
        // returns zero for arrangement; its client check also rejects a third
        // connected screen even when the user excludes it from the session.
        applyArrangementPresentation();
    } else {
        QReadLocker lock(&m_Computer->lock);
        const int hostOutputs = m_Computer->outputTopology.outputCountForLayout(m_ResolvedHostLayout);
        m_UseMultiDisplayPresentation = m_MultiDisplayPresentationAvailable;
#ifdef Q_OS_DARWIN
        // The Mac single-output policy is qualified independently of Wayland.
        m_UseMultiDisplayPresentation = m_UseMultiDisplayPresentation && hostOutputs > 1;
#endif
        qInfo("PLANK presentation selection: host-outputs=%d client-multi=%d selected-outputs=%d",
                    hostOutputs, m_MultiDisplayPresentationAvailable,
                    m_UseMultiDisplayPresentation ? 2 : 1);
    }

    const QSize resolution = configurePlankDisplayMode();
    if (!resolution.isValid()) {
        return false;
    }

    const QSize previousResolution(m_StreamConfig.width,
                                   m_StreamConfig.height);
    m_StreamConfig.width = resolution.width();
    m_StreamConfig.height = resolution.height();

    if (previousResolution.isValid() && previousResolution != resolution) {
        qInfo() << "PLANK launch geometry changed from"
                << previousResolution << "to" << resolution;
    }
    // The GUI must select and expose the actual profile outputs before the
    // exact-format decoder probe starts. No GUI object crosses to this worker.
    return !m_QtPreparePresentation || m_QtPreparePresentation();
}

void Session::getWindowDimensions(int& x, int& y,
                                  int& width, int& height)
{
    const int displayIndex = getTargetDisplayIndex();

    SDL_Rect usableBounds;
    if (m_UseMultiDisplayPresentation && m_DisplayPlan.ok &&
            SDL_GetDisplayUsableBounds(StreamUtils::getDisplayId(displayIndex), &usableBounds)) {
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            if (display.displayId == m_TargetDisplayId && display.planIndex >= 0) {
                const QRect bounds = PlankPresentation::windowedOutputRect(
                            QRect(usableBounds.x, usableBounds.y, usableBounds.w, usableBounds.h),
                            m_DisplayPlan.outputs.at(display.planIndex).size);
                x = bounds.x(); y = bounds.y(); width = bounds.width(); height = bounds.height();
                return;
            }
        }
    }
    if (SDL_GetDisplayUsableBounds(StreamUtils::getDisplayId(displayIndex), &usableBounds)) {
        // Don't use more than 80% of the display to leave room for system UI
        // and ensure the target size is not odd (otherwise one of the sides
        // of the image will have a one-pixel black bar next to it).
        SDL_Rect src, dst;
        src.x = src.y = dst.x = dst.y = 0;
        src.w = m_StreamConfig.width;
        src.h = m_StreamConfig.height;
        dst.w = ((int)SDL_ceilf(usableBounds.w * 0.80f) & ~0x1);
        dst.h = ((int)SDL_ceilf(usableBounds.h * 0.80f) & ~0x1);

        // Scale the window size while preserving aspect ratio
        StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

        // If the stream window can fit within the usable drawing area with 1:1
        // scaling, do that rather than filling the screen.
        if (m_StreamConfig.width < dst.w && m_StreamConfig.height < dst.h) {
            width = m_StreamConfig.width;
            height = m_StreamConfig.height;
        }
        else {
            width = dst.w;
            height = dst.h;
        }
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_GetDisplayUsableBounds() failed: %s",
                     SDL_GetError());

        width = m_StreamConfig.width;
        height = m_StreamConfig.height;
    }

    x = y = SDL_WINDOWPOS_CENTERED_DISPLAY(StreamUtils::getDisplayId(displayIndex));
}

void Session::updateOptimalWindowDisplayMode()
{
    bool preserveDesktopMode = strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0;
#ifdef Q_OS_DARWIN
    preserveDesktopMode = true;
#endif
    // A PLANK Wayland session is a desktop surface, not a monitor
    // mode switch. Let the compositor size the fullscreen surface and keep
    // SDL's window and pointer coordinates in the same space. SDL 3.4.2 can
    // otherwise retain the pre-fullscreen viewport for pointer events while
    // exposing the fullscreen mode dimensions through SDL_GetWindowSize().
    // Our renderer already performs the required aspect scaling and
    // letterboxing, so an exclusive Wayland display mode adds no value.
    // Mac fullscreen also preserves the current desktop mode; exclusive-mode
    // selection must not change the user's Retina scale, including manual
    // remote resolutions and connections to Linux hosts.
    if (preserveDesktopMode) {
        if (!SDL_SetWindowFullscreenMode(m_Window, nullptr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to select desktop fullscreen mode: %s",
                        SDL_GetError());
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Using compositor-native desktop fullscreen mode");
        }
        return;
    }

    SDL_DisplayMode desktopMode, bestMode, mode;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(m_Window);
    const int displayIndex = StreamUtils::getDisplayIndex(display);

    // Try the current display mode first. On macOS, this will be the normal
    // scaled desktop resolution setting.
    if (const SDL_DisplayMode* queriedDesktopMode = SDL_GetDesktopDisplayMode(display)) {
        desktopMode = *queriedDesktopMode;
        // If this doesn't fit the selected resolution, use the native
        // resolution of the panel (unscaled).
        if (desktopMode.w < m_ActiveVideoWidth || desktopMode.h < m_ActiveVideoHeight) {
            SDL_Rect safeArea;
            if (!StreamUtils::getNativeDesktopMode(displayIndex, &desktopMode, &safeArea)) {
                return;
            }
        }
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_GetDesktopDisplayMode() failed: %s",
                    SDL_GetError());
        return;
    }

    // Start with the native desktop resolution and try to find
    // the highest refresh rate that our stream FPS evenly divides.
    bestMode = desktopMode;
    bestMode.refresh_rate = 0;
    for (int i = 0; i < StreamUtils::getDisplayModeCount(displayIndex); i++) {
        if (StreamUtils::getDisplayMode(displayIndex, i, &mode)) {
            if (mode.w == desktopMode.w && mode.h == desktopMode.h &&
                    qRound(mode.refresh_rate) % m_StreamConfig.fps == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Found display mode with desktop resolution: %dx%dx%d",
                            mode.w, mode.h, qRound(mode.refresh_rate));
                if (mode.refresh_rate > bestMode.refresh_rate) {
                    bestMode = mode;
                }
            }
        }
    }

    // If we didn't find a mode that matched the current resolution and
    // had a high enough refresh rate, start looking for lower resolution
    // modes that can meet the required refresh rate and minimum video
    // resolution. We will also try to pick a display mode that matches
    // aspect ratio closest to the video stream.
    if (bestMode.refresh_rate == 0) {
        float bestModeAspectRatio = 0;
        float videoAspectRatio = (float)m_ActiveVideoWidth / (float)m_ActiveVideoHeight;
        for (int i = 0; i < StreamUtils::getDisplayModeCount(displayIndex); i++) {
            if (StreamUtils::getDisplayMode(displayIndex, i, &mode)) {
                float modeAspectRatio = (float)mode.w / (float)mode.h;
                if (mode.w >= m_ActiveVideoWidth && mode.h >= m_ActiveVideoHeight &&
                        qRound(mode.refresh_rate) % m_StreamConfig.fps == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Found display mode with video resolution: %dx%dx%d",
                                mode.w, mode.h, qRound(mode.refresh_rate));
                    if (mode.refresh_rate >= bestMode.refresh_rate &&
                            (bestModeAspectRatio == 0 || fabs(videoAspectRatio - modeAspectRatio) <= fabs(videoAspectRatio - bestModeAspectRatio))) {
                        bestMode = mode;
                        bestModeAspectRatio = modeAspectRatio;
                    }
                }
            }
        }
    }

    if (bestMode.refresh_rate == 0) {
        // We may find no match if the user has moved a 120 FPS
        // stream onto a 60 Hz monitor (since no refresh rate can
        // divide our FPS setting). We'll stick to the default in
        // this case.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "No matching display mode found; using desktop mode");
        bestMode = desktopMode;
    }

    if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN) {
        // Only print when the window is actually in full-screen exclusive mode,
        // otherwise we're not actually using the mode we've set here
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Chosen best display mode: %dx%dx%d",
                    bestMode.w, bestMode.h, qRound(bestMode.refresh_rate));
    }

    SDL_SetWindowFullscreenMode(m_Window, &bestMode);
}

void Session::togglePlankToolbar()
{
    if (m_PlankToolbar) {
        m_PlankToolbar->toggleFromShortcut(SDL_GetTicks());
    }
}

void Session::toggleFullscreen()
{
    bool fullScreen = !(SDL_GetWindowFlags(m_Window) & m_FullScreenFlag);

    bool recreateDecoder = m_UseMultiDisplayPresentation;
#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN)
    // Destroy the video decoder before toggling full-screen because D3D9 can try
    // to put the window back into full-screen before we've managed to destroy
    // the renderer. This leads to excessive flickering and can cause the window
    // decorations to get messed up as SDL and D3D9 fight over the window style.
    //
    // On Apple Silicon Macs, the AVSampleBufferDisplayLayer may cause WindowServer
    // to deadlock when transitioning out of fullscreen. Destroy the decoder before
    // exiting fullscreen as a workaround. See issue #973.
    recreateDecoder = true;
#endif
    if (recreateDecoder) {
        std::lock_guard<std::mutex> lock(m_DecoderLock);
        delete m_VideoDecoder;
        m_VideoDecoder = nullptr;
    }

    // Actually enter/leave fullscreen
    setPresentationWindowsFullscreen(fullScreen);

    // Input handler might need to start/stop keyboard grab after changing modes
    m_InputHandler->updateKeyboardGrabState();

    // Input handler might need stop/stop mouse grab after changing modes
    m_InputHandler->updatePointerRegionLock();

    // Qt dispatches its reset signal; the SDL event loop repairs a missing
    // decoder directly, even without a resize or room in the event queue.
#ifdef PLANK_QT_SESSION
    if (recreateDecoder) requestDecoderReset();
#endif
}

class AsyncConnectionStartThread : public QThread
{
public:
    AsyncConnectionStartThread(Session* session) :
        QThread(nullptr),
        m_Session(session)
    {
        setObjectName("Async Conn Start");
    }

    void run() override
    {
        m_Session->m_AsyncConnectionSuccess = m_Session->startConnectionAsync();
    }

    Session* m_Session;
};

// Called in a non-main thread
bool Session::startConnectionAsync(bool reconnecting,
                                   bool takeOverActiveSession)
{
    // Wait 1.5 seconds before connecting to let the user
    // have time to read any messages present on the segue
    if (!reconnecting && !takeOverActiveSession) {
        for (int elapsed = 0; elapsed < 1500; elapsed += 50) {
            if (m_Lifecycle.stopping()) return false;
            QThread::msleep(50);
        }
    }

    // PLANK never terminates a host application remotely. Only resume
    // the already-running Desktop application or launch it from an idle host.
    Q_ASSERT(m_Computer->currentGameId == 0 ||
             m_Computer->currentGameId == m_App.id);

    quint16 plankTransportPort = 0;
    QString plankTransportCertificateSha256;
    QString plankTransportToken;
    QString acceptedCaptureSource;
    QString acceptedEncoderBackend;
    QString acceptedEncodingMode;
    QString acceptedFileClipboardMode {QStringLiteral("off")};
    bool acceptedClipboardSync = true;
    const bool macCapture = m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT;
    MacPreviewLaunch::Reply macLaunch;
    quint32 routeInterfaceMtu = 0;
    bool routeIsIpv6 = false;
    const NvComputer::ReachabilityType routeReachability =
            m_Computer->getActiveAddressReachability(&routeInterfaceMtu, &routeIsIpv6);
    const int configuredMtu = m_Preferences->quicUdpPayloadMtu;
    const quint16 quicUdpPayloadMtu = PlankNetwork::quicUdpPayloadMtuForRoute(
                configuredMtu, routeReachability == NvComputer::RI_ZEROTIER,
                routeInterfaceMtu, routeIsIpv6);
    if (quicUdpPayloadMtu == 0) {
        const quint32 overhead = routeIsIpv6 ? PlankNetwork::InnerIpv6UdpOverhead :
                                              PlankNetwork::InnerIpv4UdpOverhead;
        const quint32 minimumMtu = PlankNetwork::MinimumQuicUdpPayloadMtu +
                overhead + PlankNetwork::AutomaticPathSafetyMargin;
        if (routeInterfaceMtu && routeInterfaceMtu < minimumMtu) {
            emit displayLaunchError(tr("The network interface MTU is %1 bytes. BDE fernweh requires at least %2 bytes for %3, including protocol headers and its safety margin.")
                                    .arg(routeInterfaceMtu).arg(minimumMtu)
                                    .arg(routeIsIpv6 ? QStringLiteral("IPv6") : QStringLiteral("IPv4")));
        }
        else {
            emit displayLaunchError(tr("The configured QUIC UDP payload (%1 bytes) is invalid or exceeds the network interface's safe payload limit. Choose Automatic or a smaller value of at least 1200 bytes.")
                                    .arg(configuredMtu));
        }
        qWarning() << "Rejected QUIC UDP payload: configured=" << configuredMtu
                   << "interface MTU=" << routeInterfaceMtu
                   << (routeIsIpv6 ? "IPv6" : "IPv4");
        return false;
    }
    qInfo() << (configuredMtu ? "Manual fixed QUIC UDP payload ceiling:" :
                              "Automatically resolved fixed QUIC UDP payload ceiling:")
            << quicUdpPayloadMtu << "bytes from interface MTU" << routeInterfaceMtu
            << (routeIsIpv6 ? "(IPv6)" : "(IPv4)")
            << "ZeroTier=" << (routeReachability == NvComputer::RI_ZEROTIER);

    try {
        std::unique_ptr<NvHTTP> http = std::make_unique<NvHTTP>(m_Computer);
        if (reconnecting) http->setRequestGate([this](bool auth) { return waitForPlankReconnectRequest(auth); });
        const QString captureSource =
                m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT ?
                    QStringLiteral("screencapturekit") :
                m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10 ?
                    QStringLiteral("x11-native10") : QStringLiteral("nvfbc");
        const QString encoderBackend =
                StreamingPreferences::isPlankAppleProfile(m_PlankVideoProfile) ?
                    QStringLiteral("videotoolbox") :
                StreamingPreferences::isPlankNvencProfile(
                    m_PlankVideoProfile) ?
                    QStringLiteral("nvenc-direct") : QStringLiteral("software-cuda");
        const QString encodingMode = StreamingPreferences::plankEncodingMode(m_PlankVideoProfile);
        if (encodingMode.isEmpty()) {
            emit displayLaunchError(tr("The bookmark contains an invalid encoding profile."));
            return false;
        }
        QString desktopSignOutOwner;
        const auto startApp = [&]() {
            if (macCapture) {
                QString pin;
                const NvOutputTopology topology = http->getOutputTopology(&pin);
                if (topology.toJson() != m_Computer->outputTopology.toJson() ||
                        topology.desktopWidth != m_StreamConfig.width ||
                        topology.desktopHeight != m_StreamConfig.height) {
                    throw GfeHttpResponseException(409, "The Mac display changed. Reconnect to refresh its capture geometry.");
                }
                // Launch is one-shot, including an ambiguous network timeout.
                // Readiness failures above keep the setup context instead.
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->sessionToken.fill(QChar('\0'));
                    m_Computer->sessionToken.clear();
                    m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
                }
                macLaunch = http->startMacPreview(topology, pin, m_StreamConfig.bitrate, quicUdpPayloadMtu);
                m_MacClipboardNegotiated = macLaunch.clipboard;
                m_MicrophoneNegotiated = macLaunch.microphone;
                m_MicrophoneSchema = macLaunch.microphoneSchema;
                m_CameraNegotiated = macLaunch.camera;
                plankTransportPort = http->controlPort();
                plankTransportCertificateSha256 = pin;
                plankTransportToken = QString::fromLatin1(macLaunch.transportToken);
                macLaunch.transportToken.fill('\0');
                macLaunch.transportToken.clear();
                acceptedCaptureSource = captureSource;
                acceptedEncoderBackend = encoderBackend;
                acceptedEncodingMode = encodingMode;
                acceptedFileClipboardMode = QStringLiteral("off");
                acceptedClipboardSync = macLaunch.clipboard;
                return;
            }
            http->startApp(PlankCollaboration::launchVerb(isCollaborator(), m_Computer->currentGameId != 0),
                          m_App.id, &m_StreamConfig,
                          m_Preferences->playAudioOnHost,
                          NvOutputTopology::ScaledSpanMode,
                          m_Computer->outputTopology.generation,
                          m_Computer->plankTopologyVersion,
                          m_PlankLaunchFeatureFlags,
                          takeOverActiveSession,
                          desktopSignOutOwner,
                          m_ResolvedHostLayout,
                          m_ResolvedVirtualModes.value(0),
                          m_ResolvedVirtualModes.value(1),
                          m_ResolvedArrangement,
                          captureSource,
                          encoderBackend,
                          encodingMode,
                          quicUdpPayloadMtu,
                          plankTransportPort,
                          plankTransportCertificateSha256,
                          plankTransportToken,
                          acceptedCaptureSource,
                          acceptedEncoderBackend,
                          acceptedEncodingMode,
                          acceptedFileClipboardMode,
                          acceptedClipboardSync,
                          m_ResolvedPrimaryOutput);
        };
        try {
            startApp();
        } catch (const GfeHttpResponseException& e) {
            const QString statusMessage = QString::fromUtf8(e.getStatusMessage());
            const QString arrangementError = http->displayArrangementError();
            if (m_Computer->plankAuthentication && !arrangementError.isEmpty() &&
                    (e.getStatusCode() == 400 || e.getStatusCode() == 409)) {
                // Also a legacy layout's stream the host's encoder cannot
                // carry (400 canvas_too_large with the bit negotiated).
                // The workstation cannot show this arrangement, now or ever:
                // say why instead of waiting or retrying.
                NvOutputTopology topology;
                {
                    QReadLocker lock(&m_Computer->lock);
                    topology = m_Computer->outputTopology;
                }
                const QString error = DisplayPlanner::errorText(arrangementError, topology.displayCapabilities);
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PLANK display arrangement refused (%d %s): %s",
                             e.getStatusCode(), qPrintable(arrangementError),
                             qPrintable(m_ResolvedArrangement.isEmpty() ? m_ResolvedHostLayout : m_ResolvedArrangement));
                if (reconnecting) {
                    m_Lifecycle.stop();
                }
                emit displayLaunchError(error);
                return false;
            }
            const bool displayTransitionStarted =
                    m_Computer->plankAuthentication &&
                    ((e.getStatusCode() == 425 &&
                      (statusMessage ==
                           QStringLiteral("PLANK host display transition started") ||
                       !m_ResolvedArrangement.isEmpty())) ||
                     (takeOverActiveSession &&
                      e.getStatusCode() == 503 &&
                      statusMessage ==
                          QStringLiteral("Host display layout transition is currently unavailable")));
            const bool activeSessionConflict =
                    m_Computer->plankAuthentication &&
                    e.getStatusCode() == 409 &&
                    QString::fromUtf8(e.getStatusMessage()) ==
                        QStringLiteral("PLANK workstation session is active");
            const PlankDesktopSignOut desktopConflict =
                    m_Computer->plankAuthentication ? http->desktopSignOut() :
                                                      PlankDesktopSignOut();
            if (desktopConflict.state != PlankDesktopSignOut::State::None) {
                if (!resolvePlankDesktopConflict(http, startApp, desktopConflict, e,
                                                 reconnecting, desktopSignOutOwner)) {
                    return false;
                }
            }
            else if (displayTransitionStarted) {
                constexpr int RetryIntervalMs = 500;
                // A host keeps a pending arrangement for up to 120 s (a
                // greeter restart may be part of it); layouts take 45 s at most.
                const int MaximumWaitMs = m_ResolvedArrangement.isEmpty() ? 45000 : 120000;
                constexpr int CancellationPollMs = 50;
                bool started = false;

                if (!hasPlankCredentials()) {
                    throw;
                }

                emit sessionCleanupWaitChanged(
                            true,
                            tr("Applying workstation display layout..."));
                qInfo() << "PLANK host display transition started; waiting up to"
                        << MaximumWaitMs << "ms";
                PlankDisplayTransitionPolicy transition(MaximumWaitMs);
                QElapsedTimer transitionTimer;
                transitionTimer.start();

                while (!transition.expired(transitionTimer.elapsed()) && !started) {
                    for (int delayMs = 0;
                         delayMs < RetryIntervalMs;
                         delayMs += CancellationPollMs) {
                        if (m_Lifecycle.stopping()) break;
                        QThread::msleep(CancellationPollMs);
                    }
                    if (m_ConnectionStartCancelled.load() ||
                            transition.expired(transitionTimer.elapsed())) break;

                    if (transition.needsAuthentication()) {
                        try {
                            {
                                QWriteLocker lock(&m_Computer->lock);
                                m_Computer->sessionToken.fill(QChar('\0'));
                                m_Computer->sessionToken.clear();
                                m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
                                m_Computer->currentGameId = 0;
                            }
                            http = std::make_unique<NvHTTP>(m_Computer);
                            if (reconnecting) http->setRequestGate([this](bool auth) { return waitForPlankReconnectRequest(auth); });
                            const QString token = authenticatePlank(*http, nullptr);
                            {
                                QWriteLocker lock(&m_Computer->lock);
                                m_Computer->sessionToken = token;
                                m_Computer->sessionIdentityKey = http->hostIdentityKey();
                                m_Computer->plankSharedDesktop = http->sharedDesktop();
                                m_Computer->authorizationState = NvComputer::AS_AUTHORIZED;
                            }
                            transition.authenticated();
                            qInfo() << "PLANK authenticated to the replacement display worker";
                        } catch (const GfeHttpResponseException& retryError) {
                            if (transition.retryAuthentication(retryError.getStatusCode(),
                                                               transitionTimer.elapsed())) {
                                qInfo() << "PLANK replacement display worker admission is not ready:"
                                        << retryError.toQString();
                                continue;
                            }
                            if (reconnecting && PlankReconnectPolicy::terminalStatus(
                                    retryError.getStatusCode(), true,
                                    PlankClock::milliseconds() < m_DesktopHandoffRetryDeadline.load()))
                                m_Lifecycle.stop();
                            throw;
                        } catch (const QtNetworkReplyException& retryError) {
                            if (retryError.getError() == QNetworkReply::SslHandshakeFailedError) throw;
                            qInfo() << "PLANK replacement display worker is not ready for authentication:"
                                    << retryError.toQString();
                            continue;
                        }
                    }

                    if (m_ConnectionStartCancelled.load() ||
                            transition.expired(transitionTimer.elapsed())) break;
                    try {
                        const NvOutputTopology topology = http->getOutputTopology();
                        {
                            QWriteLocker lock(&m_Computer->lock);
                            m_Computer->outputTopology = topology;
                        }
                        if (m_ComputerManager != nullptr) {
                            m_ComputerManager->clientSideAttributeUpdated(m_Computer);
                        }
                        // Poll the request already submitted. Re-reading a
                        // changed saved profile here would wait forever for
                        // a different arrangement the host never received.
                        // Refresh capture dimensions from the new topology.
                        if (!configurePlankLaunchGeometry(false)) {
                            m_WaitingForSessionCleanup.store(false);
                            emit sessionCleanupWaitChanged(false, QString());
                            return false;
                        }
                        if (!m_ResolvedArrangement.isEmpty() &&
                                topology.arrangementRequest == m_ResolvedArrangement &&
                                topology.arrangementState == QLatin1String("failed")) {
                            // The host tried and restored its previous layout.
                            m_WaitingForSessionCleanup.store(false);
                            emit sessionCleanupWaitChanged(false, QString());
                            qWarning() << "PLANK display arrangement failed on the host:"
                                       << topology.arrangementReason;
                            emit displayLaunchError(DisplayPlanner::errorText(
                                        topology.arrangementReason.isEmpty() ? QStringLiteral("apply_failed") :
                                                                               topology.arrangementReason,
                                        topology.displayCapabilities));
                            return false;
                        }
                        if (!topologyMatchesRequest(topology)) {
                            qInfo() << "PLANK display transition is still pending:"
                                    << topology.layoutKind << topology.virtualModes
                                    << topology.arrangementRequest << topology.arrangementState;
                            continue;
                        }
                        if (m_ConnectionStartCancelled.load() ||
                                transition.expired(transitionTimer.elapsed())) break;
                        startApp();
                        started = true;
                    } catch (const GfeHttpResponseException& retryError) {
                        if (!http->displayArrangementError().isEmpty() &&
                                (retryError.getStatusCode() == 400 || retryError.getStatusCode() == 409)) {
                            // Refused while waiting: stop at once with the reason.
                            m_WaitingForSessionCleanup.store(false);
                            emit sessionCleanupWaitChanged(false, QString());
                            NvOutputTopology topology;
                            {
                                QReadLocker lock(&m_Computer->lock);
                                topology = m_Computer->outputTopology;
                            }
                            if (reconnecting) {
                                m_Lifecycle.stop();
                            }
                            emit displayLaunchError(DisplayPlanner::errorText(http->displayArrangementError(),
                                                                              topology.displayCapabilities));
                            return false;
                        }
                        if (retryError.getStatusCode() == 423) {

                            emit sessionCleanupWaitChanged(false, QString());
                            throw;
                        }
                        if (retryError.getStatusCode() == 401) {
                            transition.authenticationLost(transitionTimer.elapsed());
                            qInfo() << "PLANK display worker changed; refreshing authentication within the layout deadline";
                            continue;
                        }
                        if (retryError.getStatusCode() != 409 &&
                                retryError.getStatusCode() != 425 &&
                                retryError.getStatusCode() != 503) {

                            emit sessionCleanupWaitChanged(false, QString());
                            throw;
                        }
                        qInfo() << "PLANK display transition wait attempt failed:"
                                << retryError.toQString();
                    } catch (const QtNetworkReplyException& retryError) {
                        if (retryError.getError() == QNetworkReply::SslHandshakeFailedError) throw;
                        qInfo() << "PLANK display transition worker is not ready:"
                                << retryError.toQString();
                    }
                }

                emit sessionCleanupWaitChanged(false, QString());
                if (!started && m_Lifecycle.stopping()) {
                    qInfo() << "PLANK connection cancelled during display transition";
                    return false;
                }
                if (!started) {
                    if (!reconnecting) {
                        emit displayLaunchError(
                                    tr("The workstation display layout did not become ready within %n second(s).",
                                       nullptr, MaximumWaitMs / 1000));
                    }
                    return false;
                }
                qInfo() << "PLANK display transition completed; launch succeeded";
            }
            else if (activeSessionConflict) {
                if (reconnecting) {
                    // The Host may still be retiring our previous transport.
                    // A busy response does not prove another client took over.
                    // Use the normal bounded/continuous recovery policy; only
                    // an explicit takeover termination ends recovery as such.
                    throw;
                }
                if (takeOverActiveSession ||
                        (m_Computer->plankFeatureFlags &
                         NvOutputTopology::SessionTakeoverFeature) == 0) {
                    emit displayLaunchError(
                                tr("The workstation has an active BDE fernweh session that cannot be transferred."));
                    return false;
                }

                constexpr int DecisionPollMs = 50;
                m_ActiveSessionTakeoverDecision.store(0);
                m_WaitingForActiveSessionTakeoverDecision.store(true);
                emit activeSessionTakeoverRequested(
                            tr("This workstation already has an active BDE fernweh session. Disconnect the existing client and continue?"));
                while (m_ActiveSessionTakeoverDecision.load() == 0 &&
                       !m_Lifecycle.stopping()) {
                    QThread::msleep(DecisionPollMs);
                }
                m_WaitingForActiveSessionTakeoverDecision.store(false);
                if (m_ActiveSessionTakeoverDecision.exchange(0) != 1 ||
                        m_Lifecycle.stopping()) {
                    qInfo() << "PLANK active-session takeover was cancelled";
                    return false;
                }

                emit sessionCleanupWaitChanged(
                            true, tr("Transferring workstation session..."));
                const bool transferred = startConnectionAsync(false, true);

                emit sessionCleanupWaitChanged(false, QString());
                return transferred;
            }
            else if (reconnecting && m_Computer->plankAuthentication &&
                    m_Computer->currentGameId == 0 &&
                    e.getStatusCode() == 400) {
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->currentGameId = m_App.id;
                }
                qInfo() << "PLANK worker already has an active Desktop stream; resuming it";
                startApp();
            }
            else if (reconnecting && m_Computer->plankAuthentication &&
                    m_Computer->currentGameId != 0 &&
                    e.getStatusCode() == 503) {
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->currentGameId = 0;
                }
                qInfo() << "PLANK replacement worker has no app to resume; "
                           "launching a fresh Desktop stream";
                startApp();
            }
            else {
                const bool generationBinding =
                        (m_Computer->plankFeatureFlags &
                         NvOutputTopology::TopologyGenerationFeature) != 0;
                if (!m_Computer->plankAuthentication || !generationBinding ||
                        e.getStatusCode() != 409) {
                    throw;
                }

                const NvOutputTopology topology = http->getOutputTopology();
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->outputTopology = topology;
                }
                if (m_ComputerManager != nullptr) {
                    m_ComputerManager->clientSideAttributeUpdated(m_Computer);
                }
                if (!configurePlankLaunchGeometry()) {
                    return false;
                }
                qInfo() << "PLANK refreshed stale topology and launch geometry; retrying launch:"
                        << topology.generation << m_StreamConfig.width
                        << m_StreamConfig.height;
                startApp();
            }
        }

        // Remote (broker) mode, section 10.2 step 3: QUIC goes to the leased
        // endpoint:port; the host's advertised transport port is ignored and
        // its transport certificate must be the broker-pinned leaf.
        {
            QString brokerPin;
            quint16 leasedPort;
            {
                QReadLocker lock(&m_Computer->lock);
                brokerPin = m_Computer->brokerHostCertSha256;
                leasedPort = m_Computer->activeAddress.port();
            }
            if (!brokerPin.isEmpty()) {
                PlankBroker::TransportTarget target;
                if (!PlankBroker::resolveTransportTarget(brokerPin, leasedPort, plankTransportPort,
                                                         plankTransportCertificateSha256, target)) {
                    throw GfeHttpResponseException(
                                400, "Remote workstation launch did not match the broker admission");
                }
                qInfo() << "PLANK brokered transport: using leased port" << target.port
                        << "instead of host-advertised port" << plankTransportPort;
                plankTransportPort = target.port;
                plankTransportCertificateSha256 = target.certificateSha256;
            }
        }

        m_EffectiveFeatureFlags = macCapture ? m_Computer->plankFeatureFlags : http->effectiveFeatureFlags();
        if (isCollaborator()) {
            const QString permission = http->collaborationPermission();
            m_CollaborationPermission.store(permission == QStringLiteral("control") ?
                                                PlankCollaboration::Control : PlankCollaboration::View);
            // Rejoining always requires another explicit owner decision.
            m_CanReconnect.store(false);
            m_MicrophoneNegotiated = false;
            m_CameraNegotiated = false;
        }

        // Record the successful launch immediately. If low-level transport
        // setup fails afterward, the next bounded attempt must resume this
        // app instead of issuing a second launch request.
        if (m_Computer->plankAuthentication) {
            m_PlankWorkerInstance = (m_Computer->plankFeatureFlags & NvOutputTopology::WorkerInstanceFeature) ?
                        http->workerInstance() : QString();
            m_PlankHostCertificateSha256 = plankTransportCertificateSha256;
            QWriteLocker lock(&m_Computer->lock);
            m_Computer->currentGameId = m_App.id;
        }

        if (m_Computer->plankAuthentication) {
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken.fill(QChar('\0'));
                m_Computer->sessionToken.clear();
                m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
            }
            if (m_ComputerManager != nullptr) {
                m_ComputerManager->clientSideAttributeUpdated(m_Computer);
            }
            qInfo() << "PLANK authentication token consumed after launch";
        }
    } catch (const GfeHttpResponseException& e) {
        if (reconnecting && PlankReconnectPolicy::terminalStatus(e.getStatusCode(), false)) {
            // Operator consent cannot recover through automatic reauthentication.
            m_Lifecycle.stop();
            emit displayLaunchError(e.toQString());
        }

        emit sessionCleanupWaitChanged(false, QString());
        if (reconnecting && e.getStatusCode() == 401) {
            QWriteLocker lock(&m_Computer->lock);
            m_Computer->sessionToken.fill(QChar('\0'));
            m_Computer->sessionToken.clear();
            m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
        }
        if (!reconnecting) {
            emit displayLaunchError(tr("Host returned error: %1").arg(e.toQString()));
        } else {
            qWarning() << "PLANK reconnect launch failed:" << e.toQString();
        }
        return false;
    } catch (const QtNetworkReplyException& e) {
        if (reconnecting && e.getError() == QNetworkReply::SslHandshakeFailedError) {
            m_Lifecycle.stop();
            emit displayLaunchError(e.toQString());
        }

        emit sessionCleanupWaitChanged(false, QString());
        if (!reconnecting) {
            emit displayLaunchError(e.toQString());
        } else {
            qWarning() << "PLANK reconnect transport setup failed:"
                       << e.toQString();
        }
        return false;
    }

#ifdef PLANK_TRANSPORT
    LiSetPlankNativeControlSender(nullptr, nullptr);
    LiSetPlankNativeInputSender(nullptr, nullptr);
#endif
    m_FileClipboardMode = acceptedFileClipboardMode;
    m_SessionClipboardEntitled = acceptedClipboardSync;
    if (!startPlankTransportDataPlane(plankTransportPort,
                                 plankTransportCertificateSha256,
                                 plankTransportToken,
                                 quicUdpPayloadMtu)) {
        plankTransportToken.fill(QChar('\0'));
        if (!reconnecting) {
            emit displayLaunchError(
                        tr("The experimental BDE fernweh data plane could not be established."));
        }
        return false;
    }
    plankTransportToken.fill(QChar('\0'));

    QString nativeNegotiationError;
    const bool negotiated = macCapture ?
                LiSetPlankNativeSessionConfiguration(&macLaunch.configuration) == 0 :
                negotiatePlankTransportSession(plankTransportPort, nativeNegotiationError);
    if (!negotiated) {
        if (macCapture) nativeNegotiationError = tr("The Mac returned an invalid native media configuration.");
        stopPlankTransportDataPlane();
        if (!reconnecting) {
            emit displayLaunchError(nativeNegotiationError);
        }
        else {
            qWarning() << "PLANK native session negotiation failed:"
                       << nativeNegotiationError;
        }
        return false;
    }

    QByteArray hostnameStr = m_Computer->activeAddress.address().toLatin1();
    QByteArray siAppVersion = m_Computer->appVersion.toLatin1();

    SERVER_INFORMATION hostInfo;
    LiInitializeServerInformation(&hostInfo);
    hostInfo.address = hostnameStr.data();
    hostInfo.serverInfoAppVersion = siAppVersion.data();
    hostInfo.serverCodecModeSupport = m_Computer->serverCodecModeSupport;

    // moonlight-common-c fills missing callbacks in the caller-owned table.
    // Restore the pull/push decoder contract before every reuse of this
    // Session for a PLANK desktop handoff.
    m_VideoCallbacks.submitDecodeUnit =
            (m_VideoCallbacks.capabilities & CAPABILITY_PULL_RENDERER) ?
                nullptr : drSubmitDecodeUnit;

    int err = LiStartConnection(&hostInfo, &m_StreamConfig, &k_ConnCallbacks,
                                &m_VideoCallbacks, &m_AudioCallbacks,
                                NULL, 0, NULL, 0);
    if (err != 0) {
        stopPlankTransportDataPlane();
        // We already displayed an error dialog in the stage failure
        // listener.
        return false;
    }

#ifdef PLANK_TRANSPORT
    {
        std::lock_guard<std::mutex> guard(m_MicrophoneMutex);
        if (m_MicrophoneNegotiated) {
            if (!reconnecting) m_MicrophoneRequested.store(m_Preferences->microphoneAutomatic);
            m_Microphone.reset(new PlankMicrophone(m_PlankTransportEndpoint,
                m_MicrophoneRequested, m_Preferences->microphoneAutomaticInput, m_MicrophoneSchema == 3));
        }
    }
    {
        std::lock_guard<std::mutex> guard(m_CameraMutex);
        m_CameraRequested.store(false);
        if (m_CameraNegotiated) m_Camera.reset(new PlankCamera(m_PlankTransportEndpoint,
            m_CameraRequested, m_Preferences->cameraDevice));
    }
    startPlankTransportMediaReceivers();
#endif

    if (!macCapture && (LiGetHostFeatureFlags() & LI_FF_LOCAL_CURSOR) == 0) {
        qCritical("Host does not advertise required PLANK local cursor transport");
        stopPlankTransportMediaReceivers();
        LiStopConnection();
        stopPlankTransportDataPlane();
        emit displayLaunchError(
                    tr("This workstation does not support the required BDE fernweh local cursor protocol."));
        return false;
    }

    updateCollaborationIndicator();
    emit diagnosticStreamParameters(QJsonObject{{"profile",int(m_PlankVideoProfile)},
        {"capture",int(m_PlankCaptureSource)},{"width",m_StreamConfig.width},
        {"height",m_StreamConfig.height},{"fps",m_StreamConfig.fps},{"bitrate_kbps",m_StreamConfig.bitrate}});
    diagnosticPhase(SessionDiagnostics::Phase::TransportReady);
    emit connectionStarted();
    return true;
}

void Session::cancelConnectionStart()
{
    m_Diagnostics.end(SessionDiagnostics::End::Cancelled);
    m_Lifecycle.stop();
    m_ConnectionStartCancelled.store(true);
    m_ActiveSessionTakeoverDecision.store(-1);
}

void Session::respondToActiveSessionTakeover(bool takeOver)
{
    int expected = 0;
    if (m_ActiveSessionTakeoverDecision.compare_exchange_strong(
                expected, takeOver ? 1 : -1)) {
        qInfo() << "PLANK active-session takeover decision:"
                << (takeOver ? "take over" : "cancel");
    }
}

void Session::respondToDesktopSignOut(bool signOut)
{
    // Shares the pending-decision state with the takeover prompt; only one
    // of them can be open during a connection start.
    int expected = 0;
    if (m_ActiveSessionTakeoverDecision.compare_exchange_strong(
                expected, signOut ? 1 : -1)) {
        qInfo() << "PLANK desktop sign-out decision:"
                << (signOut ? "sign out" : "cancel");
    }
}

void Session::respondToDesktopCollaboration(int choice)
{
    if (choice != -1 && choice != 1 && choice != 2) return;
    int expected = 0;
    m_ActiveSessionTakeoverDecision.compare_exchange_strong(expected, choice);
}

bool Session::waitForPlankDecision()
{
    return waitForPlankDecisionChoice() == 1;
}

int Session::waitForPlankDecisionChoice()
{
    constexpr int DecisionPollMs = 50;
    while (m_ActiveSessionTakeoverDecision.load() == 0 &&
           !m_ConnectionStartCancelled.load()) {
        SDL_Delay(DecisionPollMs);
    }
    m_WaitingForActiveSessionTakeoverDecision.store(false);
    const int decision = m_ActiveSessionTakeoverDecision.exchange(0);
    return m_ConnectionStartCancelled.load() ? -1 : decision;
}

bool Session::joinPlankDesktop(NvHTTP& http, const std::function<void()>& startApp, const QString& owner,
                               const QString& unlockGeneration)
{
    QJsonObject response;
    try {
        response = unlockGeneration.isEmpty() ? http.requestCollaboration() : http.requestCollaboration(owner, unlockGeneration);
    } catch (const GfeHttpResponseException& error) {
        // The owner may lock/sign out between the launch refusal and the join.
        // Preserve the refusal; never unlock somebody else's desktop or retry
        // it as a fresh credential challenge.
        if (error.getStatusCode() != 403 && error.getStatusCode() != 409) throw;
        emit displayLaunchError((unlockGeneration.isEmpty() ?
            tr("%1's desktop is locked or no longer available for sharing. Ask them to reconnect and unlock it, then join again.") :
            tr("%1's desktop could not be unlocked. The desktop or your support access may have changed. Connect again to refresh the available actions.")).arg(owner));
        return false;
    }
    const PlankCollaboration request = PlankCollaboration::parse(response);
    if (request.state != PlankCollaboration::Pending) {
        throw GfeHttpResponseException(400, "Host did not accept the collaboration request");
    }
    m_WaitingForSessionCleanup.store(true);
    emit sessionCleanupWaitChanged(true, tr("Joining %1's session…").arg(owner));
    const auto finish = [this]() {
        m_WaitingForSessionCleanup.store(false);
        emit sessionCleanupWaitChanged(false, QString());
    };
    const auto cancel = [&]() {
        try { http.cancelCollaboration(request.requestId); } catch (...) {}
    };
    try {
        const QDeadlineTimer deadline(120000);
        bool firstPoll = true;
        while (!deadline.hasExpired()) {
            // Automatically authorized joins can be ready in the request
            // response round trip. Poll immediately, then at a bounded 4 Hz.
            if (!firstPoll) {
                for (int delay = 0; delay < 250 && !m_ConnectionStartCancelled.load(); delay += 50) SDL_Delay(50);
            }
            firstPoll = false;
            if (m_ConnectionStartCancelled.load()) { cancel(); finish(); return false; }
            // Status retains this authentication; only a successful launch
            // consumes it. Never replay a broker's one-use GSSAPI admission.
            const PlankCollaboration reply = PlankCollaboration::parse(
                http.collaborationStatus(request.requestId), request.requestId);
            if (reply.state == PlankCollaboration::Pending) continue;
            if (reply.state != PlankCollaboration::Approved) {
                finish();
                emit displayLaunchError(reply.state == PlankCollaboration::Denied ?
                    tr("The desktop owner declined your collaboration request.") :
                    tr("The collaboration request expired or was cancelled. Connect again to join the session."));
                return false;
            }
            NvOutputTopology topology;
            QString error;
            if (!NvOutputTopology::fromJson(reply.topology, topology, &error))
                throw GfeHttpResponseException(400, "Host returned invalid owner display layout");
            const QString source = m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10 ?
                QStringLiteral("x11-native10") : QStringLiteral("nvfbc");
            const QString backend = StreamingPreferences::isPlankNvencProfile(m_PlankVideoProfile) ?
                QStringLiteral("nvenc-direct") : QStringLiteral("software-cuda");
            if (reply.capture.value(QStringLiteral("backend_locked")).toBool(true) &&
                    (reply.capture.value(QStringLiteral("capture_source")).toString() != source ||
                     reply.capture.value(QStringLiteral("encoder_backend")).toString() != backend)) {
                cancel(); finish();
                emit displayLaunchError(tr("The shared desktop uses capture %1 with %2. Select matching capture and encoder settings, then join the session again.")
                    .arg(reply.capture.value(QStringLiteral("capture_source")).toString(),
                         reply.capture.value(QStringLiteral("encoder_backend")).toString()));
                return false;
            }
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->outputTopology = topology;
            }
            m_CollaborationPermission.store(reply.permission);
            m_CollaborationOwner = owner;
            m_CollaborationOutputId = reply.capture.value(QStringLiteral("output_id")).toString();
            m_ResolvedArrangement.clear();
            m_ResolvedHostLayout.clear();
            m_ResolvedVirtualModes.clear();
            m_ResolvedPrimaryOutput = -1;
            m_DisplayPlan = {};
            m_UseMultiDisplayPresentation = false;
            m_IsFullScreen = false;
            m_ResolvedScalingMode = NvOutputTopology::NativeScalingMode;
            m_StreamConfig.width = reply.size.width();
            m_StreamConfig.height = reply.size.height();
            m_StreamConfig.fps = reply.fps;
            m_StreamConfig.clientRefreshRateX100 = reply.fps * 100;
            http.setCollaborationGrant(reply.grant);
            startApp();
            finish();
            return true;
        }
        cancel(); finish();
        emit displayLaunchError(tr("The session could not be joined in time. Connect again to join the session."));
        return false;
    } catch (...) {
        cancel(); finish();
        throw;
    }
}

// Single-user desktop policy: another account owns the workstation desktop.
// Either report who is using it, or (after explicit consent) ask the host to
// sign that account out and then connect from scratch to the sign-in screen,
// where the host signs this user in and the usual desktop handoff follows.
// Returns true once a launch succeeded; false after reporting the outcome.
bool Session::resolvePlankDesktopConflict(std::unique_ptr<NvHTTP>& http,
                                          const std::function<void()>& startApp,
                                          PlankDesktopSignOut conflict,
                                          const GfeHttpResponseException& refusal,
                                          bool reconnecting,
                                          QString& signOutOwner)
{
    using Step = PlankDesktopSignOut::Step;
    const auto clearSessionToken = [this]() {
        QWriteLocker lock(&m_Computer->lock);
        m_Computer->sessionToken.fill(QChar('\0'));
        m_Computer->sessionToken.clear();
        m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
    };
    const auto endWait = [this]() {
        m_WaitingForSessionCleanup.store(false);
        emit sessionCleanupWaitChanged(false, QString());
    };
    QString hostName;
    {
        QReadLocker lock(&m_Computer->lock);
        hostName = m_Computer->name;
    }
    QString requestedOwner;
    int lastStatus = refusal.getStatusCode();
    QString lastMessage = QString::fromUtf8(refusal.getStatusMessage());

    if (!reconnecting && hasPlankCredentials() &&
            (m_Computer->plankFeatureFlags & NvOutputTopology::CollaborationFeature) &&
            (conflict.collaborationAvailable || conflict.supportUnlockAvailable) &&
            (conflict.state == PlankDesktopSignOut::State::Available ||
             conflict.state == PlankDesktopSignOut::State::Connected)) {
        m_ActiveSessionTakeoverDecision.store(0);
        m_WaitingForActiveSessionTakeoverDecision.store(true);
        emit desktopCollaborationRequested(
            (conflict.supportUnlockAvailable ?
             tr("%1's desktop on %2 is locked. Your support access allows you to unlock it and join without closing their applications. Their display layout stays unchanged.") :
             tr("%1 is using %2. Join their current session to view or interact with their desktop. Their display layout stays unchanged."))
                .arg(conflict.owner, hostName), conflict.state == PlankDesktopSignOut::State::Available,
                conflict.supportUnlockAvailable);
        const int choice = waitForPlankDecisionChoice();
        if (choice == 2) {
            const bool joined = joinPlankDesktop(*http, startApp, conflict.owner,
                conflict.supportUnlockAvailable ? conflict.desktopGeneration : QString());
            if (!joined) clearSessionToken();
            return joined;
        }
        if (choice != 1 || conflict.state != PlankDesktopSignOut::State::Available) {
            clearSessionToken();
            return false;
        }
        // The destructive alternative remains explicitly confirmed below.
    }

    for (;;) {
        switch (PlankDesktopSignOut::nextStep(conflict, reconnecting,
                                              hasPlankCredentials(), requestedOwner)) {
        case Step::NotApplicable:
        case Step::Unexpected:
            clearSessionToken();
            throw GfeHttpResponseException(lastStatus, lastMessage);

        case Step::InUse:
            clearSessionToken();
            if (reconnecting) {
                m_CanReconnect.store(false);
                m_Lifecycle.stop();
            }
            emit displayLaunchError(
                        conflict.state == PlankDesktopSignOut::State::Connected ?
                            tr("%1 is connected to %2. Only one person can use a workstation at a time.")
                                .arg(conflict.owner, hostName) :
                            tr("%1 is signed in on %2. Only one person can use a workstation at a time.")
                                .arg(conflict.owner, hostName));
            qInfo() << "PLANK workstation desktop belongs to another account";
            return false;

        case Step::Confirm: {
            m_ActiveSessionTakeoverDecision.store(0);
            m_WaitingForActiveSessionTakeoverDecision.store(true);
            const bool sharingUnavailable =
                    (m_Computer->plankFeatureFlags & NvOutputTopology::CollaborationFeature) &&
                    !conflict.collaborationAvailable;
            emit desktopSignOutRequested(
                        (sharingUnavailable ?
                         tr("%1's desktop on %2 is locked or unavailable for sharing. To join, ask %1 to reconnect and unlock it. Sign %1 out and open your own desktop instead? Anything unsaved in %1's session will be lost.") :
                         tr("%1 is signed in on %2 but not connected. Sign %1 out and open your desktop? Anything unsaved in %1's session will be lost."))
                            .arg(conflict.owner, hostName));
            if (!waitForPlankDecision()) {
                qInfo() << "PLANK desktop sign-out was cancelled";
                clearSessionToken();
                return false;
            }
            // The same token stays valid for this one retry.
            requestedOwner = conflict.owner;
            signOutOwner = requestedOwner;
            try {
                startApp();
                signOutOwner.clear();
                return true;
            } catch (const GfeHttpResponseException& retryError) {
                signOutOwner.clear();
                conflict = http->desktopSignOut();
                lastStatus = retryError.getStatusCode();
                lastMessage = QString::fromUtf8(retryError.getStatusMessage());
            } catch (...) {
                signOutOwner.clear();
                throw;
            }
            continue;
        }

        case Step::AwaitGreeter: {
            constexpr int RetryIntervalMs = 2000;
            constexpr int MaximumWaitMs = 60000;
            constexpr int CancellationPollMs = 50;
            bool authenticated = false;
            bool newConflict = false;

            // The token was consumed. Broker admissions are single-use, so
            // every authentication below is a fresh one.
            clearSessionToken();
            {
                // The signed-out session's apps are gone; launch afresh.
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->currentGameId = 0;
            }
            m_WaitingForSessionCleanup.store(true);
            emit sessionCleanupWaitChanged(true, tr("Signing out %1...").arg(requestedOwner));
            qInfo() << "PLANK host is signing out the desktop owner; waiting up to"
                    << MaximumWaitMs << "ms for the sign-in screen";

            for (int elapsedMs = 0; elapsedMs < MaximumWaitMs && !newConflict;
                 elapsedMs += RetryIntervalMs) {
                for (int delayMs = 0; delayMs < RetryIntervalMs; delayMs += CancellationPollMs) {
                    if (m_ConnectionStartCancelled.load()) break;
                    SDL_Delay(CancellationPollMs);
                }
                if (m_ConnectionStartCancelled.load()) break;

                bool authenticating = false;
                bool launching = false;
                try {
                    if (!authenticated) {
                        clearSessionToken();
                        http = std::make_unique<NvHTTP>(m_Computer);
                        authenticating = true;
                        const QString token = authenticatePlank(*http, nullptr);
                        authenticating = false;
                        {
                            QWriteLocker lock(&m_Computer->lock);
                            m_Computer->sessionToken = token;
                            m_Computer->authorizationState = NvComputer::AS_AUTHORIZED;
                        }
                        authenticated = true;
                        emit sessionCleanupWaitChanged(true, tr("Opening your desktop..."));
                    }
                    const NvOutputTopology topology = http->getOutputTopology();
                    {
                        QWriteLocker lock(&m_Computer->lock);
                        m_Computer->outputTopology = topology;
                    }
                    if (m_ComputerManager != nullptr) {
                        m_ComputerManager->clientSideAttributeUpdated(m_Computer);
                    }
                    if (!configurePlankLaunchGeometry()) {
                        endWait();
                        return false;
                    }
                    launching = true;
                    startApp();
                    endWait();
                    qInfo() << "PLANK desktop sign-out completed; launch succeeded";
                    return true;
                } catch (const GfeHttpResponseException& retryError) {
                    const PlankDesktopSignOut reply =
                            launching ? http->desktopSignOut() : PlankDesktopSignOut();
                    if (reply.state != PlankDesktopSignOut::State::None) {
                        if (PlankDesktopSignOut::stillSigningOut(reply, requestedOwner)) {
                            qInfo() << "PLANK desktop owner is still being signed out";
                            continue;
                        }
                        conflict = reply;
                        lastStatus = retryError.getStatusCode();
                        lastMessage = QString::fromUtf8(retryError.getStatusMessage());
                        newConflict = true;
                        continue;
                    }
                    if (!authenticating && retryError.getStatusCode() == 401) {
                        authenticated = false;
                        qInfo() << "PLANK sign-in screen worker changed; authentication will be refreshed";
                        continue;
                    }
                    // The host confirmed sign-out before this loop. Its old
                    // worker may refuse a fresh admission while logind tears
                    // down that desktop and the greeter worker takes over.
                    // Keep the same bounded wait as a GDM desktop handover;
                    // each attempt obtains a new one-use broker token.
                    if (PlankReconnectPolicy::terminalStatus(retryError.getStatusCode(),
                                                            authenticating, true)) {
                        endWait();
                        throw;
                    }
                    qInfo() << "PLANK sign-in screen is not ready:" << retryError.toQString();
                } catch (const QtNetworkReplyException& retryError) {
                    if (retryError.getError() == QNetworkReply::SslHandshakeFailedError) {
                        endWait();
                        throw;
                    }
                    qInfo() << "PLANK sign-in screen worker is not reachable yet:"
                            << retryError.toQString();
                }
            }

            endWait();
            if (newConflict) {
                continue;
            }
            clearSessionToken();
            if (m_ConnectionStartCancelled.load()) {
                qInfo() << "PLANK connection cancelled while signing out the desktop owner";
                return false;
            }
            emit displayLaunchError(
                        tr("The workstation did not become ready within 60 seconds after signing out %1.")
                            .arg(requestedOwner));
            return false;
        }
        }
    }
}

void Session::setPlankReconnectStatus(const char* text, bool warning)
{
#ifdef PLANK_QT_SESSION
    emit qtReconnectStatusChanged(QString::fromUtf8(text), warning);
#else
    const bool nativeStatus = m_PlankToolbar &&
            m_PlankToolbar->setReconnectStatus(QString::fromUtf8(text), warning);
    // Reconnect pauses video, so an overlay alone cannot reliably repaint.
    // Use the native local surface when available without duplicating it
    // in the video overlay.
    m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
    if (!nativeStatus && text[0] != '\0') {
        m_OverlayManager.setOverlayColor(Overlay::OverlayStatusUpdate,
                    warning ? SDL_Color{0xCC, 0x00, 0x00, 0xFF} : SDL_Color{0xE0, 0xE0, 0xE0, 0xFF});
        m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate, text);
        m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
    }
    if (text[0] != '\0') {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PLANK reconnect status (%s): %s",
                    nativeStatus ? "native local surface" : "video overlay fallback", text);
    }
#endif
}

bool Session::beginPlankReconnect(
        PlankReconnectState& state)
{
    if (!hasPlankCredentials()) {
        return false;
    }

#ifndef PLANK_QT_SESSION
    // Explicit Apply and transport failure enter the same lifecycle. The SDL
    // frontend owns its legacy event-loop flags alongside that shared state.
    if (!m_Lifecycle.recoveryPending()) m_Lifecycle.requestRecovery();
#endif
    if (!m_Lifecycle.beginRecovery()) return false;
#ifndef PLANK_QT_SESSION
    m_ReconnectRequested.store(false);
    m_Reconnecting.store(true);
#endif
    m_ReconnectGreeterConfirmed.store(false);
    const bool noticeFresh = m_DesktopHandoffNoticeDeadline.exchange(0) > PlankClock::milliseconds();
    const bool greeterConfirmed = m_AuthenticatedGreeter.exchange(false);
    const bool openingDesktop = noticeFresh ||
            (greeterConfirmed &&
             (m_Computer->plankFeatureFlags & NvOutputTopology::AuthenticatedDesktopStageFeature));
    // The greeter's worker closes before the user's worker binds to the new
    // X session. A broker connect or host admission during that short gap can
    // return 401 even though this user's desktop is opening. Keep the normal
    // terminal 401 policy outside this bounded, explicit handoff window.
    m_DesktopHandoffRetryDeadline.store(openingDesktop ? PlankClock::milliseconds() + 15000 : 0);
    setPlankReconnectStatus(
                openingDesktop ? "Opening your desktop..." : "Waiting for workstation...", false);

    state = {};
    state.openingDesktop = openingDesktop;
    // The remote cursor transport stops with the host connection. Return
    // pointer ownership to the local compositor immediately so the user can
    // still see and navigate the whole client window while reconnecting. The
    // native reconnect prompt has its own Wayland surface, but cursor
    // visibility must not depend on entering that surface first.

    state.videoFormat = m_ActiveVideoFormat;
    state.videoWidth = m_ActiveVideoWidth;
    state.videoHeight = m_ActiveVideoHeight;
    state.videoFrameRate = m_ActiveVideoFrameRate;
    {
    std::lock_guard<std::mutex> decoderLock(m_DecoderLock);
    if (m_VideoDecoder != nullptr) {
        state.retainedRenderer = m_VideoDecoder->suspendForReconnect();
        if (!state.retainedRenderer) {
            delete m_VideoDecoder;
            m_VideoDecoder = nullptr;
        }
    }
    }
#ifdef PLANK_TRANSPORT
    stopPlankTransportMediaReceivers();
#endif
    LiStopConnection();
    stopPlankTransportDataPlane();
    if (m_InputHandler) m_InputHandler->resetRemoteCursorPositionEpoch();
    if (!m_RendererFactory) {

        m_ConnectionStartCancelled.store(false);
    }
    if (QThread::currentThread()->isInterruptionRequested()) {
        m_Lifecycle.stop();
        m_ConnectionStartCancelled.store(true);
        return false;
    }
    return true;
}

bool Session::waitForPlankReconnectRequest(bool restartAuthenticationAfterWait)
{
    bool waited = false;
    while (!m_Lifecycle.stopping()) {
        if (m_ReconnectPolicy.allowsRequest(PlankClock::milliseconds())) {
            // If a request was deferred, restart its password conversation so
            // an expired challenge cannot masquerade as a bad password.
            return !(waited && restartAuthenticationAfterWait);
        }
        waited = true;
        QThread::msleep(50);
    }
    return false;
}

bool Session::runPlankReconnect()
{
    if (!hasPlankCredentials()) {
        return false;
    }

    {
        QWriteLocker lock(&m_Computer->lock);
        m_Computer->sessionToken.fill(QChar('\0'));
        m_Computer->sessionToken.clear();
        m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
        m_Computer->currentGameId = 0;
    }
    for (int attempt = 1; waitForPlankReconnectRequest(); ++attempt) {
        bool authenticating = false;
        try {
            QString token;
            {
                QReadLocker lock(&m_Computer->lock);
                token = m_Computer->sessionToken;
            }
            NvHTTP http(m_Computer);
            http.setRequestGate([this](bool auth) { return waitForPlankReconnectRequest(auth); });
            if (token.isEmpty()) {
                authenticating = true;
                bool greeterConfirmed = false;
                // Brokered hosts re-admit through the broker (one-use tokens).
                token = isPlankBrokered() ? authenticatePlank(http, &greeterConfirmed) :
                        http.authenticate(m_PlankUsername, m_PlankPassword, &greeterConfirmed);
                authenticating = false;
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->sessionToken = token;
                    m_Computer->sessionIdentityKey = http.hostIdentityKey();
                    m_Computer->plankSharedDesktop = http.sharedDesktop();
                    m_Computer->authorizationState = NvComputer::AS_AUTHORIZED;
                }
                if (greeterConfirmed &&
                        ((m_Computer->plankFeatureFlags & NvOutputTopology::AuthenticatedDesktopStageFeature) ||
                         m_Computer->plankFeatureFlags == NvOutputTopology::FixedCaptureFlags)) {
                    m_ReconnectGreeterConfirmed.store(true);
                    m_AuthenticatedGreeter.store(true);
                }
            }

            NvOutputTopology topology;
            bool topologySupported;
            bool macDesktop;
            QString desktopMode;
            int desktopScale = 1;
            {
                QReadLocker lock(&m_Computer->lock);
                topologySupported = NvOutputTopology::supportsDescription(
                            m_Computer->plankTopologyVersion, m_Computer->plankFeatureFlags);
                macDesktop = m_Computer->plankFeatureFlags == NvOutputTopology::FixedCaptureFlags;
                desktopMode = m_Computer->plankVirtualMode1;
                if (macDesktop && m_Computer->plankHostLayout == NvOutputTopology::MatchClientHostLayout) {
                    QVector<NvClientDisplay> displays;
                    for (const auto& display : std::as_const(m_ClientDisplays)) {
                        displays.append({display.macMatchedBounds.isValid() ? display.macMatchedBounds : QRect(display.logicalBounds.x, display.logicalBounds.y,
                                               display.logicalBounds.w, display.logicalBounds.h), display.nativeSize, display.macBackingSize});
                    }
                    desktopMode = NvOutputTopology::resolveMacClientDisplayMode(displays, nullptr, &desktopScale);
                    if (desktopMode.isEmpty()) return false;
                }
            }
            if (topologySupported) {
                topology = macDesktop ? http.prepareMacDisplay(desktopMode,
                    StreamingPreferences::plankAppleEncodingMode(m_PlankVideoProfile), desktopScale) : http.getOutputTopology();
            }
            const QVector<NvApp> apps = http.getAppList();
            if (macDesktop && http.sharedDesktop()) {
                m_PlankVideoProfile = static_cast<StreamingPreferences::PlankVideoProfile>(
                    MacSharedSession::videoProfile(topology, m_PlankVideoProfile, true));
                const int format = m_PlankVideoProfile == StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_444 ?
                    VIDEO_FORMAT_H265_REXT10_444 : VIDEO_FORMAT_H265_MAIN10;
                m_SupportedVideoFormats.clear();
                m_SupportedVideoFormats.append(format);
                m_StreamConfig.supportedVideoFormats = format;
            }
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken = token;
                m_Computer->sessionIdentityKey = http.hostIdentityKey();
                m_Computer->plankSharedDesktop = http.sharedDesktop();
                m_Computer->authorizationState = NvComputer::AS_AUTHORIZED;
                if (topologySupported) {
                    m_Computer->outputTopology = topology;
                }
                m_Computer->updateAppList(apps);
                m_PlankLaunchFeatureFlags = m_Computer->outputTopology.launchFeatureFlags(
                            m_Computer->plankFeatureFlags);
            }

            if (m_DisplayReconfigureRequested.load() && !macDesktop && !configurePlankLaunchGeometry()) {
                // Applying a new display layout: plan it for the monitors
                // connected now. A plan that cannot work ends the attempt.
                m_Lifecycle.stop();
                return false;
            }
            if (waitForPlankReconnectRequest() && startConnectionAsync(true) &&
                    !m_Lifecycle.stopping() && m_ReconnectPolicy.allowsRequest(PlankClock::milliseconds())) {
                qInfo("PLANK reconnect transport completed on attempt %d",
                            attempt);
                return true;
            }
        } catch (const MacSessionActiveException&) {
            qInfo() << "PLANK previous Mac session is still retiring; retrying recovery";
        } catch (const GfeHttpResponseException& error) {
            qWarning() << "PLANK reconnect attempt" << attempt
                       << "failed:" << error.toQString();
            const bool openingDesktop = PlankClock::milliseconds() < m_DesktopHandoffRetryDeadline.load();
            if (PlankReconnectPolicy::terminalStatus(error.getStatusCode(), authenticating,
                                                     openingDesktop)) {
                m_Lifecycle.stop();
                if (authenticating && isPlankBrokered() && error.getStatusCode() == 401) {
                    emit displayLaunchError(tr("The stream disconnected and the workstation declined the reconnect. Connect again from the Remote list."));
                } else {
                    emit displayLaunchError(error.toQString());
                }
            } else if (openingDesktop && authenticating && error.getStatusCode() == 401) {
                qInfo() << "PLANK desktop worker is changing; retrying broker admission";
            }
            if (error.getStatusCode() == 401) {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken.fill(QChar('\0'));
                m_Computer->sessionToken.clear();
                m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
            }
        } catch (const QtNetworkReplyException& error) {
            if (error.getError() == QNetworkReply::SslHandshakeFailedError) {
                m_Lifecycle.stop();
                emit displayLaunchError(error.toQString());
            }
            qWarning() << "PLANK reconnect attempt" << attempt
                       << "was deferred or failed:" << error.toQString();
        }

#ifdef PLANK_TRANSPORT
        // A timeout or cancellation may race successful setup. Retire that
        // transport before another attempt; never leave a hidden connection.
        stopPlankTransportMediaReceivers();
#endif
        LiStopConnection();
        stopPlankTransportDataPlane();
        if (!m_Lifecycle.stopping()) {
            const int RetryDelayMs = PlankClock::milliseconds() < m_DesktopHandoffRetryDeadline.load() ?
                        2000 : 1000;
            constexpr int CancellationPollMs = 50;
            for (int elapsedMs = 0;
                 elapsedMs < RetryDelayMs && !m_Lifecycle.stopping();
                 elapsedMs += CancellationPollMs) {
                QThread::msleep(CancellationPollMs);
            }
        }
    }

    return false;
}

bool Session::finishPlankReconnect(
        bool success,
        const PlankReconnectState& state, bool* decoderNeedsReset)
{
    bool resumedRenderer = false;
    success = success && !m_Lifecycle.stopping();
    if (success) {
        const bool streamConfigurationUnchanged =
                state.videoFormat == m_ActiveVideoFormat &&
                state.videoWidth == m_ActiveVideoWidth &&
                state.videoHeight == m_ActiveVideoHeight &&
                state.videoFrameRate == m_ActiveVideoFrameRate;
        qInfo("Reconnect video configuration: requested=%dx%d active=%dx%d previous=%dx%d format=%x/%x unchanged=%d retained=%d",
              m_StreamConfig.width, m_StreamConfig.height,
              m_ActiveVideoWidth, m_ActiveVideoHeight, state.videoWidth, state.videoHeight,
              m_ActiveVideoFormat, state.videoFormat, streamConfigurationUnchanged, state.retainedRenderer);
        if (state.retainedRenderer && streamConfigurationUnchanged) {
            {
    std::lock_guard<std::mutex> decoderLock(m_DecoderLock);
            resumedRenderer = m_VideoDecoder != nullptr &&
                    m_VideoDecoder->resumeAfterReconnect();
            }
        }
        m_UnexpectedTermination = false;
    } else {
        m_UnexpectedTermination = true;
    }

#ifndef PLANK_QT_SESSION
    m_Reconnecting.store(false);
    m_ReconnectRequested.store(false);
    if (success) success = m_Lifecycle.recovered();
#endif
    m_ReconnectGreeterConfirmed.store(false);
    m_DesktopHandoffNoticeDeadline.store(0);
    m_DesktopHandoffRetryDeadline.store(0);
    setPlankReconnectStatus("", false);

    const bool displaysReconfigured = m_DisplayReconfigureRequested.exchange(false);
    if (!success) {
        m_PresentationChangePending.store(false);
        return false;
    }

    if (!m_RendererFactory && m_PresentationChangePending.load()) {
        m_DecoderLock.lock();
        delete m_VideoDecoder;
        m_VideoDecoder = nullptr;
        m_DecoderLock.unlock();
        applyPendingPresentation();
    } else if (!resumedRenderer) {
        if (decoderNeedsReset) *decoderNeedsReset = true;
        else requestDecoderReset();
    } else {
        LiRequestIdrFrame();
    }
    if (displaysReconfigured && m_InputHandler != nullptr) {
        m_InputHandler->setStreamDimensions(m_StreamConfig.width, m_StreamConfig.height);
    }
    if (state.inputCaptureWasActive && m_InputHandler) {
        m_InputHandler->setCaptureActive(true);
    }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    // Receiver teardown removes this timer on every reconnect attempt. Resume
    // periodic polling only after success, back on the SDL/AppKit main thread.
    startClipboardPollTimer();
#endif
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK reconnect completed (%s renderer)",
                resumedRenderer ? "retained" : "recreated");
    return true;
}

// At most one bounded, credential-free probe is outstanding. It owns its
// address/identity snapshots and never reads or changes Session state.
class PlankWorkerProbeThread : public QThread
{
public:
    PlankWorkerProbeThread(NvAddress address, QString instance, QString certificate,
                           bool brokered = false) :
        m_Address(address), m_Instance(instance), m_Certificate(certificate),
        m_Brokered(brokered) { }

    bool replacement() const { return m_Replacement; }
    const QString& instance() const { return m_Instance; }

    void run() override
    {
        try {
            NvHTTP http(m_Address);
            if (m_Brokered) http.setPinnedCertificateSha256(m_Certificate);
            m_Replacement = http.probeWorkerReplacement(m_Instance, m_Certificate);
        } catch (const GfeHttpResponseException&) {
            // Failure is not proof of a replacement; preserve the live stream.
        } catch (const QtNetworkReplyException&) {
        }
    }

private:
    NvAddress m_Address;
    QString m_Instance;
    QString m_Certificate;
    bool m_Brokered = false;
    bool m_Replacement = false;
};

class PlankReconnectThread : public QThread
{
public:
    explicit PlankReconnectThread(Session* session) :
        QThread(nullptr),
        m_Session(session),
        m_Success(false),
        m_CompletionPosted(false)
    {
        setObjectName("PLANK Reconnect");
    }

    bool succeeded() const
    {
        return m_Success;
    }

    bool completionPosted() const
    {
        return m_CompletionPosted.load();
    }

    void run() override
    {
        m_Success = m_Session->runPlankReconnect();

        SDL_Event event = {};
        event.type = SDL_EVENT_USER;
        event.user.code = SDL_CODE_PLANK_REPLANK_COMPLETE;
        const bool posted = SDL_PushEvent(&event);
        m_CompletionPosted.store(posted);
        if (!posted) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to post PLANK reconnect completion: %s",
                         SDL_GetError());
        }
    }

private:
    Session* m_Session;
    bool m_Success;
    std::atomic_bool m_CompletionPosted;
};

void Session::flushWindowEvents()
{
    // Pump events to ensure all pending OS events are posted
    SDL_PumpEvents();

    // SDL_CreateRenderer() callers use this barrier after renderer setup
    // because SDL may have recreated one or more native windows. Refresh every
    // Wayland Wacom subsurface explicitly; native wl_surface proxy addresses
    // alone cannot identify a replacement reliably.
    if (m_InputHandler != nullptr) {
        m_InputHandler->refreshWaylandTabletCursorParents();
    }

    // Insert a barrier to discard any additional window events.
    // We don't use SDL_FlushEvent() here because it could cause
    // important events to be lost.
    if (!PlankSessionEvents::queueWindowEventBarrier(
                m_FlushingWindowEventsRef, SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Window-event barrier could not be queued: %s", SDL_GetError());
    }
}

class ExecThread : public QThread
{
public:
    ExecThread(Session* session) :
        QThread(nullptr),
        m_Session(session)
    {
        setObjectName("Session Exec");
    }

    void run() override
    {
        m_Session->execInternal();
    }

    Session* m_Session;
};

void Session::exec(QWindow* qtWindow)
{
#ifdef Q_OS_MACOS
    auto* application = static_cast<MacApplication*>(QCoreApplication::instance());
    if (!application->beginSession()) {
        emit readyForDeletion();
        return;
    }
    const auto exitConnection = connect(application, &MacApplication::exitRequested,
                                       this, [this] {
        m_ApplicationExitRequested.store(true);
        m_Diagnostics.end(SessionDiagnostics::End::ApplicationExit);
        cancelConnectionStart();
        m_CanReconnect.store(false);
    });
    // sessionFinished precedes asynchronous transport cleanup. Only this
    // later signal releases application ownership. Queue onto Qt's thread;
    // the Session itself may already have been garbage-collected by QML.
    connect(this, &Session::readyForDeletion, application,
            [application, exitConnection] {
        QObject::disconnect(exitConnection);
        application->endSession();
    }, Qt::QueuedConnection);
#endif
#ifdef PLANK_QT_SESSION
    auto* runtime = new QtSessionRuntime(this);
    runtime->start(qtWindow);
    return;
#endif
    m_QtWindow = qtWindow;
    m_QtTargetDisplayBounds = qtWindow && qtWindow->screen() ? qtWindow->screen()->geometry() : QRect();
    m_ClientDisplayProbe = ClientDisplayProbe::probe();
    m_GuiDisplaySnapshot.publish(m_ClientDisplayProbe);

    // Use a separate thread for the streaming session on X11 or Wayland
    // to ensure we don't stomp on Qt's GL context. This breaks when using
    // the Qt EGLFS backend, so we will restrict this to X11
    m_ThreadedExec = WMUtils::isRunningX11() || WMUtils::isRunningWayland();

    if (m_ThreadedExec) {
        // Run the streaming session on a separate thread for Linux/BSD
        ExecThread execThread(this);
        SessionGuiDispatch guiDispatch(this, QCoreApplication::instance(), [this, &execThread] {
            execThread.requestInterruption();
            cancelConnectionStart();
            m_Lifecycle.stop();
            m_CanReconnect.store(false);
            m_GuiDisplaySnapshot.cancel();
            SDL_Event quit = {};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        });
        connect(this, &Session::readyForDeletion, &guiDispatch,
                [&guiDispatch] { guiDispatch.readyForDeletion(); });
        execThread.start();

        // Until the SDL streaming window is created, we should continue
        // to update the Qt UI to allow warning messages to display and
        // make sure that the Qt window can hide itself.
        while (!execThread.wait(10) && m_Window == nullptr) {
            const bool allowUserInput =
                    m_WaitingForSessionCleanup.load() ||
                    m_WaitingForActiveSessionTakeoverDecision.load();
            QCoreApplication::processEvents(
                        allowUserInput ?
                            QEventLoop::AllEvents :
                            QEventLoop::ExcludeUserInputEvents);
            QCoreApplication::sendPostedEvents();
        }
        const bool allowUserInput =
                m_WaitingForSessionCleanup.load() ||
                m_WaitingForActiveSessionTakeoverDecision.load();
        QCoreApplication::processEvents(
                    allowUserInput ?
                        QEventLoop::AllEvents :
                        QEventLoop::ExcludeUserInputEvents);
        QCoreApplication::sendPostedEvents();

        // The screen-setup dialog and Apply live on Qt's GUI thread. Keep it
        // interactive while SDL runs, but retain Session through async cleanup.
        while (!execThread.wait(10)) {
            SessionGuiDispatch::dispatch();
            if (m_GuiDisplaySnapshot.requested()) {
                if (QCoreApplication::closingDown()) m_GuiDisplaySnapshot.cancel();
                else m_GuiDisplaySnapshot.publish(ClientDisplayProbe::probe());
            }
        }
        guiDispatch.workerJoined();
        // execInternal hands transport teardown to DeferredSessionCleanupTask.
        // That task still owns a raw Session pointer after the SDL worker exits.
        // Keep Qt responsive and retain ownership until its real ready signal,
        // including when Quit or an explicit delete request canceled the stream.
        while (!guiDispatch.complete()) {
            SessionGuiDispatch::dispatch();
            if (!guiDispatch.complete()) QThread::msleep(10);
        }
        m_GuiDisplaySnapshot.cancel();
    }
    else {
        // Run the streaming session on the main thread for Windows and macOS
        execInternal();
    }
}

void Session::execInternal()
{
    // Complete initialization in this deferred context to avoid
    // calling expensive functions in the constructor (during the
    // process of loading the StreamSegue).
    //
    // NB: This initializes the SDL video subsystem, so it must be
    // called on the main thread.
    diagnosticPhase(SessionDiagnostics::Phase::Initializing);
    const bool initialized = initialize();
    if (!initialized || QThread::currentThread()->isInterruptionRequested()
#ifdef Q_OS_MACOS
            || m_ApplicationExitRequested.load()
#endif
            ) {
        m_Diagnostics.end(initialized ? SessionDiagnostics::End::ThreadInterrupted :
                          SessionDiagnostics::End::InitializationFailed);
        publishDiagnosticLifecycle(true);
        if (initialized) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
#ifdef Q_OS_MACOS
        if (!m_ApplicationExitRequested.load())
#endif
        emit sessionFinished();
        emit readyForDeletion();
        return;
    }

    // Wait for any old session to finish cleanup
    s_ActiveSessionSemaphore.acquire();

    // We're now active
    s_ActiveSession = this;

    // Initialize input before starting the connection.
    // PLANK is a remote-desktop product. Like RGS desktop mode, use
    // authoritative absolute coordinates and reserve relative capture for a
    // distinct game-mode path. This also gives receiver UI exact hit testing.
    m_InputHandler = new SdlInputHandler(*m_Preferences,
                                         m_StreamConfig.width,
                                         m_StreamConfig.height);
    m_InputCallbacks.publish(m_InputHandler);

    m_ConnectionStartCancelled.store(QThread::currentThread()->isInterruptionRequested());
    diagnosticPhase(SessionDiagnostics::Phase::Negotiating);
    AsyncConnectionStartThread asyncConnThread(this);
    if (!m_ThreadedExec) {
        // Kick off the async connection thread while we sit here and pump the event loop
        asyncConnThread.start();
        while (!asyncConnThread.wait(10)) {
            const bool allowUserInput =
                    m_WaitingForSessionCleanup.load() ||
                    m_WaitingForActiveSessionTakeoverDecision.load();
            QCoreApplication::processEvents(
                        allowUserInput ?
                            QEventLoop::AllEvents :
                            QEventLoop::ExcludeUserInputEvents);
            QCoreApplication::sendPostedEvents();
        }

        // Pump the event loop one last time to ensure we pick up any events from
        // the thread that happened while it was in the final successful QThread::wait().
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QCoreApplication::sendPostedEvents();
    }
    else {
        // We're already in a separate thread so run the connection operations
        // synchronously and don't pump the event loop. The main thread is already
        // pumping the event loop for us.
        asyncConnThread.run();
    }

    // If the connection failed, clean up and abort the connection.
    if (!m_AsyncConnectionSuccess || !m_Lifecycle.started()) {
        m_Diagnostics.end(SessionDiagnostics::End::StartupFailed);
        destroyInputHandler();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
        return;
    }

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    startClipboardPollTimer();
#endif

    int x, y, width, height;
    getWindowDimensions(x, y, width, height);

    const bool createWaylandFullscreen =
            m_IsFullScreen &&
            strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0;
    bool presentationMappingDeferred =
            createWaylandFullscreen && !m_UseMultiDisplayPresentation;
    if (createWaylandFullscreen) {
        SDL_Rect displayBounds;
        const SDL_DisplayID display = StreamUtils::getDisplayId(
                    getTargetDisplayIndex());
        if (SDL_GetDisplayBounds(display, &displayBounds)) {
            // SDL 3.4.2 can retain the initially requested Wayland viewport
            // as its absolute-pointer coordinate range after a later
            // fullscreen configure. Request the compositor's complete logical
            // output size from the first surface configure so there is no
            // coordinate-space transition to retain.
            width = displayBounds.w;
            height = displayBounds.h;
            x = y = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Creating Wayland fullscreen surface at %dx%d",
                        width, height);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to query Wayland fullscreen bounds: %s",
                        SDL_GetError());
        }
    }

#ifdef STEAM_LINK
    // We need a little delay before creating the window or we will trigger some kind
    // of graphics driver bug on Steam Link that causes a jagged overlay to appear in
    // the top right corner randomly.
    SDL_Delay(500);
#endif

    // Request at least 8 bits per color for GL
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

    // We always want a resizable window with High DPI enabled
    Uint32 defaultWindowFlags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (createWaylandFullscreen && !presentationMappingDeferred) {
        // Enter compositor-native fullscreen on the initial configure. This
        // prevents SDL from binding pointer input to an intermediate windowed
        // viewport before the fullscreen surface exists.
        defaultWindowFlags |= m_FullScreenFlag;
    }
    if (presentationMappingDeferred) {
        // Decoder selection can switch the SDL window between OpenGL and
        // Vulkan. SDL implements that switch by recreating the native Wayland
        // toplevel. Keep those intermediate surfaces unmapped so GNOME sees
        // one stable PLANK window instead of a sequence of short-lived
        // fullscreen windows.
        defaultWindowFlags |= SDL_WINDOW_HIDDEN;
    }

    // We use only the computer name on macOS to match Apple conventions where the
    // app name is featured in the menu bar and the document name is in the title bar.
#ifdef Q_OS_DARWIN
    std::string windowName = QString(m_Computer->name).toStdString();
#else
    std::string windowName = QString(m_Computer->name + " - BDE fernweh").toStdString();
#endif

    diagnosticPhase(SessionDiagnostics::Phase::CreatingWindow);
    m_Window = SDL_CreateWindow(windowName.c_str(),
                                width,
                                height,
                                defaultWindowFlags | StreamUtils::getPlatformWindowFlags());
    if (!m_Window) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateWindow() failed with platform flags: %s",
                    SDL_GetError());

        m_Window = SDL_CreateWindow(windowName.c_str(),
                                    width,
                                    height,
                                    defaultWindowFlags);
        if (!m_Window) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_CreateWindow() failed: %s",
                         SDL_GetError());

            m_Diagnostics.end(SessionDiagnostics::End::WindowCreationFailed);
            destroyInputHandler();
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
            return;
        }
    }

    m_Diagnostics.windowCreated();
    publishDiagnosticLifecycle();
    SDL_SetWindowPosition(m_Window, x, y);

    if ((isCollaborator() && !createCollaborationWindows()) ||
            (!isCollaborator() && m_UseMultiDisplayPresentation &&
             !createSecondaryWindows(defaultWindowFlags, windowName))) {
        m_Diagnostics.end(SessionDiagnostics::End::WindowCreationFailed);
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
        destroyInputHandler();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        QThreadPool::globalInstance()->start(
                    new DeferredSessionCleanupTask(this));
        return;
    }

    if (!m_IsFullScreen) {
        // Windowed means a normal compositor-managed desktop window. Do not
        // inherit a maximized launcher state that can make it indistinguishable
        // from borderless mode on Wayland.
        SDL_SetWindowFullscreen(m_Window, 0);
        SDL_RestoreWindow(m_Window);
        SDL_SetWindowBordered(m_Window, true);
        SDL_SetWindowResizable(m_Window, true);
        m_HasWindowedPresentationGeometry = true;
    }

    // HACK: Remove once proper Dark Mode support lands in SDL
#ifdef Q_OS_WIN32
    if (m_QtWindow != nullptr) {
        BOOL darkModeEnabled;

        // Query whether dark mode is enabled for our Qt window (which tracks the OS dark mode state)
        if (FAILED(DwmGetWindowAttribute((HWND)m_QtWindow->winId(), DWMWA_USE_IMMERSIVE_DARK_MODE, &darkModeEnabled, sizeof(darkModeEnabled))) &&
            FAILED(DwmGetWindowAttribute((HWND)m_QtWindow->winId(), DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &darkModeEnabled, sizeof(darkModeEnabled)))) {
            darkModeEnabled = FALSE;
        }

        HWND sdlWindow = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(m_Window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));

        if (sdlWindow != nullptr) {
            // If dark mode is enabled, propagate that to our SDL window
            if (darkModeEnabled) {
                if (FAILED(DwmSetWindowAttribute(sdlWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkModeEnabled, sizeof(darkModeEnabled)))) {
                    DwmSetWindowAttribute(sdlWindow, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &darkModeEnabled, sizeof(darkModeEnabled));
                }

                // Toggle non-client rendering off and back on to ensure dark mode takes effect on Windows 10.
                // DWM doesn't seem to correctly invalidate the non-client area after enabling dark mode.
                DWMNCRENDERINGPOLICY ncPolicy = DWMNCRP_DISABLED;
                DwmSetWindowAttribute(sdlWindow, DWMWA_NCRENDERING_POLICY, &ncPolicy, sizeof(ncPolicy));
                ncPolicy = DWMNCRP_ENABLED;
                DwmSetWindowAttribute(sdlWindow, DWMWA_NCRENDERING_POLICY, &ncPolicy, sizeof(ncPolicy));
            }
        }
    }
#endif

    m_InputHandler->setViewOnly(m_CollaborationPermission.load() == PlankCollaboration::View);
    m_InputHandler->setStreamDimensions(m_StreamConfig.width, m_StreamConfig.height);
    m_InputHandler->setWindow(m_Window);
    m_InputHandler->setPointerCaptureSupported(!isCollaborator() &&
                (m_PlankLaunchFeatureFlags & NvOutputTopology::PointerCaptureFeature));
    rebuildPresentationLayout();
    // The monitor set this stream presents, to tell a rearrangement from new screens.
    {
        const QVector<NvClientDisplay> presented = m_ClientDisplayProbe;
        m_PresentedDisplayFingerprint = ClientDisplayProbe::fingerprint(presented);
        m_ClientDisplayChange.applied(presented);
    }

    QImage iconImage(":/res/plank-logo.png");
    iconImage = iconImage.scaled(ICON_SIZE,
                                 ICON_SIZE,
                                 Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_RGBA8888);
    SDL_Surface* iconSurface = SDL_CreateSurfaceFrom(iconImage.width(),
                                                     iconImage.height(),
                                                     SDL_PIXELFORMAT_RGBA32,
                                                     (void*)iconImage.constBits(),
                                                     4 * iconImage.width());
#ifndef Q_OS_DARWIN
    // Other platforms seem to preserve our Qt icon when creating a new window.
    if (iconSurface != nullptr) {
        // This must be called before entering full-screen mode on Windows
        // or our icon will not persist when toggling to windowed mode
        SDL_SetWindowIcon(m_Window, iconSurface);
        for (SDL_Window* window : std::as_const(m_SecondaryWindows)) {
            SDL_SetWindowIcon(window, iconSurface);
        }
    }
#endif

    // Update the window display mode based on our current monitor
    // for if/when we enter full-screen mode.
    updateOptimalWindowDisplayMode();

    // Enter full screen if requested
    if (m_IsFullScreen) {
        if (presentationMappingDeferred) {
            // Keep the initial window normally sized and hidden through all
            // graphics-backend probes. Fullscreen is queued immediately before
            // the first map, after the final native surface exists. This avoids
            // SDL 3.4.2 sending zero-sized xdg_surface geometry while recreating
            // a hidden fullscreen Wayland window.
            m_PresentationFullscreen = true;
            rebuildPresentationLayout();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Deferring initial Wayland presentation map until decoder selection completes");
        } else {
            setPresentationWindowsFullscreen(true);
        }
    } else if (m_UseMultiDisplayPresentation && strcmp(SDL_GetCurrentVideoDriver(), "cocoa") == 0) {
        setPresentationWindowsFullscreen(false);
    }

    bool needsFirstEnterCapture = false;
    bool needsPostDecoderCreationCapture = false;

    // HACK: For Wayland, we wait until we get the first SDL_EVENT_WINDOW_MOUSE_ENTER
    // event where it seems to work consistently on GNOME. For other platforms,
    // especially where SDL may call SDL_RecreateWindow(), we must only capture
    // after the decoder is created.
    if (strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        // Native Wayland: Capture on SDL_EVENT_WINDOW_MOUSE_ENTER
        needsFirstEnterCapture = true;
    }
    else {
        // X11/XWayland: Capture after decoder creation
        needsPostDecoderCreationCapture = true;
    }

    // Stop text input. SDL enables it by default
    // when we initialize the video subsystem, but this
    // causes an IME popup when certain keys are held down
    // on macOS.
    SDL_StopTextInput(m_Window);
    for (SDL_Window* window : std::as_const(m_SecondaryWindows)) {
        SDL_StopTextInput(window);
    }

    // Disable the screen saver if requested
    if (m_Preferences->keepAwake) {
        SDL_DisableScreenSaver();
    }

    // Hide Qt's fake mouse cursor on EGLFS systems
    if (QGuiApplication::platformName() == "eglfs") {
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
    }

    // Set timer resolution to 1 ms on Windows for greater
    // sleep precision and more accurate callback timing.
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");

    SDL_DisplayID currentDisplayId = SDL_GetDisplayForWindow(m_Window);

    // Now that we're about to stream, any SDL_EVENT_QUIT event is expected
    // unless it comes from the connection termination callback where
    // (m_UnexpectedTermination is set back to true).
    m_UnexpectedTermination = false;

    // Toggle the stats overlay if requested by the user
    m_OverlayManager.setOverlayState(Overlay::OverlayDebug, m_Preferences->showPerformanceOverlay);

    const auto initializePlankToolbar = [this]() {
        if (m_Computer->plankAuthentication && !m_PlankToolbar) {
            m_PlankToolbar.reset(new PlankToolbar(
                        m_Window, m_OverlayManager, *m_InputHandler,
                        *m_Preferences, m_PlankBitrateKbps));
        }
    };
    if (!presentationMappingDeferred) {
        initializePlankToolbar();
    }

    // Hijack this thread to be the SDL main thread. We have to do this
    // because we want to suspend all Qt processing until the stream is over.
    PlankReconnectThread* reconnectThread = nullptr;
    PlankWorkerProbeThread* workerProbe = nullptr;
    Uint64 nextWorkerProbe = 0;
    PlankReconnectState reconnectState;
    Uint64 reconnectDecisionDeadline = 0;
    const auto handlePlankLocalUserEvent = [this](const SDL_UserEvent& userEvent) {
        if (PlankSessionEvents::dispatchWindowEventBarrier(userEvent,
                m_FlushingWindowEventsRef, SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER)) {
            return true;
        }
        switch (userEvent.code) {
        case SDL_CODE_PLANK_POINTER_CAPTURE:
            if (m_InputHandler) m_InputHandler->pollImmersiveMouse();
            return true;
        case SDL_CODE_PLANK_BITRATE_APPLIED:
            if (m_PlankToolbar) {
                m_PlankToolbar->setAppliedBitrate(
                            m_ConfirmedBitrateRequestKbps.load(std::memory_order_relaxed),
                            m_ConfirmedBitrateAppliedKbps.load(std::memory_order_relaxed),
                            m_ConfirmedBitratePeakKbps.load(std::memory_order_relaxed));
            }
            return true;
        case SDL_CODE_PLANK_CURSOR:
            if (m_InputHandler != nullptr) {
                m_InputHandler->applyPendingRemoteCursor();
            }
            return true;
        case SDL_CODE_PLANK_TABLET_CURSOR:
            if (m_InputHandler != nullptr) {
                m_InputHandler->applyPendingTabletCursorActivation();
            }
            return true;
        case SDL_CODE_PLANK_CURSOR_POSITION:
            if (m_InputHandler != nullptr) {
                m_InputHandler->applyPendingRemoteCursorPosition();
            }
            return true;
        case SDL_CODE_PLANK_DECODED_FRAME_SIZE:
            if (m_InputHandler != nullptr) {
                const std::uint64_t size =
                        m_DecodedFrameSize.load(std::memory_order_relaxed);
                m_InputHandler->updateDecodedStreamDimensions(
                            static_cast<int>(size >> 32),
                            static_cast<int>(size & 0xffffffffu));
            }
            return true;
        case SDL_CODE_PLANK_CLIPBOARD:
        case SDL_CODE_PLANK_CLIPBOARD_POLL:
        case SDL_CODE_PLANK_FILE_CLIPBOARD_READY:
        case SDL_CODE_PLANK_FILE_CLIPBOARD_PUBLISH:
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
            dispatchClipboardEvent(userEvent.code, reinterpret_cast<std::uintptr_t>(userEvent.data1));
#endif
            return true;
        default:
            return false;
        }
    };
    SDL_Event event;
    for (;;) {
        // Qt's cancel/quit must survive a full SDL queue. The queued quit
        // event is just a wakeup; interruption is the durable exit request.
        if (QThread::currentThread()->isInterruptionRequested()) {
            m_Diagnostics.end(SessionDiagnostics::End::ThreadInterrupted);
            goto DispatchDeferredCleanup;
        }
        m_InputHandler->pollImmersiveMouse();
        if (m_PlankToolbar) m_PlankToolbar->setImmersiveMouseState(
                    m_InputHandler->immersiveMouseAvailable(), m_InputHandler->immersiveMouse());
        const auto localHotkey = m_InputHandler->takeSessionHotkey();
        if (localHotkey == PlankSessionHotkeys::Disconnect) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Reserved local disconnect hotkey");
            m_Diagnostics.end(SessionDiagnostics::End::Hotkey);
            goto DispatchDeferredCleanup;
        }
        if (localHotkey & PlankSessionHotkeys::ToggleToolbar) togglePlankToolbar();
        if (localHotkey & PlankSessionHotkeys::ToggleImmersiveMouse) m_InputHandler->toggleImmersiveMouse();
#ifdef Q_OS_MACOS
        if (m_ApplicationExitRequested.load()) {
            m_Diagnostics.end(SessionDiagnostics::End::ApplicationExit);
            goto DispatchDeferredCleanup;
        }
#endif
        const Uint64 now = SDL_GetTicks(); // SDL-only toolbar/display-settle timers.
        const auto networkNow = PlankClock::milliseconds();
        const bool videoSilent = PlankHostRecovery::videoSilent(networkNow, m_LastPlankVideoReceived.load());
        if (workerProbe != nullptr && workerProbe->isFinished()) {
            workerProbe->wait();
            if (!m_Reconnecting.load() && !m_ReconnectRequested.load() && m_CanReconnect.load() &&
                    videoSilent && workerProbe->instance() == m_PlankWorkerInstance && workerProbe->replacement()) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Confirmed replacement Host worker during video silence; reconnecting before transport timeout");
                clConnectionTerminated(-2);
            }
            delete workerProbe;
            workerProbe = nullptr;
            nextWorkerProbe = networkNow + PlankHostRecovery::ProbeIntervalMs;
        }
        if (!m_Reconnecting.load() && !m_ReconnectRequested.load()) {
            if (videoSilent && m_CanReconnect.load() && !m_PlankWorkerInstance.isEmpty()) {
                // Video silence is normal for an unchanged desktop. Probe for
                // a replacement worker, but show recovery status only after
                // that replacement or a transport failure is confirmed.
                if (workerProbe == nullptr && networkNow >= nextWorkerProbe) {
                    workerProbe = new PlankWorkerProbeThread(m_Computer->activeAddress,
                                    m_PlankWorkerInstance, m_PlankHostCertificateSha256,
                                    !m_Computer->brokerHostCertSha256.isEmpty());
                    workerProbe->start();
                }
            }
        }
        if (m_PlankToolbar) {
            m_PlankToolbar->setRenderedStats(
                        m_CurrentRenderedFps.load(std::memory_order_relaxed),
                        m_CurrentVideoMbps.load(std::memory_order_relaxed),
                        currentVideoFecLoss().before,
                        currentNetworkRttMs());
            {
                std::lock_guard<std::mutex> guard(m_MicrophoneMutex);
                m_PlankToolbar->setMicrophoneState(m_Microphone != nullptr,
                    m_Microphone ? m_Microphone->state() : PlankMicrophone::State::Off);
            }
            {
                std::lock_guard<std::mutex> guard(m_CameraMutex);
                m_PlankToolbar->setCameraState(m_Camera != nullptr,
                    m_Camera ? m_Camera->state() : PlankCamera::State::Off);
            }
#if defined(Q_OS_MACOS) && defined(PLANK_TRANSPORT)
            m_PlankToolbar->setFileTransferState(fileClipboard() &&
                    (m_FileClipboardMode == QStringLiteral("bidirectional") ||
                     m_FileClipboardMode == QStringLiteral("client-to-host")),
                    fileClipboard() ? QString::fromStdString(fileClipboard()->status()) : QString());
#endif
            const auto action = m_PlankToolbar->update(
                        SDL_GetTicks(), !m_Reconnecting.load());
            if (action == PlankToolbar::Action::ToggleCamera)
                m_CameraRequested.store(!m_CameraRequested.load());
            if (action == PlankToolbar::Action::ToggleMicrophone)
                m_MicrophoneRequested.store(!m_MicrophoneRequested.load());
            if (action == PlankToolbar::Action::NewConnection) openNewConnection();
            if (action == PlankToolbar::Action::Disconnect) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK toolbar disconnect requested");
                m_Diagnostics.end(SessionDiagnostics::End::Toolbar);
                goto DispatchDeferredCleanup;
            }
            if (action == PlankToolbar::Action::KeepWaiting) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK unreachable-host prompt requested continued retries");
                m_PlankToolbar->hideReconnectPrompt();
                reconnectDecisionDeadline = PlankClock::milliseconds() +
                        static_cast<Uint64>(m_Preferences->plankUnreachableTimeoutSeconds) * 1000;
                setPlankReconnectStatus("Waiting for workstation...", false);
            }
            if (action == PlankToolbar::Action::ToggleFullscreen) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK toolbar fullscreen toggle requested");
                toggleFullscreen();
                m_PlankToolbar->notifyWindowChanged();
            } else if (action == PlankToolbar::Action::Minimize) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK toolbar minimize requested");
                minimizePresentationWindows();
            } else if (action == PlankToolbar::Action::ApplyScreens) {
                requestDisplayLayoutApply();
            } else if (action == PlankToolbar::Action::KeepScreens) {
                keepDisplayLayout();
            } else if (action == PlankToolbar::Action::SetUpScreens) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PLANK screens changed: display setup requested");
                emit displaySetupRequested();
            }
        }

        // Client monitors settled after a change (dock, undock, lid).
        if (m_ClientDisplayChangeDeadline != 0 && now >= m_ClientDisplayChangeDeadline &&
                !m_Reconnecting.load() && !m_ReconnectRequested.load()) {
            m_ClientDisplayChangeDeadline = 0;
            handleClientDisplaysChanged();
        }

        // The old desktop worker may lose Xorg before it can send a logout
        // notice. Use the replacement worker's authenticated stage instead.
        // Apply only on the SDL thread and never override an expired timeout.
        if (m_ReconnectGreeterConfirmed.exchange(false) && m_Reconnecting.load() &&
                reconnectDecisionDeadline != 0 && PlankClock::milliseconds() < reconnectDecisionDeadline) {
            setPlankReconnectStatus("Returning to the sign-in screen...", false);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Authenticated Host confirmed return to the sign-in screen");
        }

        if (m_Reconnecting.load() && reconnectDecisionDeadline != 0 &&
                (reconnectThread == nullptr || !reconnectThread->isFinished()) &&
                PlankClock::milliseconds() >= reconnectDecisionDeadline) {
            setPlankReconnectStatus("Workstation is taking longer to respond...", true);
            if (m_Preferences->plankUnreachableAction ==
                    StreamingPreferences::PLANK_UNREACHABLE_DISCONNECT) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK reconnect deadline expired; disconnecting automatically");
                m_Diagnostics.end(SessionDiagnostics::End::ReconnectTimeout);
                goto DispatchDeferredCleanup;
            }

            m_PlankToolbar->showReconnectPrompt(
                        m_Preferences->plankUnreachableTimeoutSeconds);
            reconnectDecisionDeadline = 0;
        }
        int eventWaitTimeout = m_Reconnecting.load() ? 50 :
                    (m_PlankToolbar ?
                         m_PlankToolbar->eventWaitTimeout() : 1000);
        if (m_InputHandler->immersiveMouseOwnsPointer()) eventWaitTimeout = std::min(eventWaitTimeout, 50);
        if (m_ThreadedExec) eventWaitTimeout = std::min(eventWaitTimeout, 50);
#ifdef Q_OS_MACOS
        // Native Quit and reserved hotkeys change explicit session state,
        // independent of SDL queue capacity. Bound the wait for a silent Host.
        eventWaitTimeout = std::min(eventWaitTimeout, 50);
        // Reconcile AppKit focus even when SDL omits a Spaces focus event.
        m_InputHandler->refreshTabletFocus();
#endif
        if (!PlankSessionEvents::waitForEvent(event, eventWaitTimeout, m_Window,
                m_VideoDecoder == nullptr && !m_Reconnecting.load())) {
            if (reconnectThread != nullptr &&
                    reconnectThread->isFinished() &&
                    !reconnectThread->completionPosted()) {
                event = {};
                event.type = SDL_EVENT_USER;
                event.user.code = SDL_CODE_PLANK_REPLANK_COMPLETE;
            } else {
                continue;
            }
        }

#ifdef Q_OS_MACOS
        if (m_ApplicationExitRequested.load()) {
            m_Diagnostics.end(SessionDiagnostics::End::ApplicationExit);
            goto DispatchDeferredCleanup;
        }
#endif
        if (QThread::currentThread()->isInterruptionRequested()) {
            m_Diagnostics.end(SessionDiagnostics::End::ThreadInterrupted);
            goto DispatchDeferredCleanup;
        }
        const auto nativeHotkey = m_InputHandler->takeSessionHotkey();
        if (nativeHotkey == PlankSessionHotkeys::Disconnect) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Reserved native disconnect hotkey");
            m_Diagnostics.end(SessionDiagnostics::End::Hotkey);
            goto DispatchDeferredCleanup;
        }
        if (nativeHotkey & PlankSessionHotkeys::ToggleToolbar) togglePlankToolbar();
        if (nativeHotkey & PlankSessionHotkeys::ToggleImmersiveMouse) m_InputHandler->toggleImmersiveMouse();
        // Handle Close before reconnect's presentation-only event drain. SDL
        // does not synthesize Quit while another stream window is visible.
        if (PlankSessionEvents::closeRequestsDisconnect(event,
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                windowForEvent(event.window.windowID) != nullptr)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Stream window close requested; disconnecting session");
            m_Diagnostics.end(SessionDiagnostics::End::WindowClose);
            goto DispatchDeferredCleanup;
        }
        const bool reconnectCompletion =
                event.type == SDL_EVENT_USER &&
                event.user.code == SDL_CODE_PLANK_REPLANK_COMPLETE;
#ifdef Q_OS_MACOS
        if (m_InputHandler->handleCapturedMacKeyEvent(event)) {
            continue;
        }
#endif
        if (m_Reconnecting.load() &&
                event.type != SDL_EVENT_QUIT && !reconnectCompletion) {
            // Cursor shapes, host-authoritative Wacom positions, and toolbar
            // bitrate confirmation are local presentation events. The new
            // transport can deliver them before its reconnect worker posts
            // completion. Apply them now so their one-shot pending latches do
            // not remain set after this event is consumed.
            if (event.type == SDL_EVENT_USER &&
                    handlePlankLocalUserEvent(event.user)) {
                continue;
            }

            // Keep the local window, hotkeys, and toolbar alive while the
            // transport worker retries. Never forward these events to a host
            // whose input connection has already stopped.
            switch (event.type) {
            case SDL_EVENT_MOUSE_MOTION:
                if (m_PlankToolbar &&
                        event.motion.windowID == SDL_GetWindowID(m_Window)) {
                    m_PlankToolbar->observeMouseMotion(event.motion);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (m_PlankToolbar &&
                        event.button.windowID == SDL_GetWindowID(m_Window)) {
                    const auto action =
                            m_PlankToolbar->handleMouseButton(event.button);
                    if (action == PlankToolbar::Action::ToggleCamera)
                        m_CameraRequested.store(!m_CameraRequested.load());
                    if (action == PlankToolbar::Action::ToggleMicrophone)
                        m_MicrophoneRequested.store(!m_MicrophoneRequested.load());
                    if (action == PlankToolbar::Action::NewConnection) openNewConnection();
                    if (action == PlankToolbar::Action::Disconnect) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "PLANK toolbar disconnect requested during reconnect");
                        m_Diagnostics.end(SessionDiagnostics::End::Toolbar);
                        goto DispatchDeferredCleanup;
                    }
                    if (action == PlankToolbar::Action::ToggleFullscreen) {
                        toggleFullscreen();
                        m_PlankToolbar->notifyWindowChanged();
                    } else if (action == PlankToolbar::Action::Minimize) {
                        minimizePresentationWindows();
                    }
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (m_PlankToolbar &&
                        event.wheel.windowID == SDL_GetWindowID(m_Window)) {
                    m_PlankToolbar->handleMouseWheel(event.wheel);
                }
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                // Retain PLANK's local Ctrl+Alt+Shift hotkeys. Any
                // ordinary key events are harmless because LiStopConnection()
                // has already closed the remote input channel.
                m_InputHandler->handleKeyEvent(&event.key);
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                if (m_PlankToolbar) {
                    m_PlankToolbar->notifyWindowChanged();
                }
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    m_InputHandler->notifyWindowGeometryChanged(
                                windowForEvent(event.window.windowID),
                                "pixel-size");
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (!anyPresentationWindowFocused()) {
                    if (m_PlankToolbar) {
                        m_PlankToolbar->notifyFocusLost();
                    }
                    m_InputHandler->notifyFocusLost();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
                m_TextClipboardPastePending = false;
#if (defined(Q_OS_WIN) || defined(Q_OS_MACOS)) && defined(PLANK_TRANSPORT)
                if (const auto files = fileClipboard()) files->cancelPendingPasteOnMainThread();
#endif
#endif
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                m_InputHandler->notifyFocusGained();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
                queueClipboardPollEvent();
#endif
                break;
            default:
                break;
            }
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_QUIT:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Quit event received");
            m_Diagnostics.end(SessionDiagnostics::End::QuitEvent);
            goto DispatchDeferredCleanup;

        case SDL_EVENT_USER:
            if (handlePlankLocalUserEvent(event.user)) {
                break;
            }
            switch (event.user.code) {
            case SDL_CODE_PLANK_COLLABORATION_PERMISSION:
                applyCollaborationPermission();
                break;
            case SDL_CODE_PLANK_RECONNECT:
                if (reconnectThread != nullptr ||
                        !beginPlankReconnect(reconnectState)) {
                    m_Diagnostics.end(SessionDiagnostics::End::ReconnectFailed);
                    emit displayLaunchError(
                                tr("The workstation desktop changed, but the client could not start reconnecting."));
                    goto DispatchDeferredCleanup;
                }
                reconnectDecisionDeadline = m_ReconnectPolicy.startSdlRecovery(
                        PlankClock::milliseconds(),
                        static_cast<Uint64>(m_Preferences->plankUnreachableTimeoutSeconds) * 1000,
                        m_Preferences->plankUnreachableAction ==
                            StreamingPreferences::PLANK_UNREACHABLE_DISCONNECT,
                        reconnectState.openingDesktop);
                reconnectThread = new PlankReconnectThread(this);
                reconnectThread->start();
                break;
            case SDL_CODE_PLANK_REPLANK_COMPLETE:
            {
                if (reconnectThread == nullptr) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Ignoring reconnect completion without an active worker");
                    break;
                }
                reconnectThread->wait();
                const bool reconnectSucceeded = reconnectThread->succeeded();
                delete reconnectThread;
                reconnectThread = nullptr;
                reconnectDecisionDeadline = 0;
                if (m_PlankToolbar) {
                    m_PlankToolbar->hideReconnectPrompt();
                }
                if (!finishPlankReconnect(
                            reconnectSucceeded, reconnectState)) {
                    m_Diagnostics.end(SessionDiagnostics::End::ReconnectFailed);
                    if (!m_Lifecycle.stopping()) {
                        emit displayLaunchError(reconnectState.openingDesktop ?
                                    tr("The workstation desktop changed, but the client could not reconnect.") :
                                    tr("The stream disconnected and the client could not reconnect."));
                    }
                    goto DispatchDeferredCleanup;
                }
                break;
            }
            case SDL_CODE_FRAME_READY:
                if (m_VideoDecoder != nullptr) {
                    m_VideoDecoder->renderFrameOnMainThread();
                }
                break;
            case SDL_CODE_PLANK_APPLY_SCREENS:
                requestDisplayLayoutApply();
                break;
            default:
                SDL_assert(false);
            }
            break;

        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            if (m_PlankToolbar) {
                m_PlankToolbar->notifyWindowChanged();
            }
            break;

        case SDL_EVENT_DISPLAY_ADDED:
        case SDL_EVENT_DISPLAY_REMOVED:
        case SDL_EVENT_DISPLAY_MOVED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            // Docking brings displays up one by one: act once they settle.
            m_ClientDisplayChangeDeadline = SDL_GetTicks() + 1500;
            break;

#ifdef Q_OS_DARWIN
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            if (SDL_Window* window = windowForEvent(event.window.windowID)) {
                // AppKit's green button bypasses toggleFullscreen(). Keep the
                // session's presentation and the other display windows in
                // step with a native transition of the primary window too.
                const bool fullscreen = event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
                const bool actualFullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                if (window == m_Window && fullscreen == actualFullscreen &&
                        fullscreen != m_PresentationFullscreen) {
                    m_DecoderLock.lock();
                    delete m_VideoDecoder;
                    m_VideoDecoder = nullptr;
                    m_DecoderLock.unlock();
                    setPresentationWindowsFullscreen(fullscreen);
                    SDL_Event resetEvent = {};
                    resetEvent.type = SDL_EVENT_RENDER_DEVICE_RESET;
                    SDL_PushEvent(&resetEvent);
                }
                MacWindow::logGeometry(window);
                m_InputHandler->notifyWindowGeometryChanged(
                            window,
                            event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ?
                                "enter-fullscreen" : "leave-fullscreen");
                m_InputHandler->updateKeyboardGrabState();
                if (m_PlankToolbar) {
                    m_PlankToolbar->notifyWindowChanged();
                }
            }
            break;
#endif

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
        {
            SDL_Window* eventWindow = windowForEvent(event.window.windowID);
            if (eventWindow == nullptr) {
                break;
            }
#ifdef Q_OS_DARWIN
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                MacWindow::logGeometry(eventWindow);
            }
#endif
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                m_InputHandler->notifyWindowGeometryChanged(eventWindow,
                                                            "pixel-size");
            }
            if (m_PlankToolbar && eventWindow == m_Window &&
                    event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                m_PlankToolbar->notifyWindowChanged();
            }
            // Early handling of some events
            switch (event.type) {
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (!anyPresentationWindowFocused()) {
                    if (m_PlankToolbar) {
                        m_PlankToolbar->notifyFocusLost();
                    }
                    if (m_Preferences->muteOnFocusLoss) {
                        m_AudioMuted = true;
                    }
                    m_InputHandler->notifyFocusLost();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
                m_TextClipboardPastePending = false;
#if (defined(Q_OS_WIN) || defined(Q_OS_MACOS)) && defined(PLANK_TRANSPORT)
                if (const auto files = fileClipboard()) files->cancelPendingPasteOnMainThread();
#endif
#endif
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if (m_Preferences->muteOnFocusLoss) {
                    m_AudioMuted = false;
                }
                m_InputHandler->notifyFocusGained();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
                queueClipboardPollEvent();
#endif
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                m_InputHandler->notifyMouseLeave();
                break;
            }


            // Capture the mouse on SDL_EVENT_WINDOW_MOUSE_ENTER if needed
            if (needsFirstEnterCapture && event.type == SDL_EVENT_WINDOW_MOUSE_ENTER) {
                m_InputHandler->setCaptureActive(true);
                needsFirstEnterCapture = false;
            }

            // Secondary Vulkan swapchains resize themselves during the next
            // frame. Only the primary window participates in decoder and
            // toolbar lifecycle decisions.
            if (eventWindow != m_Window) {
                break;
            }

            // We want to recreate the decoder for resizes (full-screen toggles) and the initial shown event.
            // We use SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED rather than SDL_EVENT_WINDOW_RESIZED because the latter doesn't
            // seem to fire when switching from windowed to full-screen on X11.
            if (event.type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
                (event.type != SDL_EVENT_WINDOW_SHOWN || m_VideoDecoder != nullptr)) {
                // Check that the window display hasn't changed. If it has, we want
                // to recreate the decoder to allow it to adapt to the new display.
                // This will allow Pacer to pull the new display refresh rate.
                if (event.type != SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
                    break;
                }
            }
#ifdef Q_OS_WIN32
            // We can get a resize event after being minimized. Recreating the renderer at that time can cause
            // us to start drawing on the screen even while our window is minimized. Minimizing on Windows also
            // moves the window to -32000, -32000 which can cause a false window display index change. Avoid
            // that whole mess by never recreating the decoder if we're minimized.
            else if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
                break;
            }
#endif

            if (m_FlushingWindowEventsRef > 0) {
                // Ignore window events for renderer reset if flushing
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Dropping window event during flush: %d (%d %d)",
                            event.type,
                            event.window.data1,
                            event.window.data2);
                break;
            }

            // Allow the renderer to handle the state change without being recreated
            if (m_VideoDecoder) {
                bool forceRecreation = false;

                WINDOW_STATE_CHANGE_INFO windowChangeInfo = {};
                windowChangeInfo.window = m_Window;

                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    windowChangeInfo.stateChangeFlags |= WINDOW_STATE_CHANGE_SIZE;

                    windowChangeInfo.width = event.window.data1;
                    windowChangeInfo.height = event.window.data2;
                }

                const SDL_DisplayID newDisplayId = SDL_GetDisplayForWindow(m_Window);
                if (newDisplayId != currentDisplayId) {
                    windowChangeInfo.stateChangeFlags |= WINDOW_STATE_CHANGE_DISPLAY;

                    windowChangeInfo.displayId = newDisplayId;

                    // If the refresh rates have changed, we will need to go through the full
                    // decoder recreation path to ensure Pacer is switched to the new display
                    // and that we apply any V-Sync disablement rules that may be needed for
                    // this display.
                    const SDL_DisplayMode* oldMode = SDL_GetCurrentDisplayMode(currentDisplayId);
                    const SDL_DisplayMode* newMode = SDL_GetCurrentDisplayMode(newDisplayId);
                    if (oldMode == nullptr || newMode == nullptr ||
                            oldMode->refresh_rate != newMode->refresh_rate) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "Forcing renderer recreation due to refresh rate change between displays");
                        forceRecreation = true;
                    }
                }

                if (!forceRecreation && m_VideoDecoder->notifyWindowChanged(&windowChangeInfo)) {
                    // Update the window display mode based on our current monitor
                    // NB: Avoid a useless modeset by only doing this if it changed.
                    if (newDisplayId != currentDisplayId) {
                        currentDisplayId = newDisplayId;
                        updateOptimalWindowDisplayMode();
                    }

                    break;
                }
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Recreating renderer for window event: %d (%d %d)",
                        event.type,
                        event.window.data1,
                        event.window.data2);
            SDL_Event resetEvent = {};
            resetEvent.type = SDL_EVENT_RENDER_DEVICE_RESET;
            SDL_PushEvent(&resetEvent);
            break;
        }

        case SDL_EVENT_RENDER_DEVICE_RESET:
            if (presentationMappingDeferred && m_VideoDecoder == nullptr) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Initializing renderer before first Wayland presentation map");
            } else if (event.type == SDL_EVENT_RENDER_DEVICE_RESET) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Recreating renderer by internal request: %d",
                            event.type);
            }

            diagnosticPhase(SessionDiagnostics::Phase::CreatingDecoder);
            m_DecoderLock.lock();

            // Destroy the old decoder
            delete m_VideoDecoder;

            // Insert a barrier to discard any additional window events
            // that could cause the renderer to be and recreated again.
            // We don't use SDL_FlushEvent() here because it could cause
            // important events to be lost.
            flushWindowEvents();

            // Update the window display mode based on our current monitor
            // NB: Avoid a useless modeset by only doing this if it changed.
            if (currentDisplayId != SDL_GetDisplayForWindow(m_Window)) {
                currentDisplayId = SDL_GetDisplayForWindow(m_Window);
                updateOptimalWindowDisplayMode();
            }

            // Now that the old decoder is dead, flush any events it may
            // have queued to reset itself (if this reset was the result
            // of state loss).
            SDL_PumpEvents();
            SDL_FlushEvent(SDL_EVENT_RENDER_DEVICE_RESET);

            {
                // If the stream exceeds the display refresh rate (plus some slack),
                // forcefully disable V-sync to allow the stream to render faster
                // than the display.
                int displayHz = StreamUtils::getDisplayRefreshRate(m_Window);
                bool enableVsync = m_Preferences->enableVsync;
                if (displayHz + 5 < m_StreamConfig.fps) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Disabling V-sync because refresh rate limit exceeded");
                    enableVsync = false;
                }

                // Choose a new decoder (hopefully the same one, but possibly
                // not if a GPU was removed or something).
                if (!chooseDecoder(m_ActiveVideoFormat, m_ActiveVideoWidth,
                                   m_ActiveVideoHeight, m_ActiveVideoFrameRate,
                                   enableVsync, false,
                                   s_ActiveSession->m_VideoDecoder,
                                   isIdentityGbrEnabledForFormat(m_ActiveVideoFormat),
                                   decoderCaptureSource(), decoderEncoderBackend(), nullptr,
                                   m_Window, false,
                                   DecoderSelectionMode::PreferExactHardwareThenSoftware)) {
                    m_DecoderLock.unlock();
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Failed to recreate decoder after reset");
                    m_Diagnostics.end(SessionDiagnostics::End::DecoderFailed);
                    emit displayLaunchError(tr("Unable to initialize video decoder. Please check your streaming settings and try again."));
                    goto DispatchDeferredCleanup;
                }

                m_Diagnostics.decoderReady();
                publishDiagnosticLifecycle();

                // Input maps against what this renderer actually letterboxes
                // against: the live drawable, or the layout it copied.
                m_InputHandler->setLiveDrawableGeometry(
                            m_VideoDecoder->letterboxesAgainstLiveDrawable());
                m_InputHandler->notifyWindowGeometryChanged(m_Window,
                                                            "renderer");

                // As of SDL 2.0.12, SDL_RecreateWindow() doesn't carry over mouse capture
                // or mouse hiding state to the new window. By capturing after the decoder
                // is set up, this ensures the window re-creation is already done.
                if (needsPostDecoderCreationCapture) {
                    m_InputHandler->setCaptureActive(true);
                    needsPostDecoderCreationCapture = false;
                }
            }

            if (presentationMappingDeferred) {
                int logicalWidth = 0;
                int logicalHeight = 0;
                SDL_GetWindowSize(m_Window, &logicalWidth, &logicalHeight);
                if (logicalWidth <= 0 || logicalHeight <= 0) {
                    m_DecoderLock.unlock();
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Refusing to map invalid Wayland presentation geometry: %dx%d",
                                 logicalWidth, logicalHeight);
                    emit displayLaunchError(
                                tr("Unable to display the streaming window with valid dimensions."));
                    goto DispatchDeferredCleanup;
                }
                if (!SDL_SetWindowFullscreenMode(m_Window, nullptr) ||
                        !SDL_SetWindowFullscreen(m_Window, true)) {
                    m_DecoderLock.unlock();
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Failed to queue initial Wayland fullscreen state: %s",
                                 SDL_GetError());
                    emit displayLaunchError(
                                tr("Unable to enter fullscreen mode for the streaming window."));
                    goto DispatchDeferredCleanup;
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Mapping initialized Wayland presentation at %dx%d with fullscreen pending",
                            logicalWidth, logicalHeight);
                if (!SDL_ShowWindow(m_Window)) {
                    m_DecoderLock.unlock();
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Failed to map initialized Wayland presentation: %s",
                                 SDL_GetError());
                    emit displayLaunchError(
                                tr("Unable to display the initialized streaming window."));
                    goto DispatchDeferredCleanup;
                }
                if (!SDL_SyncWindow(m_Window)) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Timed out synchronizing initial Wayland presentation map: %s",
                                SDL_GetError());
                }
                presentationMappingDeferred = false;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Mapped initialized Wayland presentation surface");
                initializePlankToolbar();
            }

            // Request an IDR frame to complete the reset
            LiRequestIdrFrame();

            // Set HDR mode. We may miss the callback if we're in the middle
            // of recreating our decoder at the time the HDR transition happens.
            m_VideoDecoder->setHdrMode(false);

            // The replacement renderer has no copy of the prior overlay
            // texture, so publish the toolbar surface again after recreation.
            if (m_PlankToolbar) {
                m_PlankToolbar->notifyWindowChanged();
            }

            // After a window resize, we need to reset the pointer lock region
            m_InputHandler->updatePointerRegionLock();

            m_DecoderLock.unlock();
            break;

        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_KEY_DOWN:
            m_InputHandler->handleKeyEvent(&event.key);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (!m_InputHandler->immersiveMouseOwnsPointer() && m_PlankToolbar &&
                    event.button.windowID == SDL_GetWindowID(m_Window)) {
                const auto action = m_PlankToolbar->handleMouseButton(event.button);
#if defined(Q_OS_MACOS) && defined(PLANK_TRANSPORT)
                if (action == PlankToolbar::Action::SendFiles) {
                    m_InputHandler->raiseAllKeys();
                    if (const auto files = fileClipboard()) files->chooseFilesOnMainThread();
                    m_PlankToolbar->notifyFocusLost();
                    break;
                }
#endif
                if (action == PlankToolbar::Action::ToggleCamera) {
                    m_CameraRequested.store(!m_CameraRequested.load());
                    break;
                }
                if (action == PlankToolbar::Action::ToggleMicrophone) {
                    m_MicrophoneRequested.store(!m_MicrophoneRequested.load());
                    break;
                }
                if (action == PlankToolbar::Action::NewConnection) {
                    openNewConnection();
                    break;
                }
                if (action == PlankToolbar::Action::Disconnect) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "PLANK toolbar disconnect requested");
                    m_Diagnostics.end(SessionDiagnostics::End::Toolbar);
                    goto DispatchDeferredCleanup;
                }
                if (action == PlankToolbar::Action::ToggleFullscreen) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "PLANK toolbar fullscreen toggle requested");
                    toggleFullscreen();
                    m_PlankToolbar->notifyWindowChanged();
                    break;
                }
                if (action == PlankToolbar::Action::Minimize) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "PLANK toolbar minimize requested");
                    minimizePresentationWindows();
                    break;
                }
                if (action == PlankToolbar::Action::ApplyScreens) {
                    requestDisplayLayoutApply();
                    break;
                }
                if (action == PlankToolbar::Action::KeepScreens) {
                    keepDisplayLayout();
                    break;
                }
                if (action == PlankToolbar::Action::SetUpScreens) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PLANK screens changed: display setup requested");
                    emit displaySetupRequested();
                    break;
                }
                if (action == PlankToolbar::Action::Consumed) {
                    break;
                }
            }
            m_InputHandler->handleMouseButtonEvent(&event.button);
            break;
        case SDL_EVENT_MOUSE_MOTION:
        {
            bool toolbarConsumedMotion = false;
            if (!m_InputHandler->immersiveMouseOwnsPointer() && m_PlankToolbar &&
                    event.motion.windowID == SDL_GetWindowID(m_Window)) {
                // The ordinary input path batches queued motion for efficient
                // transport. Aggregate it here when the toolbar is present so
                // the toolbar tracker and host receive the identical delta.
                if (event.motion.which != SDL_TOUCH_MOUSEID) {
                    PlankMouseMotion::coalescePending(event.motion);
                }
                // The single-window toolbar observes the same authoritative
                // coordinates, but motion always remains remote-desktop input.
                // Only toolbar button and wheel events have exclusive local
                // ownership.
                toolbarConsumedMotion =
                        m_PlankToolbar->observeMouseMotion(event.motion);
            }
            if (!toolbarConsumedMotion) {
                m_InputHandler->handleMouseMotionEvent(
                            &event.motion, !m_PlankToolbar);
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            if (!m_InputHandler->immersiveMouseOwnsPointer() && m_PlankToolbar &&
                    event.wheel.windowID == SDL_GetWindowID(m_Window) &&
                    m_PlankToolbar->handleMouseWheel(event.wheel)) {
                break;
            }
            m_InputHandler->handleMouseWheelEvent(&event.wheel);
            break;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        case SDL_EVENT_PEN_PROXIMITY_IN:
        case SDL_EVENT_PEN_PROXIMITY_OUT:
            m_InputHandler->handlePenProximityEvent(&event.pproximity);
            break;
        case SDL_EVENT_PEN_DOWN:
        case SDL_EVENT_PEN_UP:
            m_InputHandler->handlePenTouchEvent(&event.ptouch);
            break;
        case SDL_EVENT_PEN_MOTION:
            m_InputHandler->handlePenMotionEvent(&event.pmotion);
            break;
        case SDL_EVENT_PEN_BUTTON_DOWN:
        case SDL_EVENT_PEN_BUTTON_UP:
            m_InputHandler->handlePenButtonEvent(&event.pbutton);
            break;
        case SDL_EVENT_PEN_AXIS:
            m_InputHandler->handlePenAxisEvent(&event.paxis);
            break;
#endif
        }
    }

DispatchDeferredCleanup:
    m_Lifecycle.stop();
    // Give the local desktop back before waiting for any network worker.
    // In particular, emergency disconnect must release immersive capture
    // even while a reconnect is still unwinding.
    m_InputHandler->setCaptureActive(false);
    m_InputHandler->raiseAllKeys();
    // The reconnect worker can replace the endpoint until it joins. For an
    // established stream, queue the close while the transport can still send.
    if (reconnectThread == nullptr) queuePlankTransportDisconnect();
    SDL_HideWindow(m_Window);
    for (SDL_Window* window : std::as_const(m_SecondaryWindows)) SDL_HideWindow(window);
    if (workerProbe != nullptr) {
        // The probe has a one-second HTTP deadline and owns no Session state.
        workerProbe->wait();
        delete workerProbe;
        workerProbe = nullptr;
    }
    if (reconnectThread != nullptr) {
        m_Lifecycle.stop();
        m_ConnectionStartCancelled.store(true);
        reconnectThread->wait();
        delete reconnectThread;
        reconnectThread = nullptr;
        m_Reconnecting.store(false);
        m_ReconnectRequested.store(false);
    }
    SDL_EnableScreenSaver();
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");
    if (QGuiApplication::platformName() == "eglfs") {
        QGuiApplication::restoreOverrideCursor();
    }

    // The toolbar owns a reference to the input handler, so destroy it before
    // releasing the handler itself.
    m_PlankToolbar.reset();

    // Destroy the input handler now. This must be destroyed
    // before allowing the UI to continue execution.
    destroyInputHandler();
    clearPlankReconnectCredentials();
    {
        QWriteLocker lock(&m_Computer->lock);
        m_Computer->sessionToken.fill(QChar('\0'));
        m_Computer->sessionToken.clear();
        m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
    }

#ifdef PLANK_TRANSPORT
    // Native media threads call directly into the active decoder and audio
    // renderer. Quiesce them before either renderer can be destroyed.
    stopPlankTransportMediaReceivers();
#endif

    // Destroy the decoder, since this must be done on the main thread
    // NB: This must happen before LiStopConnection() for pull-based
    // decoders.
    m_DecoderLock.lock();
    delete m_VideoDecoder;
    m_VideoDecoder = nullptr;
    m_DecoderLock.unlock();

    // Propagate state changes from the SDL window back to the Qt window
    //
    // NB: We're making a conscious decision not to propagate the maximized
    // or normal state of the window here. The thinking is that users may
    // routinely maximize the streaming window simply to view the stream
    // in a larger window, but they don't necessarily want the UI in such
    // a large window.
    if (!m_IsFullScreen && m_QtWindow != nullptr && m_Window != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
            m_QtWindow->setWindowStates(m_QtWindow->windowStates() | Qt::WindowMinimized);
        }
        else if (m_QtWindow->windowStates() & Qt::WindowMinimized) {
            m_QtWindow->setWindowStates(m_QtWindow->windowStates() & ~Qt::WindowMinimized);
        }
#else
        if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
            m_QtWindow->setWindowState(Qt::WindowMinimized);
        }
        else if (m_QtWindow->windowState() & Qt::WindowMinimized) {
            m_QtWindow->setWindowState(Qt::WindowNoState);
        }
#endif
    }

    // This must be called after the decoder is deleted, because
    // the renderer may want to interact with the window
    for (SDL_Window* window : std::as_const(m_SecondaryWindows)) {
        SDL_DestroyWindow(window);
    }
    m_SecondaryWindows.clear();
    SDL_DestroyWindow(m_Window);
    m_Window = nullptr;

    if (iconSurface != nullptr) {
        SDL_DestroySurface(iconSurface);
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    // Cleanup can take a while, so dispatch it to a worker thread.
    // When it is complete, it will release our s_ActiveSessionSemaphore
    // reference.
    QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
}
