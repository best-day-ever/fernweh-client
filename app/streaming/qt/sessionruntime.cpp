#include <QtGlobal>
#include <QDebug>
#include "../monotonicclock.h"
#include "sessionruntime.h"
#include "../../clientidentity.h"
#include "../session.h"
#include "../plankpresentation.h"
#include "../../backend/hostrecovery.h"
#include "../../backend/collaboration.h"
#include "../../backend/clientdisplays.h"
#include "../../backend/clientdisplayprobe.h"
#include "../../backend/nvhttp.h"
#include "../../settings/plankclientpolicy.h"
#include "../audio/microphone.h"
#include "../camera/camera.h"
#include <plank_transport_pointer.h>
#ifdef Q_OS_MACOS
#include "../../macapplication.h"
#include "../macclipboardsync.h"
#include "../macfileclipboard.h"
#include "../macdisplays.h"
#endif

#include <QEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QScopedValueRollback>
#include <algorithm>
#include <tuple>

QtSessionRuntime::QtSessionRuntime(Session* session) : QObject(session), m_Session(session)
{
    m_Input = std::make_unique<QtSessionInput>();
#ifdef Q_OS_MACOS
    m_Input->setPasteHandler([this] {
        if (!m_Session->clipboardSyncEnabled()) return true;
        return m_Session->beginFileClipboardPasteOnMainThread() ||
               m_Session->deferTextClipboardPasteOnMainThread();
    });
#endif
#ifdef Q_OS_LINUX
    m_WindowGuard = std::make_unique<QtWaylandSessionGuard>();
    if (PlankClientPolicy().metrics().enabled) {
        m_Input->setPenObserver([session](const PlankPenActivity& activity) { session->publishPenMetrics(activity); });
    }
#endif
    m_TargetBitrate = session->m_PlankBitrateKbps;
    m_CapturePolicy = session->m_Preferences->captureSysKeysMode;
    m_Fullscreen = session->m_IsFullScreen;
    m_RecoveryBitrate.store(m_TargetBitrate);
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this] { updateFocus(); });
#ifdef Q_OS_MACOS
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this] { updateFocus(); });
    m_Input->setNativeFocus([this] {
        if (m_Session->m_Lifecycle.stopping()) return false;
        for (const auto& shell : m_Windows)
            if (!shell->surface().displayMenuOpen() && shell->ownsKeyboard()) return true;
        return false;
    });
#endif
    m_DisplayDebounce.setSingleShot(true);
    m_DisplayDebounce.setInterval(1500);
    connect(&m_DisplayDebounce, &QTimer::timeout, this, &QtSessionRuntime::observeDisplays);
    const auto watchScreen = [this](QScreen* screen) {
        connect(screen, &QScreen::geometryChanged, this, [this] { m_DisplayDebounce.start(); });
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] { m_DisplayDebounce.start(); });
    };
    for (auto* screen : QGuiApplication::screens()) watchScreen(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this, watchScreen](QScreen* screen) {
        watchScreen(screen); m_DisplayDebounce.start();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] {
        // QWindow migrates to a surviving monitor. Preserve the Host desktop
        // while the observation settles and the user chooses Apply or Keep.
        m_Input->releaseAll();
        m_CollapsedDisplay = true;
        showPresentationWindows(); updateGeometry();
        m_DisplayDebounce.start();
    });
    connect(session, &Session::qtDisplayLayoutApplyRequested, this,
            &QtSessionRuntime::applyDisplayLayout, Qt::QueuedConnection);
    session->m_QtPreparePresentation = [this] { return preparePresentation(); };
    connect(m_Input.get(), &QtSessionInput::shortcutRequested, this, &QtSessionRuntime::handleShortcut);
    connect(m_Input.get(), &QtSessionInput::pointerCaptureFailed, this, [this](const QString& reason) {
        emit m_Session->displayLaunchError(reason); stop();
    });
    connect(m_Input.get(), &QtSessionInput::pointerCaptureNotice, this, [this](const QString& reason) {
        m_DisplayNotice = reason; m_DisplayNoticeDeadline = PlankClock::milliseconds() + 5000;
        setStatus(QString());
    });
#ifdef Q_OS_MACOS
    m_Input->setPointerCaptureHooks([this] {
        if (m_PointerWindow) return m_PointerWindow->beginPointerCapture();
        if (!m_PointerCandidateWindow || !m_PointerCandidateWindow->beginPointerCapture()) return false;
        m_PointerWindow = m_PointerCandidateWindow;
        return true;
    }, [this](QPointF point, bool reanchor) {
        if (m_PointerWindow) m_PointerWindow->finishPointerCapture({}, false);
        m_PointerWindow = nullptr;
        if (reanchor && QGuiApplication::applicationState() == Qt::ApplicationActive) {
            for (auto& shell : m_Windows) if (shell->surface().reanchorPointer(point)) break;
        }
    });
    connect(m_Input.get(), &QtSessionInput::pointerCaptureChanged, this, [this](bool active) {
        for (auto& shell : m_Windows) shell->surface().setPointerCaptured(active);
    });
    connect(m_Input.get(), &QtSessionInput::immersiveMouseChanged, this, [this](bool enabled) {
        for (auto& shell : m_Windows)
            shell->surface().setImmersiveMouse(enabled, shell->surface().immersiveMouseAvailable());
    });
#endif
    connect(session, &Session::qtStatisticsUpdated, this, [this](const QString& text) {
        for (auto& shell : m_Windows) shell->surface().setStatisticsText(text);
    });
    connect(session, &Session::activeSessionTakeoverRequested, this, [this] {
        // The existing launcher owns the authenticated takeover dialog. Never
        // bury it under a fullscreen stream surface while startup is waiting.
        for (auto& shell : m_Windows) shell->hide();
        if (m_Launcher) { m_Launcher->show(); m_Launcher->raise(); m_Launcher->requestActivate(); }
    });
    connect(session, &Session::connectionStarted, this, [this] {
        if (m_Session->m_Lifecycle.stopping()) return;
        showPresentationWindows();
        updateGeometry();
    });
#ifdef Q_OS_MACOS
    connect(static_cast<MacApplication*>(qGuiApp), &MacApplication::exitRequested, this, &QtSessionRuntime::stop);
#endif
    connect(session, &Session::qtCollaborationChanged, this, [this] {
        m_Input->releaseAll();
        setInputEnabled(m_InputGrabbed);
        updateFocus();
    }, Qt::QueuedConnection);
    connect(session, &Session::qtQuitRequested, this, &QtSessionRuntime::stop, Qt::QueuedConnection);
    connect(session, &Session::qtDecoderResetRequested, this, [this] { request(Command::ResetDecoder); }, Qt::QueuedConnection);
    connect(session, &Session::qtReconnectRequested, this, [this] { request(Command::Reconnect); }, Qt::QueuedConnection);
    connect(session, &Session::qtReconnectStatusChanged, this,
            [this](const QString& text, bool) { setStatus(text); }, Qt::QueuedConnection);
    m_Tick.setInterval(100);
    connect(&m_Tick, &QTimer::timeout, this, &QtSessionRuntime::tick);
    m_StartupDeadline.setSingleShot(true);
    m_StartupDeadline.setInterval(10000);
    connect(&m_StartupDeadline, &QTimer::timeout, this, [this] {
        emit m_Session->displayLaunchError(tr("The Qt video surface did not become ready."));
        stop();
    });
}

QtSessionRuntime::~QtSessionRuntime()
{
    // Session is retained until readyForDeletion, emitted only after this
    // worker has exited. Never join network or GPU work in the GUI destructor.
    Q_ASSERT(!m_Worker);
    QCoreApplication::instance()->removeEventFilter(this);
}

