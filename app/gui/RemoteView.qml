import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

import RemoteBroker 1.0
import ComputerManager 1.0
import DisplaySetup 1.0
import Onboarding 1.0
import StreamingPreferences 1.0

// Remote (broker) mode: sign in once with username, password and
// authenticator code, pick an assigned workstation, stream through the
// broker's lease (bde-linux docs/plank-broker.md sections 10.1/10.2).
// A user with a Touch ID key on this Mac (section 13.4) signs in with it
// instead; without one, or when it does not work, password + code is asked.
Item {
    id: remoteView
    objectName: qsTr("Remote")

    PlankTheme {
        id: theme
    }

    property string errorText: ""
    readonly property bool hostStatusesStale: RemoteBroker.hostStatusError !== ""
    // Set after a Touch ID fallback or by "Use password instead".
    property bool usePassword: false
    Timer {
        interval: 5000
        repeat: true
        running: remoteView.visible && RemoteBroker.signedIn && !RemoteBroker.streamBenchmarkRunning
        onTriggered: RemoteBroker.refreshHostsQuietly()
    }
    readonly property bool passkeyReady: !usePassword &&
        RemoteBroker.passkeyUsers.indexOf(usernameField.text.trim().toLowerCase()) >= 0

    StackView.onActivated: {
        RemoteBroker.initialize(ComputerManager)
        RemoteBroker.refreshPasskeys()
        // Back from the first sign-in wizard without a session: continue
        // with the user name typed there.
        var prefill = Onboarding.takeSignInUsername()
        usernameField.prefill(prefill)
        if (RemoteBroker.signedIn) {
            if (!RemoteBroker.streamBenchmarkRunning) RemoteBroker.refreshHosts()
        } else if (prefill !== "") {
            if (remoteView.passkeyReady) {
                usernameField.forceActiveFocus()
            } else {
                passwordField.forceActiveFocus()
            }
        } else {
            if (usernameField.text !== "") passwordField.forceActiveFocus()
            else usernameField.forceActiveFocus()
        }
    }

    Connections {
        target: RemoteBroker
        function onErrorOccurred(message) {
            remoteView.errorText = message
        }
        function onPasskeyFallback(message) {
            remoteView.usePassword = true
            remoteView.errorText = message
            passwordField.forceActiveFocus()
        }
        function onConnectReady(hostName) {
            remoteView.errorText = ""
            var session = RemoteBroker.takeSession()
            if (session === null) {
                return
            }
            var component = Qt.createComponent("StreamSegue.qml")
            var segue = component.createObject(stackView, {
                                                   "appName": hostName,
                                                   "session": session,
                                                   "benchmark": RemoteBroker.streamBenchmarkRunning,
                                                   "isResume": false
                                               })
            stackView.push(segue)
        }
        function onStreamBenchmarkFinished(runs, message, canSave) {
            benchmarkResults.runs = runs
            benchmarkResults.message = message
            benchmarkResults.canSave = canSave
            benchmarkResults.open()
        }
        function onStreamUpgradeReviewRequired(hostId, hostName) {
            streamUpgradeReview.hostId = hostId
            streamUpgradeReview.hostName = hostName
            streamUpgradeReview.open()
        }
        function onDisplaySetupRequired(hostId, hostName, reason) {
            // Start with the applicable basic editor; Advanced is an explicit choice.
            screensDialog.openFor(hostId, hostName, reason, true, 0)
        }
        function onStreamSetupRequired(hostId, hostName, reason) {
            displaySetupDialog.openFor(hostId, hostName, true, "", reason)
        }
        function onTouchIdSetupRequested() {
            if (RemoteBroker.touchIdSetupPending) touchIdSetupDialog.open()
        }
        function onTouchIdSetupCompleted() {
            touchIdSetupDialog.close()
        }
        function onStateChanged() {
            if (!RemoteBroker.signedIn && touchIdSetupDialog.visible) touchIdSetupDialog.close()
            if (!RemoteBroker.signedIn) usernameField.prefill("")
        }
    }

    NavigableDialog {
        id: touchIdSetupDialog
        title: qsTr("Sign in with Touch ID?")
        width: Math.min(440, remoteView.width - 32)
        height: Math.min(implicitHeight, remoteView.height - 20)
        modal: true
        closePolicy: Popup.NoAutoClose
        standardButtons: Dialog.NoButton

        ColumnLayout {
            width: parent.width
            spacing: theme.spaceMedium
            Label {
                Layout.fillWidth: true
                text: qsTr("You are signed in. Add a sign-in key to this Mac so you can use Touch ID next time. Your authenticator app stays available as a fallback.")
                color: theme.textSecondary
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: RemoteBroker.touchIdSetupError !== ""
                text: RemoteBroker.touchIdSetupError
                color: theme.danger
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Button {
                    text: qsTr("Not now")
                    enabled: !RemoteBroker.touchIdSetupBusy
                    onClicked: {
                        RemoteBroker.skipTouchIdAfterSignIn()
                        touchIdSetupDialog.close()
                    }
                }
                Item { Layout.fillWidth: true }
                BusyIndicator {
                    visible: RemoteBroker.touchIdSetupBusy
                    running: visible
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                }
                Button {
                    text: qsTr("Set up Touch ID")
                    highlighted: true
                    enabled: !RemoteBroker.touchIdSetupBusy
                    onClicked: RemoteBroker.setUpTouchIdAfterSignIn()
                }
            }
        }
    }

    NavigableDialog {
        id: streamUpgradeReview
        property string hostId
        property string hostName
        title: qsTr("Review streaming settings")
        width: Math.min(460, remoteView.width - 40)
        modal: true
        closePolicy: Popup.CloseOnEscape
        onOpened: automaticVideoButton.forceActiveFocus()

        ColumnLayout {
            width: streamUpgradeReview.availableWidth
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Saved settings for %1 can be slow on some Windows PCs. Automatic video looks for hardware decoding. You can also test your displays or keep your settings.").arg(streamUpgradeReview.hostName)
            }
            Button {
                id: automaticVideoButton
                Layout.fillWidth: true
                text: qsTr("Use automatic video once")
                highlighted: true
                onClicked: {
                    streamUpgradeReview.close()
                    RemoteBroker.connectWithAutomaticVideo(streamUpgradeReview.hostId)
                }
            }
            Button {
                Layout.fillWidth: true
                text: qsTr("Run benchmark")
                onClicked: {
                    RemoteBroker.finishStreamUpgradeReview(streamUpgradeReview.hostId)
                    benchmarkStart.hostId = streamUpgradeReview.hostId
                    streamUpgradeReview.close()
                    benchmarkStart.open()
                }
            }
            Button {
                Layout.fillWidth: true
                text: qsTr("Keep my settings")
                onClicked: {
                    RemoteBroker.finishStreamUpgradeReview(streamUpgradeReview.hostId)
                    streamUpgradeReview.close()
                    RemoteBroker.connectToHost(streamUpgradeReview.hostId)
                }
            }
        }
    }

    NavigableDialog {
        id: benchmarkStart
        property string hostId
        title: qsTr("Test connection and displays")
        width: Math.min(500, remoteView.width - 40)
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: RemoteBroker.startStreamBenchmark(hostId)
        Label {
            width: parent.width
            text: qsTr("Start continuous motion on the signed-in workstation before testing. Fernweh will preview its supported hardware codecs one at a time using your selected displays. End the stream to stop the test.")
            wrapMode: Text.Wrap
        }
    }

    NavigableDialog {
        id: benchmarkResults
        property var runs: []
        property string message
        property bool canSave: false
        title: qsTr("Connection test results")
        width: Math.min(630, remoteView.width - 40)
        height: Math.min(implicitHeight, remoteView.height - 20)
        modal: true
        standardButtons: canSave ? Dialog.Ok | Dialog.Close : Dialog.Close
        onOpened: {
            if (canSave) standardButton(Dialog.Ok).text = qsTr("Use recommended settings")
        }
        onAccepted: {
            if (canSave && !RemoteBroker.acceptStreamBenchmarkRecommendation())
                remoteView.errorText = qsTr("Fernweh could not save this recommendation. Your stream settings are still available.")
        }

        ScrollView {
            anchors.fill: parent
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: parent.width
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    text: benchmarkResults.message
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: benchmarkResults.runs
                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        Label {
                            Layout.fillWidth: true
                            font.bold: true
                            text: modelData.recommended ? qsTr("%1 (recommended)").arg(modelData.profile) : modelData.profile
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: qsTr("%1 × %2 at %3 fps · %4 · %5 · %6 Mbps target")
                                  .arg(modelData.width).arg(modelData.height).arg(modelData.target)
                                  .arg(modelData.route)
                                  .arg(!modelData.decodeObserved ? qsTr("Decode unavailable") :
                                       modelData.hardware ? qsTr("Hardware decode") : qsTr("Software or mixed decode"))
                                  .arg((modelData.kbps / 1000).toFixed(1))
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: qsTr("%1 received, %2 decoded, %3 presented fps · %4 video Mbps · %5 missing frames")
                                  .arg(modelData.received.toFixed(1)).arg(modelData.decoded.toFixed(1))
                                  .arg(modelData.presented.toFixed(1)).arg(modelData.mbps.toFixed(1))
                                  .arg(modelData.missing)
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            visible: modelData.timedOut
                            text: qsTr("The preview timed out before it finished measuring.")
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            visible: modelData.lowestPresented >= 0 && modelData.lowestPresented < modelData.presentationFloor
                            text: qsTr("Lowest presentation sample %1 fps · %2 fps required")
                                  .arg(modelData.lowestPresented.toFixed(1)).arg(modelData.presentationFloor.toFixed(1))
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: modelData.exercised ?
                                      qsTr("%1 fps from fresh desktop captures · %2 of %3 s with motion")
                                          .arg(modelData.freshCaptureFps.toFixed(1))
                                          .arg(modelData.motionSeconds.toFixed(1)).arg(modelData.seconds.toFixed(1)) :
                                      qsTr("Too little sustained motion or traffic · %1 of %2 s with motion")
                                          .arg(modelData.motionSeconds.toFixed(1)).arg(modelData.seconds.toFixed(1))
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: qsTr("Mean decode %1 · render %2 · RTT %3")
                                  .arg(modelData.decodeMs < 0 ? qsTr("unavailable") : qsTr("%1 ms").arg(modelData.decodeMs.toFixed(1)))
                                  .arg(modelData.renderMs < 0 ? qsTr("unavailable") : qsTr("%1 ms").arg(modelData.renderMs.toFixed(1)))
                                  .arg(modelData.rtt < 0 ? qsTr("unavailable") : qsTr("%1 ms").arg(modelData.rtt.toFixed(0)))
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            visible: modelData.fecBefore >= 0
                            text: qsTr("Video symbol loss %1% before correction · %2% after correction")
                                  .arg(modelData.fecBefore.toFixed(2)).arg(modelData.fecAfter.toFixed(2))
                        }
                    }
                }
            }
        }
    }

    // Per-workstation settings (display setup and stream quality), asked on
    // first connect and editable from each row; kept locally
    // (RemoteBroker.displaySetup / streamSetup and their save functions).
    NavigableDialog {
        id: displaySetupDialog
        property string hostId: ""
        property string hostName: ""
        property bool connectAfter: false
        property string reason: ""
        // DisplaySetup.hostSummary: mode (follow, custom, legacy), summary.
        property var screens: ({})
        property string streamReason: ""
        property var stream: ({})
        title: qsTr("Settings for %1").arg(hostName)
        width: Math.min(560, remoteView.width - 40)
        height: Math.min(implicitHeight, remoteView.height - 20)
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok | Dialog.Cancel

        function openFor(hostId, hostName, connectAfter, reason, streamReason) {
            displaySetupDialog.streamReason = streamReason || ""
            displaySetupDialog.hostId = hostId
            displaySetupDialog.hostName = hostName
            displaySetupDialog.connectAfter = connectAfter
            displaySetupDialog.reason = reason
            displaySetupDialog.loadScreens()
            displaySetupDialog.loadStream()
            displaySetupDialog.open()
        }

        function loadScreens() {
            screens = DisplaySetup.hostSummary(hostId)
            screensChoice.load(screens.mode)
        }

        function loadStream() {
            stream = RemoteBroker.streamSetup(hostId)
            streamVideoSettings.hostPlatform = stream.platform || 0
            // A choice the workstation refused is shown as the user's own
            // choice, so it can be changed rather than hidden behind defaults.
            streamUseDefaults.checked = stream.useDefaults === true && streamReason === ""
            streamVideoSettings.load(stream.captureSource, stream.videoProfile,
                                     stream.officeBitratesKbps, stream.internetBitratesKbps)
        }

        function streamProblem(capture, profile) {
            return RemoteBroker.streamProfileProblem(hostId, capture, profile)
        }

        function saveStream() {
            return RemoteBroker.saveStreamSetup(hostId, streamUseDefaults.checked,
                                                streamVideoSettings.captureSource,
                                                streamVideoSettings.videoProfile,
                                                streamVideoSettings.officeBitratesKbps,
                                                streamVideoSettings.internetBitratesKbps)
        }

        onOpened: {
            // Right after the workstation refused a choice (streamReason) its
            // capabilities were just probed, so an unusable choice cannot be
            // saved. Otherwise they come from an earlier connect and may be
            // stale (host upgraded, encoder fixed): the choice is only
            // flagged, and the connect checks it against the live host.
            var saveButton = standardButton(Dialog.Ok)
            saveButton.text = connectAfter ? qsTr("Save and connect") : qsTr("Save")
            saveButton.enabled = Qt.binding(function() {
                return streamReason === "" || streamVideoSettings.problemText === ""
            })
        }

        onAccepted: {
            if (!saveStream()) {
                remoteView.errorText = qsTr("Those stream settings are not supported.")
                return
            }
            var chosenMode = screensChoice.selectedMode
            if (screens.mode !== chosenMode)
                DisplaySetup.setHostMode(hostId, chosenMode)
            if (connectAfter) {
                remoteView.errorText = ""
                RemoteBroker.connectToHost(hostId)
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            // ------------------------------------------------------- screens
            Label {
                text: qsTr("Screens")
                font.bold: true
                font.pointSize: 13
            }
            Label {
                Layout.fillWidth: true
                text: displaySetupDialog.screens.summary || ""
                wrapMode: Text.Wrap
                opacity: 0.72
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: theme.spaceMedium

                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true

                    WorkstationDisplayModeChoice {
                        id: screensChoice
                        Layout.fillWidth: true
                    }

                }
                Button {
                    text: qsTr("Screens…")
                    Layout.alignment: Qt.AlignTop
                    onClicked: {
                        var hostId = displaySetupDialog.hostId
                        var hostName = displaySetupDialog.hostName
                        displaySetupDialog.close()
                        screensDialog.openForMode(hostId, hostName, "", false, 0, screensChoice.selectedMode)
                    }
                }
            }

            // ------------------------------------------------ stream quality
            Label {
                Layout.topMargin: 8
                text: qsTr("Stream quality")
                font.bold: true
                font.pointSize: 13
            }
            Button {
                visible: displaySetupDialog.stream.hasBenchmarkChoice === true
                text: qsTr("Clear saved benchmark recommendation")
                onClicked: {
                    RemoteBroker.clearStreamBenchmarkRecommendation(displaySetupDialog.hostId)
                    displaySetupDialog.loadStream()
                }
            }
            Label {
                Layout.fillWidth: true
                visible: displaySetupDialog.streamReason !== ""
                text: displaySetupDialog.streamReason
                color: theme.warning
                wrapMode: Text.Wrap
            }
            PlankCheckBox {
                id: streamUseDefaults
                Layout.fillWidth: true
                text: qsTr("Use the remote access defaults (Settings › Remote Access)")
                onToggled: {
                    if (checked) {
                        var defaults = displaySetupDialog.stream.defaults
                        streamVideoSettings.load(defaults.captureSource, defaults.videoProfile,
                                                 defaults.officeBitratesKbps, defaults.internetBitratesKbps)
                    }
                }
            }
            PlankVideoSettings {
                id: streamVideoSettings
                Layout.fillWidth: true
                enabled: !streamUseDefaults.checked
                showRoutes: true
                profileProblem: displaySetupDialog.streamProblem
            }
            Label {
                Layout.fillWidth: true
                visible: displaySetupDialog.streamReason === "" && streamVideoSettings.problemText !== ""
                text: qsTr("This is what the workstation reported on the last connect. Connecting checks it again.")
                wrapMode: Text.Wrap
                opacity: 0.72
            }
        }
    }

    // One status line per workstation. The broker's reason codes are
    // short English tokens ("offline", "no address", "certificate changed");
    // say what they mean, and never repeat the state ("Offline · offline").
    function hostStatusText(host) {
        var parts = []
        parts.push(!host.online ? qsTr("Offline in Fernweh") : host.sharedDesktop ? qsTr("Shared desktop") :
                   (host.busy || host.inUseBy !== "" || (host.connectedUsers || []).length > 0) ? qsTr("Busy") : qsTr("Online"))
        if (host.inUseBy !== "" && !host.sharedDesktop) {
            parts.push(qsTr("in use by %1").arg(host.inUseBy))
        }
        if (!host.connectable && host.reason !== "") {
            switch (host.reason) {
            case "offline":
                break
            case "no address":
                parts.push(qsTr("no network address; ask the studio to check it"))
                break
            case "certificate changed":
                parts.push(qsTr("its identity changed; ask the studio to check it"))
                break
            default:
                parts.push(host.reason)
            }
        }
        return parts.join(" · ")
    }

    function submitSignIn() {
        if (!signInButton.enabled) {
            return
        }
        remoteView.errorText = ""
        if (remoteView.passkeyReady) {
            RemoteBroker.signInWithPasskey(usernameField.text)
            return
        }
        RemoteBroker.signIn(usernameField.text, passwordField.text, otpField.text)
        passwordField.clear()
        otpField.clear()
    }

    // ---------------------------------------------------------------- sign in
    Rectangle {
        id: signInPanel
        visible: !RemoteBroker.signedIn
        anchors.centerIn: parent
        width: Math.min(440, parent.width - 32)
        height: signInColumn.implicitHeight + 48
        color: theme.surface
        radius: theme.radiusLarge
        border.width: 1
        border.color: theme.borderSubtle

        ColumnLayout {
            id: signInColumn
            anchors.fill: parent
            anchors.margins: 24
            spacing: 8

            Label {
                text: qsTr("Remote access")
                color: theme.textPrimary
                font.pointSize: 15
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            Label {
                text: remoteView.passkeyReady ?
                          qsTr("Sign in with Touch ID. This Mac holds your sign-in key.") :
                          qsTr("Sign in with your studio account and the code from your authenticator app.")
                color: theme.textSecondary
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Label {
                text: RemoteBroker.brokerAddress
                color: theme.textDisabled
                font.pointSize: 9
                Layout.fillWidth: true
                Layout.bottomMargin: 4
            }

            Label {
                text: qsTr("Username")
                color: theme.textSecondary
                Layout.fillWidth: true
            }
            BrokerUsernameField {
                id: usernameField
                rememberedUsername: RemoteBroker.rememberedUsername
                Layout.fillWidth: true
                Accessible.name: qsTr("Username")
                enabled: !RemoteBroker.busy
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                onTextChanged: remoteView.usePassword = false
                Keys.onReturnPressed: remoteView.passkeyReady ? remoteView.submitSignIn() : passwordField.forceActiveFocus()
                Keys.onEnterPressed: remoteView.passkeyReady ? remoteView.submitSignIn() : passwordField.forceActiveFocus()
            }

            Label {
                visible: !remoteView.passkeyReady
                text: qsTr("Password")
                color: theme.textSecondary
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            PlankTextField {
                id: passwordField
                visible: !remoteView.passkeyReady
                Layout.fillWidth: true
                Accessible.name: qsTr("Password")
                enabled: !RemoteBroker.busy
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                Keys.onReturnPressed: otpField.forceActiveFocus()
                Keys.onEnterPressed: otpField.forceActiveFocus()
            }

            Label {
                visible: !remoteView.passkeyReady
                text: qsTr("Authenticator code")
                color: theme.textSecondary
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            PlankTextField {
                id: otpField
                visible: !remoteView.passkeyReady
                Layout.fillWidth: true
                Accessible.name: qsTr("Authenticator code")
                enabled: !RemoteBroker.busy
                placeholderText: qsTr("6 digits")
                maximumLength: 6
                inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                validator: RegularExpressionValidator { regularExpression: /[0-9]{0,6}/ }
                font.letterSpacing: 2
                Keys.onReturnPressed: remoteView.submitSignIn()
                Keys.onEnterPressed: remoteView.submitSignIn()
            }

            Label {
                visible: !RemoteBroker.configured
                text: qsTr("Remote access is not configured. Add the broker key (SPKI SHA-256 pin) in Settings.")
                color: theme.warning
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.topMargin: 4
            }

            Label {
                visible: remoteView.errorText !== "" && !RemoteBroker.signedIn &&
                         remoteView.errorText !== RemoteBroker.sessionNotice
                text: remoteView.errorText
                color: theme.danger
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.topMargin: 4
            }

            // Progress gets its own line: beside two buttons it had no room
            // left and elided to a couple of letters.
            RowLayout {
                visible: RemoteBroker.busy
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: theme.spaceSmall

                BusyIndicator {
                    running: parent.visible
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                }
                Label {
                    text: RemoteBroker.busyText
                    color: theme.textSecondary
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                spacing: theme.spaceMedium

                Item {
                    Layout.fillWidth: true
                }
                Button {
                    visible: remoteView.passkeyReady
                    text: qsTr("Use password instead")
                    flat: true
                    enabled: !RemoteBroker.busy
                    onClicked: {
                        remoteView.usePassword = true
                        passwordField.forceActiveFocus()
                    }
                }
                Button {
                    id: signInButton
                    text: remoteView.passkeyReady ? qsTr("Sign in with Touch ID") : qsTr("Sign in")
                    highlighted: true
                    enabled: !RemoteBroker.busy && RemoteBroker.configured &&
                             usernameField.text.length > 0 &&
                             (remoteView.passkeyReady ||
                              (passwordField.text.length > 0 && otpField.text.length === 6))
                    onClicked: remoteView.submitSignIn()
                }
            }

            // First sign-in (one-time password from the studio): the wizard.
            Button {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 4
                visible: RemoteBroker.configured
                text: qsTr("New to BDE fernweh? Set up your account")
                flat: true
                enabled: !RemoteBroker.busy
                onClicked: {
                    remoteView.errorText = ""
                    stackView.push("qrc:/gui/OnboardingView.qml")
                }
            }
        }
    }

    // ------------------------------------------------------------- host list
    ColumnLayout {
        visible: RemoteBroker.signedIn
        anchors.fill: parent
        anchors.margins: 24
        spacing: theme.spaceMedium

        RowLayout {
            Layout.fillWidth: true
            spacing: theme.spaceMedium

            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true
                Label {
                    text: qsTr("Signed in as %1").arg(RemoteBroker.username)
                    color: theme.textPrimary
                    font.pointSize: 13
                    font.weight: Font.DemiBold
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    text: RemoteBroker.brokerAddress
                    color: theme.textDisabled
                    font.pointSize: 9
                    Layout.fillWidth: true
                }
            }
            BusyIndicator {
                visible: RemoteBroker.busy
                running: visible
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
            }
            Label {
                visible: RemoteBroker.busy
                text: RemoteBroker.busyText
                color: theme.textSecondary
            }
            Button {
                text: qsTr("Refresh")
                enabled: !RemoteBroker.busy
                onClicked: {
                    remoteView.errorText = ""
                    RemoteBroker.refreshHosts()
                }
            }
            Button {
                text: qsTr("Sign out")
                onClicked: {
                    remoteView.errorText = ""
                    RemoteBroker.logout()
                }
            }
        }

        Label {
            visible: remoteView.errorText !== ""
            text: remoteView.errorText
            color: theme.danger
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        Label {
            visible: remoteView.hostStatusesStale
            text: RemoteBroker.hostStatusError
            textFormat: Text.PlainText
            color: theme.warning
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        Rectangle {
            visible: hostList.count === 0 && !RemoteBroker.busy && !remoteView.hostStatusesStale
            Layout.fillWidth: true
            implicitHeight: emptyColumn.implicitHeight + 40
            radius: theme.radiusMedium
            color: theme.surface
            border.width: 1
            border.color: theme.borderSubtle

            ColumnLayout {
                id: emptyColumn
                anchors.fill: parent
                anchors.margins: 20
                spacing: 4

                Label {
                    text: qsTr("No workstations yet")
                    color: theme.textPrimary
                    font.pointSize: 12
                    font.weight: Font.Medium
                    Layout.fillWidth: true
                }
                Label {
                    text: qsTr("No workstation is assigned to you for remote access. Ask the studio to assign yours, then choose Refresh.")
                    color: theme.textSecondary
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
        }

        ListView {
            id: hostList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: RemoteBroker.hosts

            delegate: RemoteWorkstationCard {
                width: hostList.width
                host: modelData
                statusText: remoteView.hostStatusText(modelData)
                statusCurrent: !remoteView.hostStatusesStale
                busy: RemoteBroker.busy || RemoteBroker.streamBenchmarkRunning
                onSettingsRequested: displaySetupDialog.openFor(modelData.id, modelData.name, false, "")
                onBenchmarkRequested: {
                    benchmarkStart.hostId = modelData.id
                    benchmarkStart.open()
                }
                onConnectRequested: {
                    remoteView.errorText = ""
                    RemoteBroker.connectToHost(modelData.id)
                }
            }
        }
    }
}
