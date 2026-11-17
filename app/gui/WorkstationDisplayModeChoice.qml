import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

// Represent the saved mode explicitly. A video-only Save must not convert a
// fixed layout into a global profile simply because no radio represented it.
ColumnLayout {
    id: choice
    property string savedMode: "follow"
    readonly property string selectedMode: fixedMode.checked ? "legacy" : customMode.checked ? "custom" : "follow"
    spacing: 0

    function load(mode) {
        savedMode = mode || "follow"
        if (savedMode === "legacy") fixedMode.checked = true
        else if (savedMode === "custom") customMode.checked = true
        else followMode.checked = true
    }

    ButtonGroup { id: group }
    RadioButton {
        id: followMode
        objectName: "followDisplayMode"
        ButtonGroup.group: group
        checked: true
        text: qsTr("Use my display setup")
    }
    RadioButton {
        id: customMode
        objectName: "customDisplayMode"
        ButtonGroup.group: group
        text: qsTr("Custom for this workstation")
    }
    RadioButton {
        id: fixedMode
        objectName: "savedFixedDisplayMode"
        ButtonGroup.group: group
        visible: choice.savedMode === "legacy"
        text: qsTr("Keep the saved fixed layout")
    }
}