bool QtSessionRuntime::snapshotDisplays()
{
    const auto screens = QGuiApplication::screens();
    auto& session = *m_Session;
    const auto probe = ClientDisplayProbe::probe();
    if (screens.isEmpty() || probe.isEmpty()) return false;
    session.m_ClientDisplays.clear();
    session.m_ClientDisplayProbe = probe;
    session.m_GuiDisplaySnapshot.publish(probe);
    session.m_TargetDisplayId = 0;
    ClientDisplays::Selection selection;
    {
        QReadLocker lock(&session.m_Computer->lock);
        auto* target = m_Launcher ? m_Launcher->screen() : QGuiApplication::primaryScreen();
        selection = ClientDisplays::resolve(session.m_Computer->plankClientDisplays,
            ClientDisplays::snapshot(), target ? target->name() : QString());
    }
    m_DisplayNotice = selection.notice;
    for (int index = 0; index < screens.size(); ++index) {
        auto* screen = screens[index];
        const auto bounds = screen->geometry();
        const QSize pixels(qRound(bounds.width() * screen->devicePixelRatio()),
                           qRound(bounds.height() * screen->devicePixelRatio()));
        if (!bounds.isValid() || !pixels.isValid()) return false;
        Session::ClientDisplaySnapshot display;
        display.displayId = index + 1;
        display.logicalBounds = {bounds.x(), bounds.y(), bounds.width(), bounds.height()};
        display.nativeSize = pixels;
#ifdef Q_OS_MACOS
        if (!MacDisplays::currentModeForBounds(bounds, display.macBackingSize,
                                               display.macMatchedBounds, m_Fullscreen)) return false;
        display.nativeSize = display.macBackingSize;
#endif
        display.probeView = ClientDisplayProbe::forSessionDisplay(bounds, display.nativeSize, probe);
        if (display.probeView.key.isEmpty()) return false;
#ifdef Q_OS_MACOS
        display.matchTarget = display.macBackingSize;
#else
        display.matchTarget = NvOutputTopology::clientMatchTarget(display.probeView);
#endif
        display.primary = screen->name() == selection.target;
        display.legacySelected = selection.names.contains(screen->name());
        if (display.primary) session.m_TargetDisplayId = display.displayId;
        session.m_ClientDisplays.append(display);
    }
    auto& displays = session.m_ClientDisplays;
    std::sort(displays.begin(), displays.end(), [](const auto& a, const auto& b) {
        return std::make_pair(a.logicalBounds.x, a.logicalBounds.y) < std::make_pair(b.logicalBounds.x, b.logicalBounds.y);
    });
    if (!session.m_TargetDisplayId) session.m_TargetDisplayId = displays.first().displayId;
    session.m_MultiDisplayPresentationAvailable = std::count_if(displays.cbegin(), displays.cend(),
        [](const auto& display) { return display.legacySelected; }) > 1;
    m_SpanDisplays = selection.names.size() > 1;
    int x = 0;
    for (auto& display : displays) {
        display.canvasRect = QRect(QPoint(x, 0), display.nativeSize);
        x += display.nativeSize.width();
    }
    // Choose the first visible surface from the same saved plan the worker
    // will validate. Excluded monitors stay hidden even during GPU startup.
    if (!m_Worker && !session.isCollaborator()) {
        QString policy;
        NvOutputTopology topology;
        int flags = 0;
        {
            QReadLocker lock(&session.m_Computer->lock);
            policy = session.m_Computer->plankHostLayout;
            topology = session.m_Computer->outputTopology;
            flags = topology.launchFeatureFlags(session.m_Computer->plankFeatureFlags);
        }
        if (policy == NvOutputTopology::MatchClientHostLayout &&
            session.m_PlankCaptureSource != StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
            QVector<NvClientDisplay> planned;
            for (const auto& display : displays) planned.append(display.probeView);
            bool exact = true;
            const int available = topology.displayArrangementPublished() ? flags :
                flags & ~NvOutputTopology::DisplayArrangementFeature;
            if (!session.planDisplayArrangement(planned, available, topology, exact)) return false;
            session.applyArrangementPresentation();
            m_SpanDisplays = false; // The worker exposes the remaining planned surfaces before its probe.
        }
    }
    m_ObservedProbe = probe;
    return true;
}

void QtSessionRuntime::start(QWindow* launcher)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_Started) return;
    m_Started = true;
    m_Session->m_ThreadedExec = true;
    m_Launcher = launcher;
    m_PreviousQuitOnLastWindow = QGuiApplication::quitOnLastWindowClosed();
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::instance()->installEventFilter(this);
    if (!snapshotDisplays()) {
        emit m_Session->displayLaunchError(tr("Unable to read the Client display layout."));
        stop(); return;
    }
#ifdef Q_OS_MACOS
    // Allocate once on the GUI thread before workers can start/stop the sync
    // state. Its internal mutex then protects polling across reconnections.
    m_Session->createClipboardSync();
#endif
    m_Presenter = std::make_unique<QtVideoPresenter>();
    const auto screens = QGuiApplication::screens();
    // Primary first: device creation and the A/V presentation clock use this
    // same output. Canvas coordinates retain physical left-to-right order.
    auto presentationScreens = screens;
    auto* target = screens[int(m_Session->m_TargetDisplayId) - 1];
    const bool canSpan = ClientDisplays::canSpan(ClientDisplays::snapshot());
    Q_UNUSED(canSpan);
    QSize desktop;
    {
        QReadLocker lock(&m_Session->m_Computer->lock);
        desktop = QSize(m_Session->m_Computer->outputTopology.desktopWidth,
                        m_Session->m_Computer->outputTopology.desktopHeight);
    }
    std::stable_sort(presentationScreens.begin(), presentationScreens.end(), [target](const auto* a, const auto* b) {
        return (a == target) > (b == target);
    });
    for (auto* screen : presentationScreens) {
        const auto display = ClientDisplayProbe::forSessionDisplay(screen->geometry(),
            screen->size() * screen->devicePixelRatio(), m_Session->m_ClientDisplayProbe);
        addWindow(screen, display.key, screen == target, desktop);
    }
    connect(m_Presenter.get(), &QtVideoPresenter::deviceReady, this, [this](bool available, const QString& reason) {
        m_StartupDeadline.stop();
        if (m_Session->m_Lifecycle.stopping()) return;
        if (!available) {
            emit m_Session->displayLaunchError(tr("No exact-format Qt video device is available: %1").arg(reason));
            stop(); return;
        }
#ifdef Q_OS_MACOS
        m_Factory = std::make_unique<QtDecodedVideoRendererFactory>(m_Presenter->deliveryFactory());
#else
        m_Factory = std::make_unique<QtDecodedVideoRendererFactory>(m_Presenter->softwareDevice(),
            m_Presenter->vaapiRenderNode(), m_Presenter->deliveryFactory());
#endif
        m_Worker = QThread::create([this] { workerMain(); });
        m_Worker->setObjectName(QStringLiteral("Qt Session Worker"));
        connect(m_Worker, &QThread::finished, this, &QtSessionRuntime::finishWorker);
        m_Worker->start();
    });
    connect(m_Presenter.get(), &QtVideoPresenter::presentationFailed, this, [this](const QString& reason) {
        emit m_Session->displayLaunchError(reason); stop();
    });
    m_Session->m_Diagnostics.windowCreated();
    m_Session->publishDiagnosticLifecycle();
    showPresentationWindows();
    m_Tick.start();
