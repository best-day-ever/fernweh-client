import QtQml

QtObject {
    property bool started: false
    signal stageStarting(string stage)
    signal stageFailed(string stage, int errorCode, string failingPorts)
    signal connectionStarted()
    signal sessionCleanupWaitChanged(bool waiting, string text)
    signal activeSessionTakeoverRequested(string text)
    signal desktopSignOutRequested(string text)
    signal desktopCollaborationRequested(string text, bool signOutAvailable, bool unlockRequired)
    signal displayLaunchError(string text)
    signal displayLaunchWarning(string text)
    signal displaySetupRequested()
    signal sessionFinished()
    signal readyForDeletion()

    function exec(window) {
        started = true
    }
}
