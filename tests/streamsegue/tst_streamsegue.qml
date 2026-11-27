import QtQuick
import QtQuick.Controls
import QtTest
import Session 1.0

Item {
    ApplicationWindow {
        id: window
        width: 640
        height: 480
        visible: true

        ToolBar { id: toolBar }
        Item { id: remotePage }
        QtObject {
            id: streamSegueErrorDialog
            property string text
            property bool quitAfter
            property bool visible
            function open() { visible = true }
        }
        StackView {
            id: stackView
            anchors.fill: parent
            initialItem: remotePage
            popExit: Transition { PauseAnimation { duration: 300 } }
        }
    }

    Component { id: mockSession; Session {} }

    TestCase {
        name: "StreamSegue"
        property Item segue
        property Session session

        function init() {
            window.show()
            streamSegueErrorDialog.text = ""
            streamSegueErrorDialog.visible = false
            if (stackView.depth === 0)
                stackView.push(remotePage, StackView.Immediate)
            session = mockSession.createObject(window)
            const component = Qt.createComponent("../../app/gui/StreamSegue.qml")
            compare(component.status, Component.Ready, component.errorString())
            segue = component.createObject(stackView, {"session": session})
            stackView.push(segue, StackView.Immediate)
            tryCompare(session, "started", true)
        }

        function cleanup() {
            stackView.clear(StackView.Immediate)
            if (segue !== null)
                segue.destroy()
            session.destroy()
            window.show()
        }

        function test_failedStreamLeavesErrorVisible() {
            const spinner = findChild(segue, "connectionSpinner")
            tryCompare(spinner, "running", true)

            session.stageFailed("network", 42, "")
            session.sessionFinished()
            session.readyForDeletion()
            compare(spinner.running, false)
            tryCompare(this, "segue", null)
            verify(streamSegueErrorDialog.visible)
            verify(streamSegueErrorDialog.text.includes("42"))
        }

        function test_hiddenSpinnerStops() {
            const spinner = findChild(segue, "connectionSpinner")
            verify(spinner !== null)
            tryCompare(spinner, "running", true)

            session.connectionStarted()
            compare(spinner.visible, false)
            compare(spinner.running, false)
            session.sessionFinished()
            session.readyForDeletion()
            tryCompare(this, "segue", null)
        }

        function test_waitsForSessionCleanupAfterRemoval() {
            session.sessionFinished()
            tryCompare(stackView, "busy", false)
            verify(segue !== null)
            compare(segue.session, session)

            session.readyForDeletion()
            tryCompare(this, "segue", null)
        }

        function test_waitsForStackTransitionAfterCleanup() {
            session.sessionFinished()
            verify(stackView.busy)
            session.readyForDeletion()
            verify(segue !== null)

            tryCompare(stackView, "busy", false)
            tryCompare(this, "segue", null)
        }
    }
}