#ifdef Q_OS_LINUX
    m_WindowGuard->setIdleWindow(&m_Windows.front()->surface());
#endif
    m_StartupDeadline.start();
    m_Presenter->initializeDevice();
}

void QtSessionRuntime::addWindow(QScreen* screen, const QString& key, bool primary, QSize desktop)
{
    const bool canSpan = QGuiApplication::screens().size() > 1;
    auto shell = std::make_unique<QtPlatformSessionShell>();
    shell->setScreen(screen);
    m_Screens.append(shell->screen());
    shell->setProperty("plankDisplayKey", key);
    shell->setTitle(m_Session->m_Computer->name + QStringLiteral(" — ") +
                    QString::fromLatin1(PlankClientIdentity::displayName));
#ifdef Q_OS_MACOS
    shell->setKeepAwake(primary && m_Session->m_Preferences->keepAwake);
    connect(shell.get(), &QtMetalSessionShell::renderingFailed, this, [this](QString reason) {
        emit m_Session->displayLaunchError(reason); stop();
    });
#endif
    shell->prepareForSession(desktop, m_Fullscreen && (primary || m_SpanDisplays));
    if (m_Session->m_Lifecycle.stopping()) return;
    auto& surface = shell->surface();
    shell->installEventFilter(this);
    surface.installEventFilter(this);
    surface.setDisplayMode(canSpan, m_SpanDisplays);
    surface.setLiveSession(true);
    surface.setStatusText(tr("Connecting to workstation…"));
    surface.setBitrateKbps(m_Session->m_PlankBitrateKbps);
    surface.setPinned(m_Session->m_Preferences->plankToolbarPinned);
    surface.setStatisticsVisible(m_Session->m_Preferences->showPerformanceOverlay);
    surface.setDragMapper([this, &surface, owner = shell.get()](QPointF logical, QPointF& stream) {
        if (!m_SpanDisplays || m_CollapsedDisplay || m_Windows.size() < 2 || !surface.screen())
            return surface.presentation().toStream(logical, stream, true);
        QVector<QtSessionOutputGeometry> outputs;
        for (const auto& window : m_Windows) {
            if (window->isVisible() && window->screen())
                outputs.append({QRectF(window->mapToGlobal(QPoint()), window->surface().size()),
                                window->surface().presentation()});
        }
        return qtMapDraggedPointer(outputs, QPointF(owner->mapToGlobal(QPoint())) + logical, stream);
    });
    connect(&surface, &QtSessionWindow::physicalMouseActivity, m_Input.get(), &QtSessionInput::activateMouse);
    connect(&surface, &QtSessionWindow::remoteMotion, m_Input.get(), &QtSessionInput::mouseMotion);
    connect(&surface, &QtSessionWindow::remoteButton, this,
            [this](int button, bool down, QPointF position) {
        m_Input->mouseButton(button, down, position);
    });
    connect(&surface, &QtSessionWindow::immersiveMouseRequested, this,
            [this, window = shell.get()] { toggleImmersiveMouse(window); });
#ifdef Q_OS_MACOS
    connect(shell.get(), &QtMetalSessionShell::pointerDelta, m_Input.get(), &QtSessionInput::mouseDelta);
    connect(shell.get(), &QtMetalSessionShell::pointerCaptureLost, m_Input.get(), &QtSessionInput::releaseAll);
    connect(shell.get(), &QtMetalSessionShell::tabletPointerInput, m_Input.get(), &QtSessionInput::cancelPointerCapture);
#endif
    connect(&surface, &QtSessionWindow::remoteWheel, m_Input.get(), &QtSessionInput::wheel);
    connect(&surface, &QtSessionWindow::remoteKey, m_Input.get(), &QtSessionInput::key);
#ifdef Q_OS_MACOS
    connect(&surface, &QtSessionWindow::remotePen, m_Input.get(), &QtSessionInput::normalizedPen);
    connect(&surface, &QtSessionWindow::remotePenCancelled, m_Input.get(), &QtSessionInput::penLeave);
    surface.setNativeKeyCapture([this] { return m_Input->capturesNativeKeys(); });
#endif
    connect(m_Input.get(), &QtSessionInput::cursorShape, &surface, &QtSessionWindow::setCursorShape);
    connect(m_Input.get(), &QtSessionInput::cursorPosition, &surface, &QtSessionWindow::setHostCursorPosition);
    connect(m_Input.get(), &QtSessionInput::tabletActive, &surface, &QtSessionWindow::setTabletActive);
    connect(&surface, &QtSessionWindow::toolbarChanged, this, [&surface, this] {
        auto& preferences = *m_Session->m_Preferences;
        if (preferences.plankToolbarPinned != surface.pinned()) {
            preferences.plankToolbarPinned = surface.pinned(); preferences.save();
            for (auto& window : m_Windows) window->surface().setPinned(surface.pinned());
        }
    });
    connect(&surface, &QtSessionWindow::bitrateRequested, this, [this](int bitrate) {
        if (bitrate == m_TargetBitrate) return;
        m_TargetBitrate = bitrate; m_RecoveryBitrate.store(bitrate);
        m_BitrateChanged = PlankClock::milliseconds();
        for (auto& window : m_Windows) window->surface().setBitrateKbps(bitrate);
    });
    connect(&surface, &QtSessionWindow::filesRequested, this, [this] {
#ifdef Q_OS_MACOS
        if (m_Session->clipboardSyncEnabled())
            if (const auto files = m_Session->fileClipboard()) files->chooseFilesOnMainThread();
#endif
    });
    connect(&surface, &QtSessionWindow::microphoneRequested, this, [this] {
        m_Session->m_MicrophoneRequested.store(!m_Session->m_MicrophoneRequested.load());
    });
    connect(&surface, &QtSessionWindow::cameraRequested, this, [this] {
        m_Session->m_CameraRequested.store(!m_Session->m_CameraRequested.load());
    });
    connect(&surface, &QtSessionWindow::newConnectionRequested, this, [this] {
        m_Session->openNewConnection();
    });
    connect(&surface, &QtSessionWindow::disconnectRequested, this, [this] {
        m_Session->m_Diagnostics.end(SessionDiagnostics::End::Toolbar);
        stop();
    });
    connect(&surface, &QtSessionWindow::fullscreenRequested, this, &QtSessionRuntime::toggleFullscreen);
    connect(&surface, &QtSessionWindow::fullscreenDisplaysRequested, this, [this, owner = shell.get()](bool span) {
        selectFullscreenDisplays(span, owner);
    });
    connect(&surface, &QtSessionWindow::screensApplyRequested, this, &QtSessionRuntime::applyDisplayLayout);
    connect(&surface, &QtSessionWindow::screensKeepRequested, this, [this] {
        m_DisplayChanges.keep(); updateScreensPrompt();
    });
    connect(&surface, &QtSessionWindow::screensSetupRequested, this, [this] {
        if (m_Launcher) { m_Launcher->show(); m_Launcher->raise(); m_Launcher->requestActivate(); }
        emit m_Session->displaySetupRequested();
    });
    connect(&surface, &QtSessionWindow::displayMenuChanged, this, &QtSessionRuntime::updateFocus);
    connect(&surface, &QtSessionWindow::minimizeRequested, this, &QtSessionRuntime::minimize);
    connect(&surface, &QtSessionWindow::inputRecaptureRequested, this, [this] {
        if (!m_Session->m_Lifecycle.streaming() || m_Session->m_Lifecycle.stopping() || m_ReconnectNoticeDeadline) return;
        m_InputGrabbed = true;
        setInputEnabled(true);
    });
    connect(&surface, &QtSessionWindow::surfaceSizeChanged, this, &QtSessionRuntime::updateGeometry);
    connect(shell.get(), &QWindow::windowStateChanged, this, [this] { updateGeometry(); });
    m_Presenter->addOutput(&surface, {}, primary);
    m_Windows.push_back(std::move(shell));
}

