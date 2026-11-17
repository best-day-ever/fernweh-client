import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

import DisplaySetup 1.0
// @fernweh-broker-begin
import RemoteBroker 1.0
// @fernweh-broker-end

// The display setup wizard: which of this computer's screens the workstation
// uses, at what size, and where; with the manual setup one click away. Opened
// on the first connect with screens that have no setup yet ("new-screens"),
// from Settings > Displays, from a workstation's Settings… ("Screens…"), and
// from the "screens changed" banner. Saving stores the layout for this set
// of monitors, for every workstation or for one; connectAfter then connects.
NavigableDialog {
    id: dialog

    property string hostId: ""
    property string hostName: ""
    property string reason: ""
    property bool connectAfter: false
    // 0: the overview, 1: the manual setup.
    property int page: 0
    property bool saved: false
    property string saveError: ""
    readonly property bool compatibility: dialog.hostId !== "" && DisplaySetup.fixedLayoutRequired
    // A running stream that asked for the setup (screens changed): saving
    // applies the layout to it, and the launcher hides again either way.
    property var session: null

    title: hostName !== "" ? qsTr("Screens for %1").arg(hostName) : qsTr("Display setup")
    width: Math.min(860, parent.width - 40)
    // As tall as the page shown needs (one screen leaves little to show),
    // never taller than the window; the pages scroll beyond that.
    height: Math.min(parent.height - 40, 860, Math.max(420, implicitHeight))
    modal: true
    dim: true
    closePolicy: Popup.CloseOnEscape

    function openFor(hostId, hostName, reason, connectAfter, page) {
        dialog.hostId = hostId || ""
        dialog.hostName = hostName || ""
        dialog.reason = reason || ""
        dialog.connectAfter = connectAfter === true
        dialog.page = page || 0
        dialog.saved = false
        dialog.saveError = ""
        advancedPage.pending = false
        advancedPage.error = ""
        DisplaySetup.begin(dialog.hostId, dialog.hostName, dialog.reason)
        compatibilityPage.reload()
        dialog.open()
    }

    function openForSession(streamSession) {
        dialog.session = streamSession
        var streamingHostId = ""
// @fernweh-broker-begin
        streamingHostId = RemoteBroker.streamingHostId()
// @fernweh-broker-end
        openFor(streamingHostId, "", "new-screens", false, 0)
    }

    function returnToStream(apply) {
        var streamSession = dialog.session
        dialog.session = null
        if (streamSession !== null) {
            if (apply) {
                streamSession.applyDisplayLayout()
            }
            window.visible = false
        }
    }

    function save() {
        dialog.saveError = ""
        if (dialog.compatibility) {
            if (!compatibilityPage.save()) {
                dialog.saveError = qsTr("The display setup could not be saved. Check the desktop size and try again.")
                return
            }
        } else {
            if (dialog.page === 1 && !advancedPage.commit()) return
            if (!DisplaySetup.accept()) {
                dialog.saveError = qsTr("The display setup could not be saved. Check the preview and try again.")
                return
            }
        }
        dialog.saved = true
        returnToStream(true)
        dialog.close()
        if (dialog.connectAfter && dialog.hostId !== "") {
// @fernweh-broker-begin
            RemoteBroker.connectToHost(dialog.hostId)
// @fernweh-broker-end
        }
    }

    // Escape, or any other way out, while a stream waits: back to it.
    onClosed: {
        if (!dialog.saved) DisplaySetup.cancel()
        returnToStream(false)
    }

    Connections {
        target: DisplaySetup
        function onActionRequested(action, key) {
            if (!dialog.visible) {
                return
            }
            if (action === "choose-screens") {
                dialog.page = 1
            } else if (action === "update-host") {
                updateHint.visible = true
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: theme.spaceMedium

        PlankTheme {
            id: theme
        }

        Label {
            Layout.fillWidth: true
            visible: text !== ""
            text: {
                if (dialog.reason === "new-screens") {
                    return qsTr("These screens (%1) are new to BDE fernweh. Check how the workstation will show them; you can change it later in Settings.").arg(DisplaySetup.label)
                }
                if (dialog.reason === "onboarding" || dialog.reason === "") {
                    return ""
                }
                return dialog.reason
            }
            color: dialog.reason === "new-screens" ? theme.textSecondary : theme.warning
            wrapMode: Text.Wrap
        }

        Label {
            id: updateHint
            Layout.fillWidth: true
            visible: false
            text: qsTr("Ask your studio administrator to update BDE fernweh on this workstation. Until then it uses the older layouts.")
            color: theme.textSecondary
            wrapMode: Text.Wrap
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: dialog.compatibility ? compatibilityPage.implicitHeight : dialog.page === 0 ? overviewPanel.implicitHeight : advancedPage.implicitHeight
            currentIndex: dialog.compatibility ? 2 : dialog.page

            ScrollView {
                id: overviewScroll
                clip: true
                contentWidth: availableWidth

                DisplaySetupPanel {
                    id: overviewPanel
                    width: overviewScroll.availableWidth
                    showSummary: false
                    allowAdvanced: true
                    onAdvancedRequested: dialog.page = 1
                }
            }

            ScrollView {
                id: advancedScroll
                clip: true
                contentWidth: availableWidth

                DisplayAdvancedPage {
                    id: advancedPage
                    width: advancedScroll.availableWidth
                    hostId: dialog.hostId
                    hostName: dialog.hostName

                }
            }
            ScrollView {
                id: compatibilityScroll
                clip: true
                contentWidth: availableWidth
                DisplayCompatibilityPage {
                    id: compatibilityPage
                    width: compatibilityScroll.availableWidth
                    hostId: dialog.hostId
                }
            }
        }
    }

    footer: Pane {
        padding: 12
        // Transparent, so the dialog's rounded corners and border show.
        background: Item {}

        PlankTheme {
            id: footerTheme
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: footerTheme.spaceSmall

            Label {
                Layout.fillWidth: true
                text: dialog.session !== null ? qsTr("Apply to the current connection") : qsTr("On next connection")
                font.weight: Font.DemiBold
                color: footerTheme.textPrimary
            }
            Label {
                Layout.fillWidth: true
                text: dialog.compatibility ? compatibilityPage.summary : DisplaySetup.summary
                color: DisplaySetup.planOk || dialog.compatibility ? footerTheme.textPrimary : footerTheme.warning
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: dialog.compatibility ? compatibilityPage.scalingSummary :
                      qsTr("Fit the desktop to your screen; keep proportions. Different screen shapes may leave black borders.") +
                      (DisplaySetup.effectivePresentation === "windows" ? qsTr(" One viewer per selected screen.") : qsTr(" One viewer window."))
                color: footerTheme.textSecondary
                font.pointSize: 10
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: !dialog.compatibility && DisplaySetup.backingExpected
                text: qsTr("Expected layout. The workstation confirms its supported sizes when you connect.")
                color: footerTheme.textSecondary
                font.pointSize: 10
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: advancedPage.pending && !dialog.compatibility && dialog.page === 1
                text: qsTr("Preview has pending dimension edits. Update preview to check the final layout before saving.")
                color: footerTheme.warning
                font.pointSize: 10
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: dialog.saveError !== ""
                text: dialog.saveError
                color: footerTheme.warning
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: footerTheme.spaceMedium

                Button {
                    visible: !dialog.compatibility
                    text: dialog.page === 0 ? qsTr("Advanced…") : qsTr("Basic")
                    flat: true
                    onClicked: {
                        if (dialog.page === 1 && !advancedPage.commit()) return
                        dialog.page = dialog.page === 0 ? 1 : 0
                    }
                }

                PlankComboBox {
                    visible: dialog.hostId !== "" && !dialog.compatibility
                    Layout.preferredWidth: 240
                    Accessible.name: qsTr("Save display setup for")
                    model: [qsTr("Use for every workstation"), qsTr("Only for %1").arg(dialog.hostName)]
                    currentIndex: DisplaySetup.scope === "workstation" ? 1 : 0
                    onActivated: function(index) {
                        if (dialog.page === 1 && !advancedPage.commit()) return
                        DisplaySetup.scope = index === 1 ? "workstation" : "global"
                    }
                }

                Item {
                    Layout.fillWidth: true
                }

                Button {
                    text: qsTr("Cancel")
                    flat: true
                    onClicked: {
                        dialog.returnToStream(false)
                        dialog.close()
                    }
                }
                Button {
                    text: dialog.connectAfter ? qsTr("Save and connect") : dialog.session !== null ? qsTr("Apply") : qsTr("Save")
                    highlighted: true
                    enabled: dialog.compatibility ? compatibilityPage.canSave : DisplaySetup.planOk
                    onClicked: dialog.save()
                }
            }
        }
    }
}
