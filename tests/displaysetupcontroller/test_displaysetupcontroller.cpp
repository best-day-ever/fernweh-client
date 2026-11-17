#include <QtTest>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QTemporaryDir>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include "gui/displaysetupcontroller.h"
#include "backend/displaymonitor.h"
#include "backend/clientdisplayprobe.h"
#include "backend/remotestreamsetup.h"
#include "backend/remotedisplaysetup.h"
#include <QJsonDocument>
#include "streaming/video/decodercaps.h"
#include "streaming/macdisplayinfo.h"
namespace {
bool standardDpi = false;
bool secondDisplay = false;
QVector<NvClientDisplay> fixtureDisplays()
{
    NvClientDisplay laptop{QRect(0, 0, 1512, 982), QSize(3024, 1964), QSize(3024, 1964), QSize(3024, 1898)};
    laptop.key = "uuid:fixture-retina";
    laptop.name = "Retina fixture";
    laptop.main = true;
    laptop.builtIn = true;
    laptop.notch = true;
    if (standardDpi) {
        laptop.bounds = QRect(0, 0, 1920, 1080);
        laptop.nativeSize = laptop.backingSize = laptop.fullscreenSize = QSize(1920, 1080);
    }
    QVector<NvClientDisplay> displays{laptop};
    if (secondDisplay) {
        NvClientDisplay external{QRect(1512, 0, 1920, 1080), QSize(1920, 1080)};
        external.key = "uuid:fixture-external";
        external.name = "External fixture";
        displays.append(external);
    }
    return displays;
}
}
DisplayMonitor::DisplayMonitor(QObject* parent) : QObject(parent) { refresh(); }
DisplayMonitor::~DisplayMonitor() = default;
void DisplayMonitor::refresh()
{
    m_Displays = fixtureDisplays();
    m_Fingerprint = ClientDisplayProbe::fingerprint(m_Displays);
    m_Label = "Display fixture";
}
void DisplayMonitor::refreshLater(){}
namespace MacDisplayInfo {bool screensHaveSeparateSpaces(){return true;}}
namespace DecoderCaps {QSize cachedMaximum(const QString&){return QSize(8192,4320);} QSize probe(const QString&){return QSize(8192,4320);}}
class BrokerStub:public QObject {
 Q_OBJECT
public:
 Q_INVOKABLE void connectToHost(const QString&){}
 Q_INVOKABLE QVariantMap displaySetup(const QString&) {return {{"virtualModes",QStringList{"1920x1080","3024x1890"}},{"layoutChoice",0},{"virtualMode1","1920x1080"},{"virtualMode2","1920x1080"},{"scalingChoice",1},{"canMatchClient",true},{"matchClientSummary","One desktop, 3024 × 1898"}};}
};
class Probe:public QObject {
 Q_OBJECT
 QTemporaryDir temp;
 DisplaySetupController* controller=nullptr;
 BrokerStub broker;
 QQmlEngine* engine=nullptr;
 QObject* root=nullptr;
 QObject* dialog=nullptr;
private slots:
 void initTestCase(){
  QCoreApplication::setOrganizationName("FernwehQualification");
  QCoreApplication::setApplicationName("ResolutionProbe");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.path());
  controller=new DisplaySetupController(this);
  qmlRegisterSingletonInstance("DisplaySetup",1,0,"DisplaySetup",controller);
  qmlRegisterSingletonInstance("RemoteBroker",1,0,"RemoteBroker",&broker);
  engine=new QQmlEngine(this);
  QQmlComponent c(engine, QUrl::fromLocalFile(QFileInfo(QString::fromUtf8(__FILE__)).dir().absoluteFilePath("Fixture.qml")));
  root=c.create();
  QVERIFY2(root,qPrintable(c.errorString()));
  dialog=root->findChild<QObject*>("setup");QVERIFY(dialog);
 }
 void init(){standardDpi=false;secondDisplay=false;QSettings s;s.clear();s.sync();QMetaObject::invokeMethod(dialog,"openFor",Q_ARG(QVariant,QVariant("workstation.example.test")),Q_ARG(QVariant,QVariant("Workstation")),Q_ARG(QVariant,QVariant("")),Q_ARG(QVariant,QVariant(false)),Q_ARG(QVariant,QVariant(0)));QTest::qWait(50);}
 void looksLikeDropdownSaveReopen(){
  QObject* combo=nullptr;
  std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
   if(item->property("currentText").toString().contains("More space")) combo=item;
   for(auto* child:item->childItems()) walk(child);
  };
  walk(dialog->property("contentItem").value<QQuickItem*>());
  QVERIFY2(combo,"Size dropdown not found");
  QVERIFY(QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,1)));
  QCOMPARE(controller->monitors().first().toMap().value("sizeChoice").toString(),QString("looks-like"));
  QVERIFY(QMetaObject::invokeMethod(dialog,"save"));
  controller->begin("workstation.example.test","Workstation","");
  QCOMPARE(controller->monitors().first().toMap().value("sizeChoice").toString(),QString("looks-like"));
  QCOMPARE(controller->monitors().first().toMap().value("width").toInt(),1512);
  QCOMPARE(controller->monitors().first().toMap().value("height").toInt(),948);
 }
 void presetSaveReopen(){controller->setSize("uuid:fixture-retina","preset:1920x1080");QVERIFY(QMetaObject::invokeMethod(dialog,"save"));controller->begin("workstation.example.test","Workstation","");QCOMPARE(controller->monitors().first().toMap().value("width").toInt(),1920);QCOMPARE(controller->monitors().first().toMap().value("height").toInt(),1080);}
 void customSaveReopen(){QVERIFY(controller->setCustomSize("uuid:fixture-retina",1920,1200));controller->setScope("workstation");QVERIFY(QMetaObject::invokeMethod(dialog,"save"));controller->begin("workstation.example.test","Workstation","");QCOMPARE(controller->monitors().first().toMap().value("width").toInt(),1920);QCOMPARE(controller->monitors().first().toMap().value("height").toInt(),1200);QCOMPARE(controller->scope(),QString("workstation"));}

 void fixedLayoutVisibilityAndCaptures(){
  QVERIFY(dialog->setProperty("page",1));QTest::qWait(50);
  QSettings settings;
  RemoteStreamSetup::Capabilities caps;
  caps.known=true;caps.platform=RemoteStreamSetup::LinuxPlatform;
  caps.featureFlags=DisplayArrangement::Feature;
  caps.displayCapabilities=QString::fromUtf8(QJsonDocument(DisplayArrangement::Capabilities::fleetDefault().toJson()).toJson(QJsonDocument::Compact));
  auto* window=qobject_cast<QQuickWindow*>(root);QVERIFY(window);
  QObject* fixed=nullptr;
  std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
   if(item->property("text").toString()=="Remote desktop" && item->parent()->property("layoutChoice").isValid()) fixed=item->parent();
   for(auto* child:item->childItems()) walk(child);
  };
  walk(dialog->property("contentItem").value<QQuickItem*>());
  auto* section=qobject_cast<QQuickItem*>(fixed);QVERIFY(section);
  auto capture=[&](const QString& name){
   const QString directory=qEnvironmentVariable("PLANK_DISPLAY_SETUP_CAPTURE_DIR");
   if(directory.isEmpty())return;
   QVERIFY(QDir().mkpath(directory));QTest::qWait(150);
   QVERIFY(window->grabWindow().save(directory+"/"+name+".png"));
  };
  RemoteStreamSetup::saveCapabilities(settings,"workstation.example.test",caps);settings.sync();
  controller->begin("workstation.example.test","Workstation","");
  QCOMPARE(controller->fixedLayoutRequired(),false);
  QCOMPARE(section->isVisible(),false);
  capture("modern-linux");
  // Choosing software encoding must not bring the duplicate controls back.
  auto stream=RemoteStreamSetup::builtInDefaults(RemoteStreamSetup::LinuxPlatform);
  stream.mode=RemoteStreamSetup::Custom;stream.videoProfile=StreamingPreferences::PLANK_PROFILE_H264_10BIT_444;
  QVERIFY(RemoteStreamSetup::saveHost(settings,"workstation.example.test",stream));settings.sync();
  controller->begin("workstation.example.test","Workstation","");
  QCOMPARE(controller->fixedLayoutRequired(),false);QCOMPARE(section->isVisible(),false);
  caps.featureFlags=0;caps.displayCapabilities.clear();
  RemoteStreamSetup::saveCapabilities(settings,"workstation.example.test",caps);settings.sync();
  controller->begin("workstation.example.test","Workstation","");
  QCOMPARE(controller->fixedLayoutRequired(),true);QCOMPARE(section->isVisible(),true);
  capture("older-linux");
  caps.platform=RemoteStreamSetup::MacPlatform;
  RemoteStreamSetup::saveCapabilities(settings,"workstation.example.test",caps);settings.sync();
  controller->begin("workstation.example.test","Workstation","");
  QCOMPARE(controller->fixedLayoutRequired(),true);QCOMPARE(section->isVisible(),true);
  capture("mac-host");
  controller->begin("unknown.example.test","Workstation","");
  QCOMPARE(controller->fixedLayoutRequired(),false);QCOMPARE(section->isVisible(),false);
 }

 void staleServerInfoFlagsKeepAuthenticatedDisplayCapabilities(){
  QSettings settings;
  RemoteStreamSetup::Capabilities caps;
  caps.known=true;caps.platform=RemoteStreamSetup::LinuxPlatform;
  caps.featureFlags=0x76c7ffff; // Before-sign-in snapshot without arrangement bit.
  caps.displayCapabilities=QString::fromUtf8(QJsonDocument(DisplayArrangement::Capabilities::fleetDefault().toJson()).toJson(QJsonDocument::Compact));
  RemoteStreamSetup::saveCapabilities(settings,"workstation.example.test",caps);settings.sync();
  controller->begin("workstation.example.test","Workstation","");
  QCOMPARE(controller->fixedLayoutRequired(),false);
  QCOMPARE(controller->hostKind(),QString("arrangement"));
  controller->setSize("uuid:fixture-retina","preset:1920x1080");
  QCOMPARE(controller->monitors().first().toMap().value("width").toInt(),1920);
  QCOMPARE(controller->monitors().first().toMap().value("height").toInt(),1080);
 }
 void advancedFocusedEditsSaveAtomically(){
  QVERIFY(dialog->setProperty("page",1));QTest::qWait(50);
  QList<QObject*> fields;
  std::function<void(QQuickItem*)> walk=[&](QQuickItem* item){
   if(item->property("validator").isValid() && item->property("text").isValid()) fields<<item;
   for(auto* child:item->childItems())walk(child);
  };walk(dialog->property("contentItem").value<QQuickItem*>());
  QCOMPARE(fields.size(),4);
  fields[0]->setProperty("text","1920");fields[1]->setProperty("text","1080");
  qobject_cast<QQuickItem*>(fields[1])->forceActiveFocus();
  QMetaObject::invokeMethod(fields[1],"textEdited");
  QVERIFY(QMetaObject::invokeMethod(dialog,"save"));
  controller->begin("workstation.example.test","Workstation","");
  QCOMPARE(controller->monitors().first().toMap().value("width").toInt(),1920);
  QCOMPARE(controller->monitors().first().toMap().value("height").toInt(),1080);
 }
 void invalidGeometryDoesNotPartiallySave(){
  auto before=controller->monitors();
  QVERIFY(!controller->applyGeometry({QVariantMap{{"key","uuid:fixture-retina"},{"width",1920},{"height",1079},{"x",0},{"y",0}}}));
  QCOMPARE(controller->monitors(),before);
  QSettings settings;DisplayProfile::Profile saved;
  QVERIFY(!DisplayProfile::loadGlobal(settings,controller->fingerprint(),saved));
 }
 void compatibilitySaveDoesNotWriteGlobalProfile(){
  QSettings settings;RemoteStreamSetup::Capabilities caps;caps.known=true;caps.platform=RemoteStreamSetup::LinuxPlatform;
  RemoteStreamSetup::saveCapabilities(settings,"workstation.example.test",caps);settings.sync();
  QMetaObject::invokeMethod(dialog,"openFor",Q_ARG(QVariant,QVariant("workstation.example.test")),Q_ARG(QVariant,QVariant("Workstation")),Q_ARG(QVariant,QVariant("")),Q_ARG(QVariant,QVariant(false)),Q_ARG(QVariant,QVariant(0)));
  QCOMPARE(dialog->property("compatibility").toBool(),true);
  QVERIFY(QMetaObject::invokeMethod(dialog,"save"));
  auto setup=RemoteDisplaySetup::load(settings,"workstation.example.test");
  QCOMPARE(setup.configured,true);
  QCOMPARE(setup.hostLayout,RemoteDisplaySetup::layoutForChoice(0));
  DisplayProfile::Profile global;
  QVERIFY(!DisplayProfile::loadGlobal(settings,controller->fingerprint(),global));
 }
 void cancelDiscardsUnsavedEdits(){
  controller->setSize("uuid:fixture-retina","looks-like");
  QVERIFY(QMetaObject::invokeMethod(dialog,"close"));QTest::qWait(50);
  QSettings settings;DisplayProfile::Profile global;
  QVERIFY(!DisplayProfile::loadGlobal(settings,controller->fingerprint(),global));
 }
 void compactViewportKeepsSaveVisible(){
  auto* window=qobject_cast<QQuickWindow*>(root);QVERIFY(window);
  controller->setSize("uuid:fixture-retina","looks-like");
  window->resize(900,700);QTest::qWait(100);
  QVERIFY(dialog->property("height").toReal() <= 660);
  const QString directory=qEnvironmentVariable("PLANK_DISPLAY_SETUP_CAPTURE_DIR");
  if(!directory.isEmpty()){
   QVERIFY(QDir().mkpath(directory));QVERIFY(window->grabWindow().save(directory+"/basic-retina.png"));
   dialog->setProperty("page",1);QTest::qWait(100);QVERIFY(window->grabWindow().save(directory+"/advanced-compact.png"));
  }
  window->resize(1100,1000);
 }
 void modernHostRefusesFixedLayoutWrites(){
  QSettings settings;RemoteStreamSetup::Capabilities caps;caps.known=true;caps.platform=RemoteStreamSetup::LinuxPlatform;
  caps.featureFlags=DisplayArrangement::Feature;
  caps.displayCapabilities=QString::fromUtf8(QJsonDocument(DisplayArrangement::Capabilities::fleetDefault().toJson()).toJson(QJsonDocument::Compact));
  RemoteStreamSetup::saveCapabilities(settings,"workstation.example.test",caps);settings.sync();
  QVERIFY(!controller->useFixedLayout("workstation.example.test",2,"1920x1080","1920x1080",1));
  QCOMPARE(DisplayProfile::hostMode(settings,"workstation.example.test"),DisplayProfile::HostMode::Follow);
 }
 void standardDpiDoesNotOfferRetinaSizing(){
  standardDpi=true;controller->begin("workstation.example.test","Workstation","");
  const auto monitor=controller->monitors().first().toMap();
  QCOMPARE(monitor.value("highDpi").toBool(),false);
  QCOMPARE(monitor.value("width").toInt(),1920);
  for(const auto& option:monitor.value("sizeOptions").toList())
   QVERIFY(option.toMap().value("value").toString() != "looks-like");
 }
 void multiScreenSelectionAndPrimaryPersist(){
  secondDisplay=true;controller->begin("workstation.example.test","Workstation","");
  controller->setMonitorOn("uuid:fixture-retina",false);
  controller->setPrimary("uuid:fixture-external");
  QVERIFY(QMetaObject::invokeMethod(dialog,"save"));
  controller->begin("workstation.example.test","Workstation","");
  QCOMPARE(controller->monitors().size(),2);
  QCOMPARE(controller->monitors().first().toMap().value("on").toBool(),false);
  QCOMPARE(controller->monitors().last().toMap().value("primary").toBool(),true);
  QCOMPARE(controller->monitors().last().toMap().value("included").toBool(),true);
 }
 void invalidFormKeepsEditsAndCanBeCorrected(){
  dialog->setProperty("page",1);QTest::qWait(50);
  QList<QQuickItem*> fields;
  std::function<void(QQuickItem*)> walk=[&](QQuickItem* item){
   if(item->property("validator").isValid() && item->property("text").isValid())fields<<item;
   for(auto* child:item->childItems())walk(child);
  };walk(dialog->property("contentItem").value<QQuickItem*>());
  QCOMPARE(fields.size(),4);
  fields[0]->setProperty("text","1920");fields[1]->setProperty("text","1079");
  QMetaObject::invokeMethod(fields[1],"textEdited");
  QVERIFY(QMetaObject::invokeMethod(dialog,"save"));
  QCOMPARE(dialog->property("visible").toBool(),true);
  QCOMPARE(fields[1]->property("text").toString(),QString("1079"));
  QCOMPARE(controller->monitors().first().toMap().value("width").toInt(),3024);
  fields[1]->setProperty("text","1080");
  QVERIFY(QMetaObject::invokeMethod(dialog,"save"));
  QTRY_COMPARE(dialog->property("visible").toBool(),false);
  controller->begin("workstation.example.test","Workstation","");
  QCOMPARE(controller->monitors().first().toMap().value("height").toInt(),1080);
 }
 void compatibilityFixedResolutionSavesWithMainButton(){
  QSettings settings;RemoteStreamSetup::Capabilities caps;caps.known=true;caps.platform=RemoteStreamSetup::LinuxPlatform;
  RemoteStreamSetup::saveCapabilities(settings,"workstation.example.test",caps);settings.sync();
  QMetaObject::invokeMethod(dialog,"openFor",Q_ARG(QVariant,QVariant("workstation.example.test")),Q_ARG(QVariant,QVariant("Workstation")),Q_ARG(QVariant,QVariant("")),Q_ARG(QVariant,QVariant(false)),Q_ARG(QVariant,QVariant(0)));
  QQuickItem* layout=nullptr;
  std::function<void(QQuickItem*)> walk=[&](QQuickItem* item){
   if(item->property("currentText").toString()=="Match this computer")layout=item;
   for(auto* child:item->childItems())walk(child);
  };walk(dialog->property("contentItem").value<QQuickItem*>());QVERIFY(layout);
  layout->setProperty("currentIndex",2);
  QVERIFY(QMetaObject::invokeMethod(dialog,"save"));
  const auto saved=RemoteDisplaySetup::load(settings,"workstation.example.test");
  QCOMPARE(saved.hostLayout,RemoteDisplaySetup::layoutForChoice(2));
  QCOMPARE(saved.virtualMode1,QString("1920x1080"));
 }
 void geometryBatchRejectsInvalidSecondScreenAtomically(){
  secondDisplay=true;controller->begin("workstation.example.test","Workstation","");
  const auto before=controller->monitors();
  QVERIFY(!controller->applyGeometry({
   QVariantMap{{"key","uuid:fixture-retina"},{"width",1920},{"height",1080},{"x",0},{"y",0}},
   QVariantMap{{"key","uuid:fixture-external"},{"width",1920},{"height",1079},{"x",1920},{"y",0}}}));
  QCOMPARE(controller->monitors(),before);
 }
 void cleanupTestCase(){delete root;delete engine;}
};
int main(int argc,char**argv){QGuiApplication app(argc,argv);Probe p;return QTest::qExec(&p,argc,argv);}
#include "test_displaysetupcontroller.moc"