QtPlatformSessionShell* QtSessionRuntime::windowForKey(const QString& key) const
{
    for (const auto& shell : m_Windows)
        if (shell->property("plankDisplayKey").toString() == key) return shell.get();
    return nullptr;
}

bool QtSessionRuntime::preparePresentation()
{
    auto& s = *m_Session;
    QVector<NvClientDisplay> displays;
    for (const auto& display : s.m_ClientDisplays) displays.append(display.probeView);
    NvOutputTopology topology;
    { QReadLocker lock(&s.m_Computer->lock); topology = s.m_Computer->outputTopology; }
    auto layout = qtSessionLayout(s.m_DisplayPlan, displays, topology,
        QSize(s.m_StreamConfig.width, s.m_StreamConfig.height), s.isCollaborator(), s.m_CollaborationOutputId);
    if (!layout.desktopSize.isValid())
        layout.desktopSize = QSize(topology.desktopWidth, topology.desktopHeight);
    if (!layout.desktopSize.isValid()) layout.desktopSize = layout.streamSize;
    if (layout.guest && layout.outputs.isEmpty()) {
        emit s.displayLaunchError(tr("The owner's captured display is no longer available. Join the session again."));
        return false;
    }
    return m_LayoutHandoff.request(this, [this, layout, displays] {
        return !m_Closing && applyPresentation(layout, displays);
    });
}

bool QtSessionRuntime::applyPresentation(const QtSessionLayout& layout,
                                         const QVector<NvClientDisplay>& probe)
{
    if (m_Closing || QGuiApplication::screens().isEmpty()) return false;
    m_Input->releaseAll();
    m_Layout = layout;
    m_LayoutProbe = probe;
    m_StreamSize = layout.streamSize;
    if (m_MarkApplied || !m_BaselineApplied) m_CollapsedDisplay = false;
    m_Views.clear();
    auto* fallback = m_Windows.empty() ? QGuiApplication::primaryScreen() : m_Windows.front()->screen();
    if (!fallback || !QGuiApplication::screens().contains(fallback)) fallback = QGuiApplication::primaryScreen();
    if (!fallback) fallback = QGuiApplication::screens().first();
    QtPlatformSessionShell* primary = nullptr;
    const auto current = ClientDisplayProbe::probe();
    QVector<QRect> guestRects;
    for (const auto& output : layout.outputs) guestRects.append(output.desktopPixels.toRect());
    const auto placement = layout.guest ? PlankCollaboration::windowPlacement(guestRects, fallback->availableGeometry()) : QVector<QRect>();
    {
        QScopedValueRollback<bool> changing(m_ChangingPresentation, true);
        for (int index = 0; index < layout.outputs.size(); ++index) {
            const auto& output = layout.outputs[index];
            QScreen* screen = nullptr;
            if (layout.guest) screen = fallback;
            else for (auto* candidate : QGuiApplication::screens()) {
                const auto view = ClientDisplayProbe::forSessionDisplay(candidate->geometry(),
                    candidate->size() * candidate->devicePixelRatio(), current);
                if (view.key == output.key) { screen = candidate; break; }
            }
            auto* shell = windowForKey(output.key);
            if (!screen) { m_CollapsedDisplay = true; continue; }
            if (!shell) { addWindow(screen, output.key, false, layout.desktopSize); shell = m_Windows.back().get(); }
            shell->setScreen(screen);
            if (layout.guest && placement.size() == layout.outputs.size()) shell->setGeometry(placement[index]);
            m_Views.insert(shell, output);
            if (output.primary || !primary) primary = shell;
        }
        if (!layout.planned && !m_Windows.empty()) primary = m_Windows.front().get();
        if (!primary) {
            // A selected monitor disappeared during negotiation. Keep a
            // scaled view of the existing primary output on a surviving seat.
            if (m_Windows.empty()) addWindow(fallback, QStringLiteral("fallback"), true, layout.desktopSize);
            primary = m_Windows.front().get();
            primary->setScreen(fallback);
            if (!layout.outputs.isEmpty()) m_Views.insert(primary, layout.outputs.first());
            m_CollapsedDisplay = true;
        }
        const auto first = std::find_if(m_Windows.begin(), m_Windows.end(), [primary](const auto& shell) { return shell.get() == primary; });
        if (first != m_Windows.end()) std::rotate(m_Windows.begin(), first, std::next(first));
        if (!m_Presenter->setPrimaryOutput(&primary->surface())) return false;
        if (layout.planned) m_SpanDisplays = layout.outputs.size() > 1;
        if (layout.guest) m_Fullscreen = false;
        for (auto& shell : m_Windows)
            shell->surface().setDisplayMode(layout.planned ? layout.outputs.size() > 1 :
                ClientDisplays::canSpan(ClientDisplays::snapshot()), m_SpanDisplays && !m_CollapsedDisplay);
    }
    showPresentationWindows();
    updateGeometry();
    return true;
}

void QtSessionRuntime::observeDisplays()
{
    if (m_Closing || !m_BaselineApplied || m_Session->isCollaborator()) return;
    const auto observed = ClientDisplayProbe::probe();
    if (observed.isEmpty() || !m_DisplayChanges.observe(observed)) return;
    m_ObservedProbe = observed;
    m_CollapsedDisplay = !m_DisplayChanges.matchesApplied();
    if (!m_CollapsedDisplay && m_Layout.planned) applyPresentation(m_Layout, m_LayoutProbe);
    else { showPresentationWindows(); updateGeometry(); }
    updateScreensPrompt();
}

void QtSessionRuntime::updateScreensPrompt()
{
    QSettings settings;
    QString text;
    if (!m_Session->isCollaborator() && m_DisplayChanges.shouldPrompt(settings))
        text = tr("Screens changed: %1").arg(ClientDisplayProbe::label(m_ObservedProbe));
    for (auto& shell : m_Windows) shell->surface().setScreensPrompt(text);
}

void QtSessionRuntime::applyDisplayLayout()
{
    auto& s = *m_Session;
    if (m_Closing || s.isCollaborator() || !s.m_CanReconnect.load() ||
        !s.hasPlankCredentials() || !s.m_Lifecycle.requestRecovery()) return;
    m_ApplyLayoutRequested.store(true);
    s.m_DisplayReconfigureRequested.store(true);
    m_MarkApplied = true;
    for (auto& shell : m_Windows) shell->surface().setScreensPrompt({});
    request(Command::Reconnect);
}

bool QtSessionRuntime::eventFilter(QObject* object, QEvent* event)
{
    if (event->type() == QEvent::Close) {
        for (const auto& shell : m_Windows) {
            if (object == shell.get() || object == &shell->surface()) {
                m_Session->m_Diagnostics.end(SessionDiagnostics::End::WindowClose);
                break;
            }
        }
    }
#ifdef Q_OS_MACOS
    if (event->type() == QEvent::TabletLeaveProximity)
        for (auto& shell : m_Windows) shell->surface().cancelPen();
#endif
#ifdef Q_OS_LINUX
    if (object == QCoreApplication::instance() && event->type() == QEvent::Quit && !m_Finished) {
        m_ExitApplication = true;
        m_Session->m_Diagnostics.end(SessionDiagnostics::End::ApplicationExit);
        stop();
        return true;
    }
#endif
    return QObject::eventFilter(object, event);
}

