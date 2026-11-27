import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

Rectangle {
    id: card
    property var host
    property string statusText
    property bool statusCurrent: true
    property bool busy: false
    readonly property var people: statusCurrent ? (host.connectedUsers || []) : []
    // Explicit connection requests re-probe hosts the broker last saw offline.
    readonly property bool canConnect: host.connectable || (!host.online && host.reason === "offline")
    readonly property bool tryConnect: !statusCurrent || !host.connectable
    signal settingsRequested()
    signal connectRequested()
    signal benchmarkRequested()
    implicitHeight: Math.max(68, content.implicitHeight + 24)
    height: implicitHeight
    radius: theme.radiusMedium
    color: theme.surface
    border.width: 1
    border.color: theme.borderSubtle

    PlankTheme { id: theme }
    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 12
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        spacing: theme.spaceMedium
        Rectangle {
            width: 10; height: 10; radius: 5
            color: !card.statusCurrent || !card.host.online ? theme.textDisabled :
                   (!card.host.sharedDesktop && (card.host.busy || card.people.length > 0 || card.host.inUseBy !== "") ? theme.warning : theme.success)
            Layout.alignment: Qt.AlignVCenter
        }
        ColumnLayout {
            spacing: 3
            Layout.fillWidth: true
            Label {
                text: card.host.name
                color: theme.textPrimary
                font.pointSize: 12
                font.weight: Font.Medium
                elide: Label.ElideRight
                Layout.fillWidth: true
                textFormat: Text.PlainText
            }
            Label {
                objectName: "workstationStatus"
                text: card.statusCurrent ? card.statusText : qsTr("Status unavailable")
                color: theme.textSecondary
                font.pointSize: 10
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                textFormat: Text.PlainText
            }
            Label {
                objectName: "connectedPeople"
                visible: card.people.length > 0
                text: qsTr("Connected (%1): %2").arg(card.people.length).arg(card.people.join(", "))
                color: theme.textPrimary
                font.pointSize: 10
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                textFormat: Text.PlainText
            }
        }
        Button {
            objectName: "benchmarkButton"
            text: qsTr("Test connection…")
            Accessible.name: qsTr("Test connection to %1").arg(card.host.name)
            flat: true
            enabled: card.statusCurrent && card.host.connectable && !card.busy
            onClicked: card.benchmarkRequested()
        }
        Button {
            text: qsTr("Settings…")
            Accessible.name: qsTr("Settings for %1").arg(card.host.name)
            flat: true
            enabled: !card.busy
            onClicked: card.settingsRequested()
        }
        Button {
            objectName: "connectButton"
            text: card.canConnect && card.tryConnect ? qsTr("Try connecting") : qsTr("Connect")
            Accessible.name: card.canConnect && card.tryConnect ?
                             qsTr("Try connecting to %1").arg(card.host.name) :
                             qsTr("Connect to %1").arg(card.host.name)
            highlighted: card.canConnect
            enabled: card.canConnect && !card.busy
            onClicked: card.connectRequested()
        }
    }
}
