import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3
import DisplaySetup 1.0
// @fernweh-broker-begin
import RemoteBroker 1.0
// @fernweh-broker-end

// One applicable editor for hosts without per-screen arrangements. All
// changes stay local until the dialog's single Save action calls save().
ColumnLayout {
    id: page
    property string hostId: ""
    property var setup: ({})
    property var modes: []
    readonly property int layoutChoice: DisplaySetup.remoteMac ? (layout.currentIndex === 0 ? 0 : 2) : layout.currentIndex
    readonly property bool canSave: modes.length > 0 && (layoutChoice !== 0 || setup.canMatchClient === true)
    readonly property string summary: {
        if (layoutChoice === 0) return setup.matchClientSummary || setup.matchClientReason || qsTr("Match this computer's screens when connecting")
        if (layoutChoice === 1) return qsTr("Keep the workstation's current desktop and screen arrangement.")
        if (layoutChoice === 2) return qsTr("One remote desktop: %1").arg(mode1.currentText)
        return qsTr("Two remote desktops side by side: %1 + %2").arg(mode1.currentText).arg(mode2.currentText)
    }
    readonly property string scalingSummary: qsTr("The viewer fits the desktop to its window and keeps proportions. Different screen shapes may leave black borders.")

    function reload() {
        setup = ({})
// @fernweh-broker-begin
        if (hostId !== "") setup = RemoteBroker.displaySetup(hostId)
// @fernweh-broker-end
        modes = setup.virtualModes || []
        advanced.checked = false
        layout.currentIndex = DisplaySetup.remoteMac ? (setup.layoutChoice === 0 ? 0 : 1) : Math.max(0, setup.layoutChoice || 0)
        mode1.currentIndex = Math.max(0, modes.indexOf(setup.virtualMode1))
        mode2.currentIndex = Math.max(0, modes.indexOf(setup.virtualMode2))
        scaling.currentIndex = setup.scalingChoice === 0 ? 0 : 1
    }
    function save() {
        return canSave && DisplaySetup.useFixedLayout(hostId, layoutChoice,
                modes[mode1.currentIndex], modes[mode2.currentIndex], scaling.currentIndex)
    }

    spacing: theme.spaceMedium
    PlankTheme { id: theme }
    Label {
        Layout.fillWidth: true
        text: DisplaySetup.remoteMac ? qsTr("Remote Mac desktop") : qsTr("Display setup for an older workstation")
        color: theme.textPrimary
        font.weight: Font.DemiBold
    }
    Label {
        Layout.fillWidth: true
        text: DisplaySetup.remoteMac ? qsTr("This Mac host uses a fixed desktop mode. Choose the desktop to request when connecting.") : qsTr("This host supports fixed layouts. Update its Host software to use individual screen sizes and placement.")
        color: theme.textSecondary
        wrapMode: Text.Wrap
    }
    Label { text: qsTr("Remote desktop"); color: theme.textSecondary }
    PlankComboBox {
        id: layout
        Layout.fillWidth: true
        model: DisplaySetup.remoteMac ? [qsTr("Match this computer"), qsTr("Choose a fixed desktop size")] :
               [qsTr("Match this computer"), qsTr("Keep the workstation's existing screens"),
                qsTr("One virtual desktop"), qsTr("Two virtual desktops side by side")]
    }
    Label {
        Layout.fillWidth: true
        visible: page.layoutChoice === 0
        text: qsTr("Automatic: the desktop is chosen from this computer's screens on each connection. To choose a resolution yourself, select a fixed desktop above.")
        color: theme.textSecondary
        wrapMode: Text.Wrap
    }
    RowLayout {
        visible: page.layoutChoice >= 2
        Layout.fillWidth: true
        Label { text: qsTr("Desktop size"); color: theme.textSecondary }
        PlankComboBox {
            id: mode1
            Layout.fillWidth: true
            model: page.modes.map(function(mode) { return mode.replace("x", " × ") })
        }
        PlankComboBox {
            id: mode2
            visible: page.layoutChoice === 3
            Layout.fillWidth: true
            model: mode1.model
        }
    }
    PlankCheckBox {
        id: advanced
        visible: !DisplaySetup.remoteMac
        text: qsTr("Advanced stream sizing")
    }
    Label {
        visible: advanced.checked && !DisplaySetup.remoteMac
        text: qsTr("Encoded stream resolution")
        color: theme.textSecondary
    }
    PlankComboBox {
        id: scaling
        visible: advanced.checked && !DisplaySetup.remoteMac
        Layout.fillWidth: true
        model: [qsTr("Keep the remote desktop's pixel resolution"), qsTr("Resample the stream to this computer's resolution")]
    }
    Label {
        Layout.fillWidth: true
        visible: advanced.checked && !DisplaySetup.remoteMac
        text: qsTr("Resampling changes the video frame size, not the workstation desktop. It can reduce sharpness. The viewer still fits the image to its window.")
        color: theme.textSecondary
        wrapMode: Text.Wrap
    }
    Label {
        Layout.fillWidth: true
        visible: !page.canSave
        text: page.modes.length === 0 ? qsTr("No supported desktop sizes are available. Connect once to refresh this host's capabilities.") : (page.setup.matchClientReason || qsTr("This screen arrangement cannot be matched. Choose a fixed desktop size above."))
        color: theme.warning
        wrapMode: Text.Wrap
    }
}