void QtSessionRuntime::request(Command command)
{
    if (m_Session->m_Lifecycle.stopping()) return;
    if (command == Command::ResetDecoder && !m_Session->m_Lifecycle.streaming()) return;
    if (command == Command::Reconnect) {
        if (m_Session->m_Lifecycle.state() != PlankSessionLifecycle::State::RecoveryQueued) return;
        setInputEnabled(false);
        beginRecovery();
    }
    {
        std::lock_guard<std::mutex> lock(m_CommandMutex);
        if (int(command) > int(m_Command)) m_Command = command;
    }
    m_CommandChanged.notify_all();
}

void QtSessionRuntime::stop()
{
    if (m_Closing) return;
    m_Closing = true;
    m_Session->m_Lifecycle.stop();
    m_LayoutHandoff.cancel();
    m_Session->m_GuiDisplaySnapshot.cancel();
    m_DisplayDebounce.stop();
    m_Session->m_ReconnectPolicy.allowUntil(0);
    m_Session->cancelConnectionStart();
    m_Session->m_CanReconnect.store(false);
    m_StartupDeadline.stop();
    m_Tick.stop();
    setInputEnabled(false);
#ifdef Q_OS_LINUX
    m_WindowGuard->setIdleWindow(nullptr);
#endif
    for (auto& shell : m_Windows) shell->hide();
    if (m_Presenter) m_Presenter->stop(); // also wakes test-frame qualification
    {
        std::lock_guard<std::mutex> lock(m_CommandMutex);
        m_Command = Command::Stop;
    }
    m_CommandChanged.notify_all();
    if (!m_Worker) QTimer::singleShot(0, this, &QtSessionRuntime::finishWorker);
}

bool QtSessionRuntime::resetDecoder()
{
    auto& s = *m_Session;
    s.m_DecoderLock.lock();
    delete s.m_VideoDecoder;
    s.m_VideoDecoder = nullptr;
    s.m_DecoderLock.unlock();
    if (m_Session->m_Lifecycle.stopping()) return false;
    s.diagnosticPhase(SessionDiagnostics::Phase::CreatingDecoder);
    IVideoDecoder* decoder = nullptr;
    const bool ready = Session::chooseDecoder(s.m_ActiveVideoFormat, s.m_ActiveVideoWidth, s.m_ActiveVideoHeight,
        s.m_ActiveVideoFrameRate, true, false, decoder, s.isIdentityGbrEnabledForFormat(s.m_ActiveVideoFormat),
        s.decoderCaptureSource(), s.decoderEncoderBackend(), m_Factory.get());
    if (!ready || m_Session->m_Lifecycle.stopping()) {
        if (!ready) s.m_Diagnostics.end(SessionDiagnostics::End::DecoderFailed);
        delete decoder; return false;
    }
    s.m_Diagnostics.decoderReady();
    s.publishDiagnosticLifecycle();
    s.m_DecoderLock.lock();
    s.m_VideoDecoder = decoder;
    s.m_DecoderLock.unlock();
    LiRequestIdrFrame();
    return true;
}

void QtSessionRuntime::workerMain()
{
    auto& s = *m_Session;
    for (;;) {
        if (m_Session->m_Lifecycle.stopping()) return;
        if (Session::s_ActiveSessionSemaphore.tryAcquire(1, 50)) break;
    }
    m_OwnsSession = true;
    Session::s_ActiveSession = &s;
    s.m_QtInputCallbacks.publish(m_Input.get());
    s.diagnosticPhase(SessionDiagnostics::Phase::Initializing);
    bool ready = !m_Session->m_Lifecycle.stopping() && s.initialize(m_Factory.get()) && !m_Session->m_Lifecycle.stopping();
    if (ready) {
        m_Input->setStreamSize(QSize(s.m_StreamConfig.width, s.m_StreamConfig.height));
        s.diagnosticPhase(SessionDiagnostics::Phase::Negotiating);
        ready = s.startConnectionAsync() && !m_Session->m_Lifecycle.stopping();
    }
    if (ready) {
        m_Input->startTablets(s.isCollaborator() ? 0 : LiGetHostFeatureFlags());
        ready = preparePresentation() && resetDecoder() && s.m_Lifecycle.started();
    }
    const auto publishStreaming = [this, &s] {
        const QSize size(s.m_ActiveVideoWidth, s.m_ActiveVideoHeight);
        QMetaObject::invokeMethod(this, [this, size] { setStreaming(size); }, Qt::QueuedConnection);
    };
    if (ready) publishStreaming();
    // Initialization, connection and decoder failure sites report their own
    // actionable error. Do not replace it with a second generic format error.
    quint64 nextProbe = 0;
    while (ready && !m_Session->m_Lifecycle.stopping()) {
        Command command;
        {
            std::unique_lock<std::mutex> lock(m_CommandMutex);
            m_CommandChanged.wait_for(lock, std::chrono::milliseconds(100), [this] { return m_Session->m_Lifecycle.stopping() || m_Command != Command::None; });
            command = m_Command;
            m_Command = Command::None;
        }
        if (m_Session->m_Lifecycle.stopping()) break;
        if (command == Command::Reconnect) {
            m_Input->beginReconnect();
            Session::PlankReconnectState state;
            if (!s.beginPlankReconnect(state) || m_Session->m_Lifecycle.stopping()) break;
            m_Input->resetCursorEpoch();
            if (m_ApplyLayoutRequested.exchange(false)) {
                { std::lock_guard<std::mutex> lock(s.m_DecoderLock);
                  delete s.m_VideoDecoder; s.m_VideoDecoder = nullptr; }
                state.retainedRenderer = false;
                if (!m_LayoutHandoff.request(this, [this] { return snapshotDisplays(); })) break;
            }
            s.m_StreamConfig.bitrate = m_RecoveryBitrate.load();
            const bool connected = s.runPlankReconnect() && !m_Session->m_Lifecycle.stopping();
            // Use the established recovery state machine. It requests a
            // decoder reset only if format/size changed; otherwise retain it.
            bool needsReset = false;
            ready = s.finishPlankReconnect(connected, state, &needsReset) && !m_Session->m_Lifecycle.stopping();
            if (ready) m_Input->finishReconnect();
            if (ready) ready = preparePresentation();
            if (ready && needsReset) ready = resetDecoder();
            if (ready) ready = s.m_Lifecycle.recovered();
            if (ready) publishStreaming();
        } else if (command == Command::ResetDecoder && s.m_Lifecycle.streaming()) {
            ready = resetDecoder();
            if (ready) publishStreaming();
        }
        if (!ready || m_Session->m_Lifecycle.stopping()) break;
        const auto now = PlankClock::milliseconds();
        if (now >= nextProbe && !s.m_Lifecycle.recoveryPending() && s.m_CanReconnect.load() &&
                PlankHostRecovery::videoSilent(now, s.m_LastPlankVideoReceived.load()) && !s.m_PlankWorkerInstance.isEmpty()) {
            nextProbe = now + PlankHostRecovery::ProbeIntervalMs;
            try {
                NvHTTP http(s.m_Computer->activeAddress);
                if (http.probeWorkerReplacement(s.m_PlankWorkerInstance, s.m_PlankHostCertificateSha256))
                    Session::clConnectionTerminated(-2);
            } catch (const GfeHttpResponseException&) {
            } catch (const QtNetworkReplyException&) {}
        }
    }
    // All shared media state is retired on this worker, before releasing the
    // active-session token or allowing another session to start.
    s.m_CanReconnect.store(false);
    s.m_Lifecycle.stop();
    QMetaObject::invokeMethod(this, [this] { setInputEnabled(false); }, Qt::QueuedConnection);
    s.m_QtInputCallbacks.clear();
    m_Input->shutdown();
    s.stopPlankTransportMediaReceivers();
    {
        std::lock_guard<std::mutex> decoderLock(s.m_DecoderLock);
        delete s.m_VideoDecoder;
        s.m_VideoDecoder = nullptr;
    }
    LiStopConnection();
    s.stopPlankTransportDataPlane();
    s.clearPlankReconnectCredentials();
    {
        QWriteLocker lock(&s.m_Computer->lock);
        s.m_Computer->sessionToken.fill(QChar('\0'));
        s.m_Computer->sessionToken.clear();
        s.m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
    }
}

