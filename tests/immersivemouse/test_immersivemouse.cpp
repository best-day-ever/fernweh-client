#include "../../app/streaming/input/immersivemouse.h"
#include <QtTest>
#include <limits>

class TestImmersiveMouse : public QObject
{
    Q_OBJECT
    struct Fixture {
        bool nativeAllowed = true, transportWorks = true, absoluteWorks = true;
        int begins = 0, finishes = 0, failures = 0;
        bool reanchored = false;
        QPointF anchor;
        QStringList order;
        std::vector<PlankPointerPacket> packets;
        PlankImmersiveMouse mouse{PlankImmersiveMouse::Hooks{
            [this] { ++begins; order << "native-begin"; return nativeAllowed; },
            [this](QPointF point, bool reanchor) { ++finishes; anchor = point; reanchored = reanchor; order << "native-finish"; },
            [this](const unsigned char* bytes, unsigned length) {
                PlankPointerPacket packet {};
                if (plank_pointer_decode(bytes, length, &packet)) return false;
                packets.push_back(packet); order << QString::number(packet.operation);
                return transportWorks;
            },
            [this](QPointF point, QSize) { anchor = point; order << "absolute"; return absoluteWorks; },
            [this] { order << "buttons-up"; },
            [this] { ++failures; order << "failed"; }
        }};
        bool begin() { return mouse.toggle(true, QSize(3840,2160), QPointF(100,200), 10); }
        void end(std::uint64_t now = 100) { mouse.toggle(true, QSize(3840,2160), {}, now); }
        void ack(QPointF point = {123.5,456.25}, int sequenceDelta = 0, int epochDelta = 0, int widthDelta = 0) {
            const auto end = packets.back();
            PlankPointerPacket packet {PLANK_POINTER_END_ACK, end.epoch + epochDelta, end.sequence + sequenceDelta,
                std::uint16_t(end.width + widthDelta), end.height, float(point.x()), float(point.y())};
            unsigned char bytes[PLANK_POINTER_PACKET_SIZE]; plank_pointer_encode(bytes, &packet);
            mouse.acknowledge(bytes, sizeof(bytes));
        }
    };
private slots:
    void startsOffAndRequiresPermission()
    {
        Fixture f;
        QVERIFY(!f.mouse.enabled()); QVERIFY(!f.mouse.ownsPointer());
        QVERIFY(!f.mouse.toggle(false, {1920,1080}, {30,40}, 0));
        QCOMPARE(f.begins,0); QVERIFY(f.packets.empty());
    }
    void invalidGeometryAndAnchor_data()
    {
        QTest::addColumn<QSize>("size"); QTest::addColumn<QPointF>("point");
        QTest::newRow("empty") << QSize(0,1080) << QPointF(1,2);
        QTest::newRow("oversized") << QSize(SHRT_MAX+1,1080) << QPointF(1,2);
        QTest::newRow("nan") << QSize(1920,1080) << QPointF(std::numeric_limits<double>::quiet_NaN(),2);
        QTest::newRow("infinity") << QSize(1920,1080) << QPointF(1,std::numeric_limits<double>::infinity());
    }
    void invalidGeometryAndAnchor()
    {
        QFETCH(QSize,size); QFETCH(QPointF,point); Fixture f;
        QVERIFY(!f.mouse.toggle(true,size,point,0)); QCOMPARE(f.begins,0);
    }
    void refusedNativeCaptureKeepsAbsoluteMode()
    {
        Fixture f; f.nativeAllowed=false;
        QVERIFY(!f.begin()); QVERIFY(!f.mouse.enabled()); QVERIFY(!f.mouse.ownsPointer());
        QVERIFY(f.packets.empty()); QCOMPARE(f.failures,0);
    }
    void rawMotionContinuesAcrossButtonReleases()
    {
        Fixture f; QVERIFY(f.begin());
        f.mouse.motion({10.25,-3.5}); f.mouse.motion({0.5,17});
        QCOMPARE(f.packets.size(),std::size_t(3));
        QCOMPARE(f.packets[0].operation,std::uint8_t(PLANK_POINTER_BEGIN));
        QCOMPARE(f.packets[1].operation,std::uint8_t(PLANK_POINTER_MOVE));
        QCOMPARE(f.packets[1].x,10.25f); QCOMPARE(f.packets[1].y,-3.5f);
        QCOMPARE(f.packets[2].x,0.5f); QCOMPARE(f.packets[2].y,17.0f);
        QVERIFY(f.mouse.enabled()); QCOMPARE(f.finishes,0);
        QVERIFY(!f.order.contains("absolute"));
    }
    void completionWaitsForAuthoritativeAck()
    {
        Fixture f; QVERIFY(f.begin()); f.end();
        QVERIFY(!f.mouse.enabled()); QVERIFY(f.mouse.ownsPointer()); QVERIFY(f.mouse.ending());
        QCOMPARE(f.order[f.order.size()-2],QString("buttons-up"));
        QCOMPARE(f.packets.back().operation,std::uint8_t(PLANK_POINTER_END));
        QCOMPARE(f.finishes,0);
        f.ack();
        QCOMPARE(f.finishes,1); QVERIFY(f.reanchored); QCOMPARE(f.anchor,QPointF(123.5,456.25));
        QVERIFY(!f.mouse.ownsPointer()); QVERIFY(!f.mouse.enabled());
    }
    void staleRepliesCannotReleaseOrReviveCapture()
    {
        Fixture f; QVERIFY(f.begin()); f.end();
        f.ack({12,34},1); QVERIFY(f.mouse.ending()); QCOMPARE(f.finishes,0);
        f.ack({12,34},0,1); QVERIFY(f.mouse.ending());
        f.mouse.cancel(); QCOMPARE(f.finishes,1);
        f.ack(); QVERIFY(!f.mouse.enabled()); QVERIFY(!f.mouse.ownsPointer()); QCOMPARE(f.finishes,1);
    }
    void deferredMovementAndActionsStayOrdered()
    {
        Fixture f; QVERIFY(f.begin()); f.end();
        f.mouse.motion({10.5,-2.25});
        QVERIFY(f.mouse.defer([&](QPointF p) { f.order << "button"; QCOMPARE(p,QPointF(134,454)); }));
        f.mouse.motion({-5,3});
        QVERIFY(f.mouse.defer([&](QPointF p) { f.order << "key"; QCOMPARE(p,QPointF(129,457)); }));
        QCOMPARE(f.finishes,0); f.ack();
        QCOMPARE(f.order.mid(f.order.size()-5),QStringList({"absolute","button","absolute","key","native-finish"}));
        QCOMPARE(f.anchor,QPointF(129,457)); QCOMPARE(f.failures,0);
    }
    void deferredMovementClampsOnlyAfterRelease()
    {
        Fixture f; QVERIFY(f.begin()); f.mouse.motion({-20,0});
        QCOMPARE(f.packets.back().x,-20.0f);
        f.end(); f.mouse.motion({-500,10000}); f.ack({10,20});
        QCOMPARE(f.anchor,QPointF(0,2159)); QCOMPARE(f.failures,0);
    }
    void aRapidSecondToggleStartsANewEpochAfterAck()
    {
        Fixture f; QVERIFY(f.begin()); const auto epoch=f.packets.back().epoch; f.end();
        QVERIFY(f.mouse.toggle(true,{3840,2160},{1,2},101));
        f.mouse.motion({4,5}); f.ack();
        QVERIFY(f.mouse.enabled()); QVERIFY(f.mouse.captured());
        QCOMPARE(f.packets[f.packets.size()-2].operation,std::uint8_t(PLANK_POINTER_BEGIN));
        QCOMPARE(f.packets.back().epoch,epoch+1); QCOMPARE(f.finishes,0);
        f.end(200); f.ack(); QCOMPARE(f.finishes,1);
    }
    void lostNativeFocusDuringRestartReleasesThePointer()
    {
        Fixture f; QVERIFY(f.begin()); f.end();
        QVERIFY(f.mouse.toggle(true,{3840,2160},{1,2},101));
        f.nativeAllowed=false; f.ack();
        QVERIFY(!f.mouse.enabled()); QVERIFY(!f.mouse.ownsPointer()); QCOMPARE(f.finishes,1);
    }
    void timeoutFailsClosedAtTheDeadline()
    {
        Fixture f; QVERIFY(f.begin()); f.end(100);
        f.mouse.poll(2099); QCOMPARE(f.failures,0); QVERIFY(f.mouse.ownsPointer());
        f.mouse.poll(2100); QCOMPARE(f.failures,1); QVERIFY(!f.mouse.ownsPointer());
        QVERIFY(!f.reanchored); QCOMPARE(f.packets.back().operation,std::uint8_t(PLANK_POINTER_CANCEL));
    }
    void malformedAckAndWrongDimensionsFailClosed()
    {
        Fixture f; QVERIFY(f.begin()); f.end(); f.ack({10,20},0,0,1);
        QCOMPARE(f.failures,1); QVERIFY(!f.mouse.ownsPointer());
        Fixture g; QVERIFY(g.begin()); g.end(); g.mouse.acknowledge(nullptr,0);
        QCOMPARE(g.failures,1); QVERIFY(!g.mouse.ownsPointer());
    }
    void unrepresentableHostAnchorFailsClosed()
    {
        Fixture f; QVERIFY(f.begin()); f.end(); f.ack({4000,10});
        QCOMPARE(f.failures,1); QVERIFY(!f.reanchored);
    }
    void cancellationDropsDeferredInputAndResetsMode()
    {
        Fixture f; QVERIFY(f.begin()); f.end();
        int forwarded=0; f.mouse.defer([&](QPointF) { ++forwarded; });
        f.mouse.cancel(); QCOMPARE(forwarded,0); QCOMPARE(f.finishes,1); QVERIFY(!f.reanchored);
        QVERIFY(f.begin()); QVERIFY(f.mouse.enabled()); f.mouse.cancel(); QCOMPARE(f.finishes,2);
    }
    void queuedInputIsBounded()
    {
        Fixture f; QVERIFY(f.begin()); f.end(); int forwarded=0;
        for(int i=0;i<129;++i) f.mouse.defer([&](QPointF) { ++forwarded; });
        QCOMPARE(f.failures,1); QCOMPARE(forwarded,0); QVERIFY(!f.mouse.ownsPointer());
    }
    void sendFailuresNeverFallBackToAbsoluteMotion()
    {
        Fixture f; f.transportWorks=false; QVERIFY(!f.begin());
        QCOMPARE(f.failures,1); QVERIFY(!f.mouse.ownsPointer()); QVERIFY(!f.order.contains("absolute"));
        Fixture g; QVERIFY(g.begin()); g.transportWorks=false; g.mouse.motion({1,2});
        QCOMPARE(g.failures,1); QVERIFY(!g.mouse.ownsPointer());
    }
    void badMotionAndFailedAbsoluteReplayReleaseNativeCapture()
    {
        Fixture f; QVERIFY(f.begin()); f.mouse.motion({std::numeric_limits<double>::infinity(),0});
        QCOMPARE(f.failures,1); QVERIFY(!f.mouse.ownsPointer());
        Fixture g; QVERIFY(g.begin()); g.end(); g.mouse.motion({1,2}); g.absoluteWorks=false; g.ack();
        QCOMPARE(g.failures,1); QVERIFY(!g.reanchored);
    }
};
QTEST_GUILESS_MAIN(TestImmersiveMouse)
#include "test_immersivemouse.moc"
