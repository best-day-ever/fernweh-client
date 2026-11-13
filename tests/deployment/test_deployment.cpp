#include <QtTest/QtTest>
#include "../../app/backend/plankbroker.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QResource>
#include <QSettings>
#include <QTemporaryDir>
#include "../../app/settings/streamingpreferences.h"
#include "../../app/streaming/camera/camera.h"

// The build contract needs preference metadata, never a real camera device.
QVariantList PlankCamera::devices() { return {}; }

class DeploymentTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDirectory;
private slots:
    void initTestCase() {
        QVERIFY(settingsDirectory.isValid());
        QCoreApplication::setOrganizationName("FernwehBuildTests");
        QCoreApplication::setApplicationName("DeploymentProfiles");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    }
    void effectiveBuildSettings() {
        QCOMPARE(FernwehDeployment::BrokerEnabled, qEnvironmentVariable("FERNWEH_BROKER_ENABLED") == "1");
        if (!FernwehDeployment::BrokerEnabled) {
            QVERIFY(PlankBroker::defaultHost().isEmpty());
            QVERIFY(PlankBroker::defaultPins().isEmpty());
            QVERIFY(PlankBroker::defaultPasskeyRpId().isEmpty());
            QVERIFY(!FernwehDeployment::UpdatesEnabled);
            return;
        }
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
    void compiledResourcesMatchCapabilities() {
        QVERIFY(QResource(QStringLiteral(":/gui/PcView.qml")).isValid());
        QVERIFY(QResource(QStringLiteral(":/gui/SettingsView.qml")).isValid());
        QVERIFY(QResource(QStringLiteral(":/gui/DisplaySetupDialog.qml")).isValid());
        for (const char* name : {":/gui/RemoteView.qml", ":/gui/OnboardingView.qml",
                                ":/backend/fernweh-install-update.ps1"}) {
            QCOMPARE(QResource(QString::fromLatin1(name)).isValid(), FernwehDeployment::BrokerEnabled);
        }
    }
    void existingBrokerPreferencesCannotEnableDisabledBuild() {
        QSettings saved;
        const QString pin(64, QLatin1Char('c'));
        saved.setValue("plank-broker-host", "saved.example.org");
        saved.setValue("plank-broker-port", 29443);
        saved.setValue("plank-broker-spki-pins", QStringList{pin});
        saved.sync();
        auto* preferences = StreamingPreferences::get();
        preferences->reload();
        if (FernwehDeployment::BrokerEnabled) {
            QCOMPARE(preferences->brokerHost, QStringLiteral("saved.example.org"));
            QCOMPARE(preferences->brokerPins, QStringList{pin});
        } else {
            QVERIFY(preferences->brokerHost.isEmpty());
            QVERIFY(preferences->brokerPins.isEmpty());
        }
        preferences->save();
        saved.sync();
        QCOMPARE(saved.value("plank-broker-host").toString(), QStringLiteral("saved.example.org"));
        QCOMPARE(saved.value("plank-broker-spki-pins").toStringList(), QStringList{pin});
    }
    void sharedViewsCompileWithOnlyAvailableModules() {
        auto singleton = [](QQmlEngine*, QJSEngine*) -> QObject* { return new QObject; };
        qmlRegisterSingletonType<QObject>("ComputerManager", 1, 0, "ComputerManager", singleton);
        qmlRegisterSingletonType<QObject>("SystemProperties", 1, 0, "SystemProperties", singleton);
        qmlRegisterUncreatableType<StreamingPreferences>("StreamingPreferences", 1, 0, "StreamingPreferences", "test metadata");
        qmlRegisterUncreatableType<QObject>("InternetConnectivity", 1, 0, "InternetConnectivity", "test type");
        qmlRegisterUncreatableType<QObject>("Session", 1, 0, "Session", "test type");
        qmlRegisterSingletonType<QObject>("DisplaySetup", 1, 0, "DisplaySetup", singleton);
        if (FernwehDeployment::BrokerEnabled) {
            qmlRegisterSingletonType<QObject>("RemoteBroker", 1, 0, "RemoteBroker", singleton);
            qmlRegisterSingletonType<QObject>("FernwehUpdater", 1, 0, "FernwehUpdater", singleton);
        }
        QQmlEngine engine;
        for (const char* name : {"qrc:/gui/main.qml", "qrc:/gui/SettingsView.qml", "qrc:/gui/StreamSegue.qml"}) {
            QQmlComponent component(&engine, QUrl(QString::fromLatin1(name)));
            QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        }
    }
};
QTEST_MAIN(DeploymentTest)
#include "test_deployment.moc"