void QtSessionRuntime::finishWorker()
{
    if (m_Finished) return;
    if (m_Worker && !m_Worker->wait(0)) {
        QTimer::singleShot(10, this, &QtSessionRuntime::finishWorker); return;
    }
#ifdef Q_OS_MACOS
    // GPU work is retired asynchronously. Never join a drawable/GPU wait while
    // the GUI owns input, a permission dialog or the application Quit sequence.
    for (auto& shell : m_Windows) shell->stopRendering();
    for (const auto& shell : m_Windows) if (!shell->renderingStopped()) {
        QTimer::singleShot(10, this, &QtSessionRuntime::finishWorker); return;
    }
#endif
    m_Finished = true;
    m_Session->m_Lifecycle.finished();
    m_StartupDeadline.stop(); m_Tick.stop();
    setInputEnabled(false);
#ifdef Q_OS_LINUX
    m_WindowGuard->setIdleWindow(nullptr);
#endif
    delete m_Worker; m_Worker = nullptr;
    if (m_Presenter) m_Presenter->stop();
    m_Factory.reset();
    m_Presenter.reset();
    m_Windows.clear();
    m_Session->m_QtPreparePresentation = {};
    if (m_OwnsSession) {
        m_Session->m_RendererFactory = nullptr;
        Session::s_ActiveSession = nullptr;
        Session::s_ActiveSessionSemaphore.release();
        m_OwnsSession = false;
    }
    QCoreApplication::instance()->removeEventFilter(this);
    QGuiApplication::setQuitOnLastWindowClosed(m_PreviousQuitOnLastWindow);
    // Do not access this after readyForDeletion: QML may collect Session.
    if (m_ExitApplication) QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
    const QPointer<Session> session(m_Session);
    session->publishDiagnosticLifecycle(true);
    emit session->sessionFinished();
    if (session) emit session->readyForDeletion();
}

void QtSessionRuntime::setStreaming(QSize size)
{
    if (!m_Session->m_Lifecycle.streaming() || m_Closing) return;
    m_StreamSize = size;
    if (!m_DisplayNotice.isEmpty() && !m_DisplayNoticeDeadline)
        m_DisplayNoticeDeadline = PlankClock::milliseconds() + 12000;
    m_Input->setStreamSize(m_Layout.desktopSize.isValid() ? m_Layout.desktopSize : size);
    m_Input->setVideoSize(size);
#ifdef Q_OS_MACOS
    m_Input->setPointerCaptureSupported(!m_Session->isCollaborator() && (m_Session->m_PlankLaunchFeatureFlags & PLANK_POINTER_CAPTURE_FEATURE) != 0);
    for (auto& shell : m_Windows) shell->surface().setImmersiveMouse(m_Input->immersiveMouse(),
        !m_Session->isCollaborator() && (m_Session->m_PlankLaunchFeatureFlags & PLANK_POINTER_CAPTURE_FEATURE) != 0);
#endif
    m_ReconnectNoticeDeadline = 0;
    if (!m_BaselineApplied || m_MarkApplied) {
        m_DisplayChanges.applied(m_LayoutProbe);
        m_BaselineApplied = true; m_MarkApplied = false;
    }
    updateScreensPrompt();
    updateGeometry();
    setStatus(m_Presenter->hasPresentedLiveFrame() ? QString() : tr("Connecting to workstation…"));
    for (auto& shell : m_Windows) {
        shell->surface().setWaiting(false);
        shell->surface().setEmbeddedCursor(!(LiGetHostFeatureFlags() & LI_FF_LOCAL_CURSOR));
    }
    setInputEnabled(m_InputGrabbed);
}

void QtSessionRuntime::updateGeometry()
{
    if (m_ChangingPresentation || !m_Presenter || m_Windows.empty() || !m_StreamSize.isValid()) return;
    m_Input->releaseAll();
    QScopedValueRollback<bool> updating(m_ChangingPresentation, true);
    const bool fullscreen = m_Windows.front()->windowState() == Qt::WindowFullScreen;
    const bool multi = fullscreen && m_SpanDisplays && m_Windows.size() > 1;
    if (m_Windows.front()->windowState() == Qt::WindowMinimized) {
        for (size_t i = 1; i < m_Windows.size(); ++i) m_Windows[i]->hide();
        updateFocus();
        return;
    }
    if (m_Layout.planned) {
        for (size_t index = 0; index < m_Windows.size(); ++index) {
            auto& shell = *m_Windows[index];
            if (!m_Views.contains(&shell) || (m_CollapsedDisplay && index)) { shell.hide(); continue; }
            const auto& view = m_Views[&shell];
            auto& surface = shell.surface();
            const QSize drawable(qRound(surface.width() * shell.devicePixelRatio()), qRound(surface.height() * shell.devicePixelRatio()));
            if (!drawable.isValid()) continue;
            const auto slice = PlankPresentation::sliceForSource(m_StreamSize, view.sourcePixels, drawable);
            const QtSessionGeometry geometry{m_StreamSize, slice.sourceRect, slice.destinationRect,
                drawable, surface.size(), m_Layout.desktopSize, view.desktopPixels};
            surface.setPresentation(geometry);
            m_Presenter->setGeometry(&surface, geometry);
        }
        updateFocus();
        return;
    }
    QSize canvas(0, 0);
    QMap<QtPlatformSessionShell*, QRect> outputRects;
    if (multi) {
        std::vector<QtPlatformSessionShell*> ordered;
        for (const auto& shell : m_Windows) {
            if (!shell->screen()) return; // screenRemoved owns controlled teardown
            ordered.push_back(shell.get());
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return a->screen()->geometry().x() < b->screen()->geometry().x();
        });
        for (auto* shell : ordered) {
            const auto& surface = shell->surface();
            const QSize pixels(qRound(surface.width() * shell->devicePixelRatio()), qRound(surface.height() * shell->devicePixelRatio()));
            outputRects.insert(shell, QRect(QPoint(canvas.width(), 0), pixels));
            canvas.rwidth() += pixels.width();
            canvas.setHeight(qMax(canvas.height(), pixels.height()));
        }
    }
    for (size_t index = 0; index < m_Windows.size(); ++index) {
        auto& shell = *m_Windows[index];
        if (index && !multi) { shell.hide(); continue; }
        if (index && !shell.isVisible()) shell.showSession(true);
        auto& surface = shell.surface();
        const QSize drawable(qRound(surface.width() * shell.devicePixelRatio()), qRound(surface.height() * shell.devicePixelRatio()));
        if (!drawable.isValid()) continue;
        QRect output(QPoint(), drawable);
        if (multi) output = outputRects.value(&shell);
        const auto slice = PlankPresentation::sliceForDrawable(m_StreamSize, multi ? canvas : drawable, output, drawable);
        const QSize desktop = m_Layout.desktopSize.isValid() ? m_Layout.desktopSize : m_StreamSize;
        const QRectF input(slice.sourceRect.x() * desktop.width() / m_StreamSize.width(),
            slice.sourceRect.y() * desktop.height() / m_StreamSize.height(),
            slice.sourceRect.width() * desktop.width() / m_StreamSize.width(),
            slice.sourceRect.height() * desktop.height() / m_StreamSize.height());
        const QtSessionGeometry geometry{m_StreamSize, slice.sourceRect, slice.destinationRect,
            drawable, surface.size(), desktop, input};
        surface.setPresentation(geometry);
        m_Presenter->setGeometry(&surface, geometry);
    }
    updateFocus();
}

