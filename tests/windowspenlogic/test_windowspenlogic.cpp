#include <QtTest>
#include "windowspenlogic.h"
#include "macpen.h"
#include <limits>
#include <vector>

class TestWindowsPen : public QObject {
    Q_OBJECT
private slots:
    void fractionalSamplesStayNearWindowsPointer() {
        WindowsPenLogic::FractionalPosition position;
        for (int i=0; i<1000; ++i) {
            const float pixel=float(i/10);
            const float result=position.sample(pixel, double(pixel)+2.35);
            QVERIFY(std::abs(result-pixel)<=1);
        }
        QCOMPARE(position.sample(10, 1000), 10.0f);
        QCOMPARE(position.sample(10, std::numeric_limits<double>::quiet_NaN()), 10.0f);
        position.reset();
        QCOMPARE(position.sample(20, 22.5), 20.5f);
    }
    void mouseReturnsAfterHoverButNeverCancelsHeldStroke() {
        QVERIFY(WindowsPenLogic::ownsPointer(true, .1f, 100, 10000));
        QVERIFY(WindowsPenLogic::ownsPointer(true, 0, 100, 399));
        QVERIFY(!WindowsPenLogic::ownsPointer(true, 0, 100, 400));
        QVERIFY(!WindowsPenLogic::ownsPointer(false, .1f, 100, 100));
    }
    void inkAndWintabHaveOnePositionOwner() {
        WindowsPenLogic::Authority authority;
        authority.ink(100);
        QVERIFY(!authority.drive(300, true, .1f));
        QVERIFY(!authority.drive(400, true, .1f));
        QVERIFY(authority.drive(520, true, .1f));
        QVERIFY(!authority.acceptsContact()); // A stroke started before takeover.
        QVERIFY(authority.drive(530, true, 0));
        QVERIFY(authority.acceptsContact());
        QVERIFY(authority.drive(550, true, .0001f));
        authority.ink(560);
        QVERIFY(!authority.drive(561, true, .1f));
        QVERIFY(!authority.drive(1000, false, .1f));
    }
    void nativeSamplesKeepLightStrokeEdgesAndPressure() {
        std::vector<PlankMacPenPacket> sent;
        PlankMacPenInput pen([&](const auto& p){sent.push_back(p);});
        pen.setActive(true);
        pen.handleSample(1,.1f,.2f,0,false,0);
        pen.handleSample(1,.1f,.2f,1.0f/8191,true,0);
        for (int i=1;i<8192;++i) pen.handleSample(1,.1f,.2f,float(i)/8191,true,0);
        pen.handleSample(1,.1f,.2f,0,false,0);
        QCOMPARE(sent.front().eventType, uint8_t(LI_TOUCH_EVENT_HOVER));
        QCOMPARE(sent.at(1).eventType, uint8_t(LI_TOUCH_EVENT_DOWN));
        QCOMPARE(sent.at(1).pressureOrDistance, 1.0f/8191);
        QCOMPARE(sent.at(2).eventType, uint8_t(LI_TOUCH_EVENT_MOVE));
        QCOMPARE(sent.back().eventType, uint8_t(LI_TOUCH_EVENT_UP));
        for (int i=2;i<int(sent.size())-1;++i) QCOMPARE(sent.at(i).eventType,uint8_t(LI_TOUCH_EVENT_MOVE));
    }
    void releaseAndNewPenNeverContinueOldContact() {
        std::vector<PlankMacPenPacket> sent;
        PlankMacPenInput pen([&](const auto& p){sent.push_back(p);});
        pen.setActive(true); pen.handleSample(1,.2f,.3f,.1f,true,0);
        pen.setActive(false);
        QCOMPARE(sent.back().eventType,uint8_t(LI_TOUCH_EVENT_CANCEL_ALL));
        const auto count=sent.size(); pen.handleSample(1,.2f,.3f,.1f,true,0);
        QCOMPARE(sent.size(),count);
        pen.setActive(true); pen.handleSample(2,.6f,.7f,.2f,true,0);
        QCOMPARE(sent.back().eventType,uint8_t(LI_TOUCH_EVENT_DOWN));
        pen.handleProximityOut(2);
        QCOMPARE(sent.back().eventType,uint8_t(LI_TOUCH_EVENT_HOVER_LEAVE));
        pen.handleSample(3,.4f,.5f,.3f,true,PlankMacPenInputFlags::EraserTip);
        QCOMPARE(sent.back().toolType,uint8_t(LI_TOOL_TYPE_ERASER));
    }
};
QTEST_GUILESS_MAIN(TestWindowsPen)
#include "test_windowspenlogic.moc"
