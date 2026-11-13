#include <QtTest/QtTest>
#include "../../app/backend/plankbroker.h"

class DeploymentTest : public QObject {
    Q_OBJECT
private slots:
    void effectiveBuildSettings() {
        QCOMPARE(PlankBroker::defaultHost(), qEnvironmentVariable("FERNWEH_BROKER_HOST").trimmed().toLower());
        QCOMPARE(PlankBroker::defaultPasskeyRpId(), qEnvironmentVariable("FERNWEH_PASSKEY_RP_ID").trimmed().toLower());
        QCOMPARE(PlankBroker::defaultPins(), PlankBroker::parsePinList(qEnvironmentVariable("FERNWEH_BROKER_PINS")));
        const QString port = qEnvironmentVariable("FERNWEH_BROKER_PORT", "29000");
        QCOMPARE(PlankBroker::DefaultPort, quint16(port.toUShort()));
        QCOMPARE(FernwehDeployment::UpdatesEnabled, qEnvironmentVariable("FERNWEH_UPDATES_ENABLED") == "1");
    }
    void absentTrustNeverMatches() {
        QVERIFY(!PlankBroker::spkiPinMatches({}, PlankBroker::defaultPins()));
        if (PlankBroker::defaultHost().isEmpty()) {
            QVERIFY(PlankBroker::defaultPins().isEmpty());
            QVERIFY(!FernwehDeployment::UpdatesEnabled);
        }
    }
};
QTEST_GUILESS_MAIN(DeploymentTest)
#include "test_deployment.moc"