void QtSessionRuntime::setStatus(const QString& text)
{
    const auto visible = text.isEmpty() && PlankClock::milliseconds() < m_DisplayNoticeDeadline ? m_DisplayNotice : text;
    for (auto& shell : m_Windows) shell->surface().setStatusText(visible);
}

void QtSessionRuntime::beginRecovery()
{
    m_ReconnectNoticeDeadline = PlankClock::milliseconds() + quint64(m_Session->m_Preferences->plankUnreachableTimeoutSeconds) * 1000;
    if (m_Session->m_Preferences->plankUnreachableAction == StreamingPreferences::PLANK_UNREACHABLE_DISCONNECT)
        m_Session->m_ReconnectPolicy.allowUntil(m_ReconnectNoticeDeadline);
    else
        // The timeout reveals a status dialog; it does not suspend recovery.
        // The shared worker retains cancellation, bounded requests, retry delay,
        // terminal authentication errors and protection against silent takeover.
        m_Session->m_ReconnectPolicy.allowContinuously();
    for (auto& shell : m_Windows) shell->surface().setWaiting(false);
}

void QtSessionRuntime::setInputEnabled(bool enabled)
{
    enabled = enabled && m_Session->m_CollaborationPermission.load() != PlankCollaboration::View;
    for (auto& shell : m_Windows) shell->surface().setRemoteInputEnabled(enabled);
    m_Input->setEnabled(enabled);
    updateFocus();
}

void QtSessionRuntime::updateFocus()
{
#ifdef Q_OS_MACOS
    if (m_PointerWindow && !m_PointerWindow->ownsKeyboard()) m_Input->releaseAll();
#endif
    auto* focused = QGuiApplication::focusWindow();
    bool active = false;
    for (const auto& shell : m_Windows)
        active |= !shell->surface().displayMenuOpen() &&
                  (focused == shell.get() || focused == &shell->surface());
    m_Input->setFocused(active && !m_Session->m_Lifecycle.stopping());
#ifdef Q_OS_MACOS
    m_Session->m_QtClipboardFocused.store(active && m_Session->m_Lifecycle.streaming() && !m_Session->m_Lifecycle.stopping());
#endif
    const auto policy = m_CapturePolicy;
    const bool fullscreen = !m_Windows.empty() && m_Windows.front()->windowState() == Qt::WindowFullScreen;
    const bool captureAllowed = m_Session->m_Lifecycle.streaming() && m_InputGrabbed && !m_Session->m_Lifecycle.stopping() &&
        m_Session->m_CollaborationPermission.load() != PlankCollaboration::View &&
        (policy == StreamingPreferences::CSK_ALWAYS || (policy == StreamingPreferences::CSK_FULLSCREEN && fullscreen));
#ifdef Q_OS_MACOS
    // Keep policy separate from cached Qt focus. The native tap and Quit
    // guard recheck AppKit's key window/Space on the first returned key.
    m_Input->setSystemKeys(captureAllowed);
#else
    const bool grab = active && captureAllowed;
    m_Input->setSystemKeys(grab);
#endif
#ifdef Q_OS_LINUX
    QtPlatformSessionShell* focusedShell = nullptr;
    for (auto& shell : m_Windows)
        if (focused == shell.get() || focused == &shell->surface()) focusedShell = shell.get();
    m_WindowGuard->setWindows(focusedShell, focusedShell ? &focusedShell->surface() : nullptr);
    m_WindowGuard->setPolicy(grab, active && m_Session->m_Lifecycle.streaming() && m_InputGrabbed && m_ConfinePointer &&
        fullscreen && QGuiApplication::screens().size() == 1);
    // The Wayland shortcut inhibitor is attached separately to Qt's surface.
    // XCB implements this public Qt API directly.
    if (QGuiApplication::platformName() != QStringLiteral("wayland"))
        for (auto& shell : m_Windows) shell->setKeyboardGrabEnabled(grab && shell.get() == focusedShell);
#endif
}

void QtSessionRuntime::toggleFullscreen()
{
    if (m_Windows.empty()) return;
    m_Fullscreen = !m_Fullscreen;
    showPresentationWindows();
    updateGeometry();
    updateFocus();
}

void QtSessionRuntime::showPresentationWindows()
{
    QScopedValueRollback<bool> changing(m_ChangingPresentation, true);
    for (size_t i = 0; i < m_Windows.size(); ++i) {
        m_Windows[i]->surface().cancelInput();
        const bool selected = !m_Layout.planned || m_Views.contains(m_Windows[i].get());
        const bool show = selected && (!i || (m_Layout.planned ? !m_CollapsedDisplay :
            (m_Fullscreen && m_SpanDisplays && !m_CollapsedDisplay)));
        if (show) m_Windows[i]->showSession(m_Fullscreen);
        else m_Windows[i]->hide();
    }
}

void QtSessionRuntime::selectFullscreenDisplays(bool span, QtPlatformSessionShell* source)
{
    if (m_Session->m_Lifecycle.stopping() || m_Windows.empty() || !source || !source->screen()) return;
    if (m_Layout.planned) {
        m_CollapsedDisplay = !span;
        if (!span) {
            const auto selected = std::find_if(m_Windows.begin(), m_Windows.end(), [source](const auto& shell) { return shell.get() == source; });
            if (selected != m_Windows.end()) std::rotate(m_Windows.begin(), selected, std::next(selected));
            m_Presenter->setPrimaryOutput(&m_Windows.front()->surface());
        }
        m_Fullscreen = true;
        for (auto& shell : m_Windows)
            shell->surface().setDisplayMode(m_Layout.outputs.size() > 1, span);
        showPresentationWindows(); updateGeometry(); updateFocus();
        return;
    }
    const bool canSpan = m_Windows.size() == 2 && ClientDisplays::canSpan(ClientDisplays::snapshot());
    if (span && !canSpan) return;
    // Presentation-only override. Keep the authentication/Host-layout snapshot
    // immutable while the network worker uses it for launch and reconnect.
    auto* target = source->screen();
    {
        QScopedValueRollback<bool> changing(m_ChangingPresentation, true);
        m_Input->setFocused(false);
        for (auto& shell : m_Windows) { shell->surface().cancelInput(); shell->hide(); }
        m_Windows.front()->setScreen(target);
        m_Screens[0] = target;
        if (canSpan) {
            for (auto* other : QGuiApplication::screens()) if (other != target) {
                m_Windows[1]->setScreen(other);
                m_Screens[1] = other;
            }
        }
        m_SpanDisplays = span;
        m_Fullscreen = true;
        for (auto& shell : m_Windows) shell->surface().setDisplayMode(canSpan, span);
    }
    showPresentationWindows();
    updateGeometry();
    m_Windows.front()->requestActivate();
    updateFocus();
}

void QtSessionRuntime::minimize()
{
    for (auto& shell : m_Windows) shell->surface().cancelInput();
    if (!m_Windows.empty()) m_Windows.front()->showMinimized();
    for (size_t i = 1; i < m_Windows.size(); ++i) m_Windows[i]->hide();
}

