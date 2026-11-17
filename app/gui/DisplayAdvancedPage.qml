import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

import DisplaySetup 1.0


// Manual display setup for advanced users: exact sizes and positions (drag
// with edge snapping), which workstation output backs each display, how the
// stream is presented. Hosts without arrangements use DisplayCompatibilityPage.
ColumnLayout {
    id: page

    property string hostId: ""
    property string hostName: ""
    // Capture every field before replanning replaces the delegates. Save uses
    // this too, so a focused editor never needs a separate Apply button.
    property bool pending: false
    property string error: ""

    function commit() {
        var edits = []
        for (var i = 0; i < geometryRows.count; ++i) {
            var row = geometryRows.itemAt(i)
            if (row) edits.push(row.edit())
        }
        if (!DisplaySetup.applyGeometry(edits)) {
            error = qsTr("Enter supported, even pixel dimensions and positions. Your changes have not been saved.")
            return false
        }
        pending = false
        error = ""
        return true
    }

    spacing: theme.spaceMedium

    PlankTheme {
        id: theme
    }

    function backingIndex(preference) {
        return preference === "physical" ? 1 : preference === "virtual" ? 2 : 0
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("Exact desktop dimensions and placement")
        color: theme.textPrimary
        font.weight: Font.DemiBold
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("Dimensions below change the remote desktop. Use even pixel values. Save applies all fields, including the one you are editing.")
        color: theme.textSecondary
        wrapMode: Text.Wrap
    }

    // ------------------------------------------------------ hand placement
    PlankCheckBox {
        Layout.fillWidth: true
        objectName: "manualDisplayPlacement"
        text: qsTr("Place the displays by hand (otherwise they are arranged like your desk)")
        checked: DisplaySetup.manual
        onToggled: {
            if (page.commit()) DisplaySetup.setManual(checked)
            else checked = Qt.binding(function() { return DisplaySetup.manual })
        }
    }

    Rectangle {
        id: canvas
        Layout.fillWidth: true
        Layout.preferredHeight: DisplaySetup.monitorCount > 1 ? 200 : 160
        color: theme.canvas
        radius: theme.radiusMedium
        border.width: 1
        border.color: theme.borderSubtle

        readonly property var desk: DisplaySetup.desktop
        // Room around the desktop so a display can be dragged past its edges.
        readonly property real scale: Math.min((width - 80) / Math.max(1, desk.width),
                                               (height - 60) / Math.max(1, desk.height))
        readonly property real originX: (width - desk.width * scale) / 2
        readonly property real originY: (height - desk.height * scale) / 2

        Label {
            x: 10
            y: 6
            text: qsTr("Drag to move; edges snap together")
            color: theme.textDisabled
            font.pointSize: 9
        }

        Repeater {
            id: tiles
            model: DisplaySetup.monitors

            delegate: Rectangle {
                id: tile
                visible: modelData.included
                x: canvas.originX + modelData.x * canvas.scale
                y: canvas.originY + modelData.y * canvas.scale
                width: modelData.width * canvas.scale
                height: modelData.height * canvas.scale
                radius: theme.radiusSmall
                color: dragArea.drag.active ? theme.surfacePressed : theme.surfaceRaised
                border.width: modelData.primary ? 2 : 1
                border.color: modelData.primary ? theme.accent : theme.border

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 6
                    text: (modelData.primary ? "★ " : "") + modelData.name + "\n" + modelData.sizeText
                    color: theme.textPrimary
                    font.pointSize: 8
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                }

                MouseArea {
                    id: dragArea
                    anchors.fill: parent
                    drag.target: tile
                    cursorShape: Qt.SizeAllCursor
                    onReleased: {
                        // Back to workstation pixels, then snap each edge to a
                        // neighbour's edge within about 12 screen pixels.
                        var x = Math.round((tile.x - canvas.originX) / canvas.scale)
                        var y = Math.round((tile.y - canvas.originY) / canvas.scale)
                        var w = modelData.width
                        var h = modelData.height
                        var snap = 12 / canvas.scale
                        var others = DisplaySetup.monitors
                        for (var i = 0; i < others.length; ++i) {
                            var o = others[i]
                            if (!o.included || o.key === modelData.key) continue
                            var candidatesX = [o.x + o.width, o.x - w, o.x, o.x + o.width - w]
                            for (var a = 0; a < candidatesX.length; ++a) {
                                if (Math.abs(x - candidatesX[a]) < snap) { x = candidatesX[a]; break }
                            }
                            var candidatesY = [o.y + o.height, o.y - h, o.y, o.y + o.height - h]
                            for (var b = 0; b < candidatesY.length; ++b) {
                                if (Math.abs(y - candidatesY[b]) < snap) { y = candidatesY[b]; break }
                            }
                        }
                        var key = modelData.key
                        if (page.commit()) DisplaySetup.setPosition(key, x, y)
                    }
                }
            }
        }
    }

    // ------------------------------------------------------- exact values
    RowLayout {
        Layout.fillWidth: true
        spacing: theme.spaceSmall

        Label { text: qsTr("Display"); color: theme.textSecondary; font.pointSize: 9; Layout.fillWidth: true; Layout.minimumWidth: 110 }
        Label { text: qsTr("Width"); color: theme.textSecondary; font.pointSize: 9; Layout.preferredWidth: 76 }
        Label { text: qsTr("Height"); color: theme.textSecondary; font.pointSize: 9; Layout.preferredWidth: 76 }
        Label { text: qsTr("X"); color: theme.textSecondary; font.pointSize: 9; Layout.preferredWidth: 76 }
        Label { text: qsTr("Y"); color: theme.textSecondary; font.pointSize: 9; Layout.preferredWidth: 76 }
        Label { text: qsTr("Workstation output"); color: theme.textSecondary; font.pointSize: 9; Layout.preferredWidth: 190 }
    }

    Repeater {
        id: geometryRows
        model: DisplaySetup.monitors

        delegate: RowLayout {
            function edit() {
                return {key: modelData.key, width: Number(widthField.text), height: Number(heightField.text),
                        x: Number(xField.text), y: Number(yField.text)}
            }
            Layout.fillWidth: true
            spacing: theme.spaceSmall
            enabled: modelData.on

            Label {
                text: (modelData.primary ? "★ " : "") + modelData.name
                color: modelData.included ? theme.textPrimary : theme.textDisabled
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.minimumWidth: 110
            }
            PlankTextField {
                id: widthField
                Layout.preferredWidth: 76
                text: modelData.included ? String(modelData.width) : ""
                validator: IntValidator { bottom: 2; top: 16384 }
                onTextEdited: page.pending = true
                Keys.onReturnPressed: page.commit()
                Keys.onEnterPressed: page.commit()
                Accessible.name: qsTr("Width for %1").arg(modelData.name)

            }
            PlankTextField {
                id: heightField
                Layout.preferredWidth: 76
                text: modelData.included ? String(modelData.height) : ""
                validator: IntValidator { bottom: 2; top: 16384 }
                onTextEdited: page.pending = true
                Keys.onReturnPressed: page.commit()
                Keys.onEnterPressed: page.commit()
                Accessible.name: qsTr("Height for %1").arg(modelData.name)

            }
            PlankTextField {
                id: xField
                Layout.preferredWidth: 76
                text: modelData.included ? String(modelData.x) : ""
                validator: IntValidator { bottom: -16384; top: 16384 }
                onTextEdited: page.pending = true
                Keys.onReturnPressed: page.commit()
                Keys.onEnterPressed: page.commit()
                Accessible.name: qsTr("X position for %1").arg(modelData.name)

            }
            PlankTextField {
                id: yField
                Layout.preferredWidth: 76
                text: modelData.included ? String(modelData.y) : ""
                validator: IntValidator { bottom: -16384; top: 16384 }
                onTextEdited: page.pending = true
                Keys.onReturnPressed: page.commit()
                Keys.onEnterPressed: page.commit()
                Accessible.name: qsTr("Y position for %1").arg(modelData.name)

            }
            PlankComboBox {
                id: outputChoice
                objectName: "workstationOutputPreference"
                Layout.preferredWidth: 190
                enabled: DisplaySetup.hostKind !== "legacy" && DisplaySetup.hostKind !== "mac"
                model: [qsTr("Automatic"), qsTr("Workstation screen"), qsTr("Virtual display")]
                currentIndex: page.backingIndex(modelData.preference)
                onActivated: function(index) {
                    var key = modelData.key
                    if (page.commit()) DisplaySetup.setPreference(key, ["auto", "physical", "virtual"][index])
                    else currentIndex = Qt.binding(function() { return page.backingIndex(modelData.preference) })
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Button {
            text: qsTr("Update preview")
            enabled: page.pending
            onClicked: page.commit()
        }
        Label {
            Layout.fillWidth: true
            text: page.error !== "" ? page.error : page.pending ? qsTr("Dimensions edited. Update the preview to check them; Save applies them too.") : ""
            visible: text !== ""
            color: page.error !== "" ? theme.warning : theme.textSecondary
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: theme.spaceMedium
        text: qsTr("How the desktop appears on this computer")
        color: theme.textPrimary
        font.weight: Font.DemiBold
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("The viewer fits the remote desktop to the window and keeps its proportions. This does not change text scaling inside the workstation.")
        color: theme.textSecondary
        wrapMode: Text.Wrap
    }
    // -------------------------------------------------------- presentation
    RowLayout {
        Layout.fillWidth: true
        spacing: theme.spaceMedium

        Label {
            text: qsTr("On this computer")
            color: theme.textSecondary
        }
        PlankComboBox {
            Layout.fillWidth: true
            objectName: "viewerPresentation"
            model: [qsTr("One fullscreen window per screen"), qsTr("One window for all screens")]
            currentIndex: DisplaySetup.presentation === "single" ? 1 : 0
            onActivated: function(index) {
                if (page.commit()) DisplaySetup.setPresentation(index === 1 ? "single" : "windows")
                else currentIndex = Qt.binding(function() { return DisplaySetup.presentation === "single" ? 1 : 0 })
            }
        }

    }

    Button {
        text: qsTr("Back to the suggested layout")
        flat: true
        onClicked: { page.pending = false; page.error = ""; DisplaySetup.resetToProposal() }
    }


}
