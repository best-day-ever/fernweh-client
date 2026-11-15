#include <QtTest>
#include "backend/macmediafeatures.h"

namespace {
const QString Mode = QStringLiteral("hevc-10-444-videotoolbox");
QJsonObject selection(const QJsonObject& offer)
{
    QJsonObject features;
    const auto offered = offer.value("features").toObject();
    for (const auto& name : MacMediaFeatures::names()) {
        const auto choices = offered.value(name).toArray();
        features.insert(name, choices.isEmpty() ? QJsonValue::Null : choices.first());
    }
    return {{"schema_version", 1}, {"launch_schema", 7},
            {"transport", MacMediaFeatures::transport()},
            {"required_features", MacMediaFeatures::required()}, {"features", features}};
}
}

class MediaFeaturesTest : public QObject
{
    Q_OBJECT
private slots:
    void microphoneOfferMatchesNativeCapture()
    {
        const auto profiles = MacMediaFeatures::offer(Mode).value("features").toObject().value("microphone").toArray();
#ifdef Q_OS_LINUX
        QCOMPARE(profiles.size(), 2);
        QCOMPARE(profiles.first().toObject().value("schema_version").toInt(), 3);
#else
        // This executes with the actual native Windows/macOS compiler flags.
        // Both adapters must retain v2 instead of promising a timed v3 clock.
        QCOMPARE(profiles.size(), 1);
#endif
        QCOMPARE(profiles.last().toObject().value("schema_version").toInt(), 2);
        QVERIFY(!profiles.last().toObject().contains("capture_clock"));
    }

    void selectedMicrophoneMustHaveBeenOffered()
    {
        const auto offer = MacMediaFeatures::offer(Mode);
        auto response = selection(offer);
        MacMediaFeatures::Agreement agreement;
        QVERIFY(MacMediaFeatures::select(response, Mode, agreement));
        QVERIFY(agreement.enabled(QStringLiteral("microphone")));

        auto features = response.value("features").toObject();
        auto microphone = MacMediaFeatures::profile(QStringLiteral("microphone"), Mode);
        microphone.insert("schema_version", 3);
        microphone.insert("capture_clock", "monotonic-ns");
        features.insert("microphone", microphone);
        response.insert("features", features);
#ifdef Q_OS_LINUX
        QVERIFY(MacMediaFeatures::select(response, Mode, agreement));
#else
        QVERIFY(!MacMediaFeatures::select(response, Mode, agreement));
        QVERIFY(!agreement.enabled(QStringLiteral("microphone")));
#endif
        microphone.insert("schema_version", 4);
        features.insert("microphone", microphone);
        response.insert("features", features);
        QVERIFY(!MacMediaFeatures::select(response, Mode, agreement));
    }

    void optionalMicrophoneCanRemainDisabled()
    {
        auto response = selection(MacMediaFeatures::offer(Mode));
        auto features = response.value("features").toObject();
        features.insert("microphone", QJsonValue::Null);
        response.insert("features", features);
        MacMediaFeatures::Agreement agreement;
        QVERIFY(MacMediaFeatures::select(response, Mode, agreement));
        QVERIFY(!agreement.enabled(QStringLiteral("microphone")));
    }
};
QTEST_GUILESS_MAIN(MediaFeaturesTest)
#include "test_mediafeatures.moc"