void QtSessionRuntime::handleShortcut(int key)
{
    if (m_Windows.empty()) return;
    auto& surface = m_Windows.front()->surface();
    switch (key) {
    case Qt::Key_Q:
        m_Session->m_Diagnostics.end(SessionDiagnostics::End::Hotkey);
        stop(); break;
    case Qt::Key_Z: m_InputGrabbed = !m_InputGrabbed; setInputEnabled(m_Session->m_Lifecycle.streaming() && m_InputGrabbed); break;
    case Qt::Key_X: surface.toggleFullscreen(); break;
    case Qt::Key_T:
        for (auto& shell : m_Windows) if (shell->ownsKeyboard()) { shell->surface().toggleToolbar(); return; }
        surface.toggleToolbar(); break;
    case Qt::Key_S:
    {
        const bool show = !surface.statisticsVisible();
        for (auto& shell : m_Windows) shell->surface().setStatisticsVisible(show);
        break;
    }
    case Qt::Key_D: surface.minimize(); break;
    case Qt::Key_V: m_Input->pasteText(); break;
    case Qt::Key_K:
        m_CapturePolicy = m_CapturePolicy == StreamingPreferences::CSK_OFF ?
            StreamingPreferences::CSK_ALWAYS : StreamingPreferences::CSK_OFF;
        updateFocus(); break;
    case Qt::Key_L: m_ConfinePointer = !m_ConfinePointer; updateFocus(); break;
    case Qt::Key_M: toggleImmersiveMouse(); break;
    default: break;
    }
}

void QtSessionRuntime::toggleImmersiveMouse(QtPlatformSessionShell* source)
{
#ifdef Q_OS_MACOS
    if (!m_Session->m_Lifecycle.streaming() || m_Session->m_Lifecycle.stopping()) return;
    if (!source) {
        for (auto& shell : m_Windows) if (shell->ownsKeyboard()) { source = shell.get(); break; }
    }
    if (!source || !source->surface().immersiveMouseAvailable()) return;
    if (!m_Input->immersiveMouse()) m_PointerCandidateWindow = source;
    QPointF anchor = source->surface().presentation().inputPixels().center();
    const auto& geometry = source->surface().presentation();
    QPointF local = source->mapFromGlobal(QCursor::pos());
    geometry.toStream(local, anchor); // Toolbar activation keeps a bounded fallback.
    m_Input->setImmersiveMouse(!m_Input->immersiveMouse(), anchor);
#else
    Q_UNUSED(source);
#endif
}

void QtSessionRuntime::tick()
{
    if (m_Session->m_Lifecycle.stopping()) return;
    if (m_Session->m_GuiDisplaySnapshot.requested())
        m_Session->m_GuiDisplaySnapshot.publish(ClientDisplayProbe::probe());
#ifdef Q_OS_MACOS
    m_Session->pollClipboardOnMainThread();
#endif
    if (m_Session->m_ReconnectGreeterConfirmed.exchange(false)) setStatus(tr("Returning to the sign-in screen…"));
    if (m_ReconnectNoticeDeadline && PlankClock::milliseconds() >= m_ReconnectNoticeDeadline) {
        m_ReconnectNoticeDeadline = 0;
        if (m_Session->m_Preferences->plankUnreachableAction == StreamingPreferences::PLANK_UNREACHABLE_DISCONNECT) {
            m_Session->m_Diagnostics.end(SessionDiagnostics::End::ReconnectTimeout);
            stop();
            return;
        } else {
            qInfo("PLANK Qt waiting for workstation; automatic recovery remains active");
            setInputEnabled(false);
            for (auto& shell : m_Windows) shell->surface().setWaiting(true);
        }
    }
    if (m_Session->m_Lifecycle.streaming()) {
        // A healthy Host may send no new frames while its desktop is idle.
        // Only transport failure or a confirmed worker replacement enters
        // recovery; the lifecycle then leaves Streaming so its status
        // and timeout notice remain owned by that recovery path.
        setStatus(m_Presenter->hasPresentedLiveFrame() ? QString() : tr("Connecting to workstation…"));
    }
    const auto now = PlankClock::milliseconds();
    const auto requested = m_Session->m_ConfirmedBitrateRequestKbps.load();
    const auto applied = m_Session->m_ConfirmedBitrateAppliedKbps.load();
    if (requested == m_TargetBitrate && applied > 0 && applied != m_TargetBitrate) {
        m_TargetBitrate = applied; m_RecoveryBitrate.store(applied);
        for (auto& shell : m_Windows) shell->surface().setBitrateKbps(applied);
    }
    const auto flags = LI_FF_DYNAMIC_VIDEO_BITRATE | LI_FF_ENCODER_TARGET_ACK;
    const bool rateSupported = (LiGetHostFeatureFlags() & flags) == flags;
    if (m_Session->m_Lifecycle.streaming() && rateSupported && m_TargetBitrate != applied && now >= m_BitrateChanged + 250 &&
        now >= m_BitrateSent + (m_LastSentBitrate == m_TargetBitrate ? 1000 : 250)) {
        if (LiSetVideoBitrate(m_TargetBitrate) == 0) { m_LastSentBitrate = m_TargetBitrate; m_BitrateSent = now; }
    }
    QVariantMap telemetry{{QStringLiteral("fps"), m_Session->m_CurrentRenderedFps.load()},
        {QStringLiteral("videoMbps"), m_Session->m_CurrentVideoMbps.load()},
        {QStringLiteral("loss"), m_Session->currentVideoFecLoss().before},
        {QStringLiteral("rtt"), m_Session->currentNetworkRttMs()},
        {QStringLiteral("bitrateEnabled"), rateSupported && m_Session->m_Lifecycle.streaming()},
        {QStringLiteral("bitrateConfirmed"), requested == m_TargetBitrate && applied == m_TargetBitrate},
        {QStringLiteral("peakKbps"), m_Session->m_ConfirmedBitratePeakKbps.load()}};
    {
        std::unique_lock<std::mutex> lock(m_Session->m_MicrophoneMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            telemetry.insert(QStringLiteral("microphoneSupported"), m_Session->m_Microphone != nullptr);
            telemetry.insert(QStringLiteral("microphoneState"), int(m_Session->m_Microphone ? m_Session->m_Microphone->state() : PlankMicrophone::State::Off));
        }
    }
    {
        std::unique_lock<std::mutex> lock(m_Session->m_CameraMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            telemetry.insert(QStringLiteral("cameraSupported"), m_Session->m_Camera != nullptr);
            telemetry.insert(QStringLiteral("cameraState"), int(m_Session->m_Camera ? m_Session->m_Camera->state() : PlankCamera::State::Off));
        }
    }
    const bool guest = m_Session->isCollaborator();
    telemetry.insert(QStringLiteral("collaborationText"), guest ?
        tr("Sharing with %1 · %2").arg(m_Session->m_CollaborationOwner,
            m_Session->m_CollaborationPermission.load() == PlankCollaboration::View ?
            tr("View only") : tr("Keyboard and mouse control")) : QString());
#ifdef Q_OS_MACOS
    const auto files = m_Session->fileClipboard();
    telemetry.insert(QStringLiteral("fileTransferEnabled"), files && m_Session->clipboardSyncEnabled() &&
        (m_Session->m_FileClipboardMode == QStringLiteral("client-to-host") ||
         m_Session->m_FileClipboardMode == QStringLiteral("bidirectional")));
    telemetry.insert(QStringLiteral("fileTransferStatus"), files ? QString::fromStdString(files->status()) : QString());
#endif
    for (auto& shell : m_Windows) shell->surface().setTelemetry(telemetry);
}
