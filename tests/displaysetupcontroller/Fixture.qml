import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Controls.Material 2.15
import "../../app/gui" as Fernweh

ApplicationWindow {
    id: window
    width: 1100
    height: 1000
    visible: true
    Material.theme: Material.Dark
    Material.primary: "#393D43"
    Material.accent: "#5A9CE6"
    Item { id: stackView; anchors.fill: parent }
    Fernweh.DisplaySetupDialog { id: setup; objectName: "setup" }
}
