#include <QtTest>
#include "../../app/streaming/qt/sessionlayout.h"
#include "../../app/streaming/qt/sessiongeometry.h"
#include "../../app/streaming/clientdisplaychange.h"
#include <QTemporaryDir>
#include <future>
#include <thread>

namespace {
NvClientDisplay monitor(const QString& key, QRect bounds, QSize pixels)
{
    NvClientDisplay value{bounds, pixels, pixels}; value.key = key; return value;
}
QVector<NvClientDisplay> monitors()
{
    return {monitor(QStringLiteral("laptop"), {0,0,1920,1080}, {1920,1080}),
            monitor(QStringLiteral("left"), {1920,0,3440,1440}, {3440,1440}),
            monitor(QStringLiteral("right"), {5360,0,2560,1440}, {5120,2880})};
}
DisplayPlanner::Plan packedPlan()
{
    DisplayPlanner::Plan plan; plan.ok = true; plan.packed = true;
    plan.presentation = QStringLiteral("windows");
    plan.canvas = {8560,2880}; plan.capture = {5120,4320};
    for (const auto& display : monitors()) {
        DisplayPlanner::Output output; output.key = display.key; output.clientBounds = display.bounds;
        output.on = display.key != QStringLiteral("laptop"); output.included = output.on;
        output.primary = display.key == QStringLiteral("right");
        output.position = display.key == QStringLiteral("right") ? QPoint(3440,0) : QPoint();
        output.size = display.nativeSize;
        output.captureRect = display.key == QStringLiteral("right") ? QRect(0,1440,5120,2880) : QRect(0,0,3440,1440);
        plan.outputs.append(output);
    }
    return plan;
}
NvOutputTopology ownerTopology()
{
    NvOutputTopology topology; topology.desktopWidth = 8560; topology.desktopHeight = 2880;
    topology.captureWidth = 5120; topology.captureHeight = 4320;
    NvOutput left; left.id = QStringLiteral("one"); left.width=3440; left.height=1440;
    left.captureWidth=3440; left.captureHeight=1440; left.primary=true;
    NvOutput right; right.id=QStringLiteral("two"); right.x=3440; right.width=5120; right.height=2880;
    right.captureY=1440; right.captureWidth=5120; right.captureHeight=2880;
    topology.outputs={left,right}; return topology;
}
}
class QtDisplayLayoutTest : public QObject
{
    Q_OBJECT
private slots:
    void savedSelectionPrimaryAndPackedCropsBindByIdentity()
    {
        const auto plan = packedPlan();
        auto displays = monitors(); std::reverse(displays.begin(),displays.end());
        const auto layout = qtSessionLayout(plan, displays, {}, plan.capture);
        QVERIFY(layout.planned); QVERIFY(!layout.guest);
        QCOMPARE(layout.outputs.size(),2);
        QCOMPARE(layout.outputs[0].key,QStringLiteral("right"));
        QVERIFY(layout.outputs[0].primary);
        QCOMPARE(layout.outputs[0].sourcePixels,QRectF(0,1440,5120,2880));
        QCOMPARE(layout.outputs[0].desktopPixels,QRectF(3440,0,5120,2880));
        QCOMPARE(layout.desktopSize,QSize(8560,2880));
        for (const auto& output : layout.outputs) QVERIFY(output.key != QStringLiteral("laptop"));
    }
    void authenticatedHostCaptureRectanglesOverrideThePrediction()
    {
        auto plan=packedPlan();
        plan.arrangement=QStringLiteral("1:3440x1440+0+0:auto,5120x2880+3440+0:auto");
        plan.outputs[1].primary=true; plan.outputs[2].primary=false;
        plan.outputs[1].arrangementIndex=0; plan.outputs[2].arrangementIndex=1;
        auto topology=ownerTopology();
        topology.featureFlags=NvOutputTopology::DisplayArrangementFeature;
        topology.displayCapabilities.valid=true;
        topology.arrangementRequest=plan.arrangement;
        topology.arrangementState=QStringLiteral("applied");
        topology.outputs[0].arrangementIndex=0; topology.outputs[1].arrangementIndex=1;
        topology.outputs[0].captureY=2880; topology.outputs[1].captureY=0;
        QVERIFY(topology.matchesRequestedArrangement(plan.arrangement));
        const auto layout=qtSessionLayout(plan,monitors(),topology,plan.capture);
        QCOMPARE(layout.outputs[0].sourcePixels,QRectF(0,2880,3440,1440));
        QCOMPARE(layout.outputs[1].sourcePixels,QRectF(0,0,5120,2880));
        QCOMPARE(layout.outputs[1].desktopPixels,QRectF(3440,0,5120,2880));
    }
    void oneWindowPreferenceKeepsWholeUnpackedDesktopOrPrimaryPackedView()
    {
        auto plan=packedPlan(); plan.presentation=QStringLiteral("single");
        const auto packed=qtSessionLayout(plan,monitors(),{},plan.capture);
        QCOMPARE(packed.outputs.size(),1); QVERIFY(packed.outputs.first().primary);
        QCOMPARE(packed.outputs.first().sourcePixels,QRectF(0,1440,5120,2880));
        plan.packed=false; plan.capture=plan.canvas;
        const auto whole=qtSessionLayout(plan,monitors(),{},plan.capture);
        QCOMPARE(whole.outputs.size(),1);
        QCOMPARE(whole.outputs.first().sourcePixels,QRectF(QPointF(),plan.canvas));
        QCOMPARE(whole.outputs.first().desktopPixels,QRectF(QPointF(),plan.canvas));
    }
    void packedPictureAndDesktopInputUseDifferentRectangles()
    {
        QtSessionGeometry geometry{{5120,4320},{0,1440,5120,2880},{0,0,1024,576},
            {1024,576},{1024,576},{8560,2880},{3440,0,5120,2880}};
        QVERIFY(geometry.valid()); QPointF desktop, local;
        QVERIFY(geometry.toStream({512,288},desktop)); QCOMPARE(desktop,QPointF(6000,1440));
        QVERIFY(geometry.fromStream(desktop,local)); QCOMPARE(local,QPointF(512,288));
        QVERIFY(geometry.fromCapture({2560,2880},local)); QCOMPARE(local,QPointF(512,288));
        QVERIFY(!geometry.fromStream({1000,1440},local));
        QVERIFY(!geometry.fromCapture({2560,1000},local));
        QVERIFY(geometry.toStream({2000,-100},desktop,true)); QCOMPARE(desktop,QPointF(8559,0));
    }
    void guestOutputOnlyUsesOwnerDesktopWithoutReplanning()
    {
        const auto topology = ownerTopology();
        const auto layout = qtSessionLayout({},monitors(),topology,{5120,2880},true,QStringLiteral("two"));
        QVERIFY(layout.guest); QCOMPARE(layout.outputs.size(),1);
        QCOMPARE(layout.outputs[0].sourcePixels,QRectF(0,0,5120,2880));
        QCOMPARE(layout.outputs[0].desktopPixels,QRectF(3440,0,5120,2880));
        const auto full = qtSessionLayout({},monitors(),topology,{5120,4320},true);
        QCOMPARE(full.outputs.size(),2); QVERIFY(full.outputs.first().primary);
        QCOMPARE(full.outputs[1].sourcePixels,QRectF(0,1440,5120,2880));
        QVERIFY(qtSessionLayout({},monitors(),topology,{5120,2880},true,QStringLiteral("missing")).outputs.isEmpty());
    }
    void keepAndTransientRestorePreserveAppliedBaseline()
    {
        QTemporaryDir directory; QSettings settings(directory.filePath("profile.ini"),QSettings::IniFormat);
        DisplayProfile::setAskOnChange(settings,true);
        ClientDisplayChange changes; const auto applied=monitors(); changes.applied(applied);
        auto changed=applied; changed[1].bounds.moveTop(100);
        QVERIFY(changes.observe(changed)); QVERIFY(changes.shouldPrompt(settings));
        changes.keep(); QVERIFY(!changes.shouldPrompt(settings)); QVERIFY(!changes.matchesApplied());
        QVERIFY(changes.observe(applied)); QVERIFY(changes.matchesApplied()); QVERIFY(!changes.shouldPrompt(settings));
        QVERIFY(changes.observe(changed)); QVERIFY(!changes.shouldPrompt(settings));
        changes.applied(changed); QVERIFY(changes.matchesApplied());
        DisplayProfile::setAskOnChange(settings,false);
        QVERIFY(changes.observe(applied)); QVERIFY(!changes.shouldPrompt(settings));
    }
    void layoutAcknowledgementRunsOnGuiOwner()
    {
        QObject owner; QtSessionLayoutHandoff handoff; QThread* appliedOn=nullptr;
        auto result=std::async(std::launch::async,[&] {return handoff.request(&owner,[&] {appliedOn=QThread::currentThread();return true;});});
        QTRY_VERIFY(result.wait_for(std::chrono::milliseconds(0))==std::future_status::ready);
        QVERIFY(result.get()); QCOMPARE(appliedOn,owner.thread());
    }
    void cancellationReleasesAnUnservicedLayoutAndRemainsTerminal()
    {
        QObject owner; QtSessionLayoutHandoff handoff; std::atomic_bool applied{false};
        auto result=std::async(std::launch::async,[&] {return handoff.request(&owner,[&] {applied=true;return true;});});
        handoff.cancel();
        QVERIFY(result.wait_for(std::chrono::seconds(1))==std::future_status::ready);
        QVERIFY(!result.get()); QCoreApplication::processEvents(); QVERIFY(!applied.load());
        QVERIFY(!handoff.request(&owner,[] {return true;}));
    }
};
QTEST_GUILESS_MAIN(QtDisplayLayoutTest)
#include "test_qtdisplaylayout.moc"
