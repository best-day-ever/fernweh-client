#include "backend/diagnosticnetworkprobe.h"
#include "backend/plankbroker.h"
#include "../plankbroker/tlsfixtures.h"
#include <QtTest>
#include <QUdpSocket>
#include <QSslServer>
#include <QSslSocket>
#include <QSslKey>
#include <QMessageAuthenticationCode>
#include <QJsonDocument>
#include <QFile>
#include <QtEndian>
#include <future>

namespace {
QJsonObject grant() {
    return {{"schema",1},{"request_id",QString(32,'b')},{"client_instance",QString(32,'c')},
        {"probe_id",QString(32,'a')},{"probe_secret",QString(64,'1')},{"port",4096},
        {"duration_ms",120000},{"bandwidth_rate_kbps",200000},{"bandwidth_ms",10000},{"bandwidth_bytes",250000000}};
}
class Echo {
public:
    explicit Echo(int drop=0,bool bad=false,bool reorder=false,int delay=0) {
        std::promise<quint16> ready; auto result=ready.get_future();
        thread=QThread::create([this,drop,bad,reorder,delay,ready=std::move(ready)]() mutable {
            QUdpSocket socket; if (!socket.bind(QHostAddress::LocalHost,0)) { ready.set_value(0); return; }
            ready.set_value(socket.localPort()); QElapsedTimer clock; clock.start(); QByteArray held; QHostAddress heldPeer; quint16 heldPort=0;
            const auto secret=QByteArray::fromHex(QString(64,'1').toLatin1());
            while (!stop.load()) {
                if (!socket.waitForReadyRead(10)) continue;
                while (!stop.load() && socket.hasPendingDatagrams()) {
                    QByteArray p(1401,'\0'); QHostAddress peer; quint16 port;
                    auto n=socket.readDatagram(p.data(),p.size(),&peer,&port); p.resize(int(n));
                    if (n<128) continue;
                    const auto seq=qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(p.constData()+24));
                    if (drop && seq%quint32(drop)==0) continue;
                    if (delay) QThread::msleep(delay);
                    p[5]=1; const auto now=quint64(clock.nsecsElapsed()/1000);
                    qToBigEndian(now,reinterpret_cast<uchar*>(p.data()+36)); qToBigEndian(now+1,reinterpret_cast<uchar*>(p.data()+44));
                    p.replace(p.size()-32,32,QMessageAuthenticationCode::hash(p.left(p.size()-32),secret,QCryptographicHash::Sha256));
                    if (bad) p[p.size()-1]=char(p[p.size()-1]^1);
                    if (reorder && seq==0) { held=p; heldPeer=peer; heldPort=port; continue; }
                    socket.writeDatagram(p,peer,port);
                    if (!held.isEmpty()) { socket.writeDatagram(held,heldPeer,heldPort); held.clear(); }
                    if (reorder && seq==2) socket.writeDatagram(p,peer,port);
                }
            }
        }); thread->start(); port=result.get();
    }
    ~Echo() { stop=true; thread->wait(); delete thread; }
    quint16 port=0;
private:
    QThread* thread=nullptr; std::atomic_bool stop{false};
};
}

class DiagnosticNetworkProbeTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(QSslSocket::setActiveBackend("openssl")); }
    void canonicalWireVectorMatchesTheBroker() {
        QFile file(QFINDTESTDATA("../../../../protocol/fixtures/network-diagnostics-packet.json")); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto fixture=QJsonDocument::fromJson(file.readAll()).object();
        const auto bytes=DiagnosticNetworkProbe::packetForTest(QByteArray::fromHex(fixture["probe_id"].toString().toLatin1()),
            QByteArray::fromHex(fixture["secret_hex"].toString().toLatin1()),7,123456,128);
        QCOMPARE(bytes.toHex(),fixture["request_hex"].toString().toLatin1());
    }
    void exactUdpReceiptHasIndependentCountsAndTiming() {
        Echo echo; QVERIFY(echo.port); auto g=grant(); g["port"]=echo.port;
        const auto result=DiagnosticNetworkProbe::runImpl({"127.0.0.1",443,{}},g,std::make_shared<std::atomic_bool>(false),600);
        QCOMPARE(result["outcome"].toString(),QString("complete")); QVERIFY(result["sent"].toInt()>=8);
        QCOMPARE(result["received"].toInt(),result["sent"].toInt()); QCOMPARE(result["invalid_replies"].toInt(),0);
        QCOMPARE(result["rtt_us"].toArray().size(),result["received"].toInt());
        QCOMPARE(result["rtt_phases"].toArray().size(),result["received"].toInt());
        QVERIFY(result["upload"].isNull()); // short fixtures cannot generate bandwidth traffic.
        const auto destination=qEnvironmentVariable("PLANK_NETWORK_FIXTURE_OUTPUT");
        if (!destination.isEmpty()) {
            QFile output(destination); QVERIFY(output.open(QIODevice::WriteOnly|QIODevice::Truncate));
            QVERIFY(output.setPermissions(QFile::ReadOwner|QFile::WriteOwner));
            output.write(QJsonDocument(result).toJson(QJsonDocument::Compact));
        }
    }
    void controlledLossDelayReorderAndDuplicatesRemainVisible() {
        Echo echo(5,false,true,5); auto g=grant(); g["port"]=echo.port;
        const auto r=DiagnosticNetworkProbe::runImpl({"127.0.0.1",443,{}},g,std::make_shared<std::atomic_bool>(false),700);
        QVERIFY(r["sent"].toInt()>r["received"].toInt());
        for (const auto value:r["rtt_us"].toArray()) QVERIFY(value.toInt()>=4000);
        Echo reordered(0,false,true); g["port"]=reordered.port;
        const auto order=DiagnosticNetworkProbe::runImpl({"127.0.0.1",443,{}},g,std::make_shared<std::atomic_bool>(false),400);
        QVERIFY(order["reordered"].toInt()>0); QVERIFY(order["duplicates"].toInt()>0);
        QCOMPARE(order["received"].toInt(),order["sent"].toInt());
    }
    void badMacNeverBecomesAValidReply() {
        Echo echo(0,true); auto g=grant(); g["port"]=echo.port;
        const auto r=DiagnosticNetworkProbe::runImpl({"127.0.0.1",443,{}},g,std::make_shared<std::atomic_bool>(false),300);
        QCOMPARE(r["received"].toInt(),0); QVERIFY(r["invalid_replies"].toInt()>0);
    }
    void cancellationDoesNotWaitForTheTwoMinuteBudget() {
        Echo echo; auto g=grant(); g["port"]=echo.port; auto cancel=std::make_shared<std::atomic_bool>(false);
        QTimer::singleShot(80,[cancel] { cancel->store(true); }); QElapsedTimer elapsed; elapsed.start();
        const auto r=DiagnosticNetworkProbe::run({"127.0.0.1",443,{}},g,cancel);
        QCOMPARE(r["outcome"].toString(),QString("cancelled")); QVERIFY(elapsed.elapsed()<2000);
    }
    void malformedRateCannotMakeTheClientGenerateAnUnboundedTest() {
        auto g=grant(); g["bandwidth_rate_kbps"]=1000000; QElapsedTimer elapsed; elapsed.start();
        const auto r=DiagnosticNetworkProbe::run({"127.0.0.1",443,{}},g,std::make_shared<std::atomic_bool>(false));
        QCOMPARE(r["sent"].toInt(),0); QVERIFY(elapsed.elapsed()<500);
    }
    void pinnedTransferAcceptsOnlyTheConfiguredTlsIdentity() {
        QSslServer server; auto config=QSslConfiguration::defaultConfiguration();
        const auto certificate=QByteArray::fromBase64(PlankTlsFixture::EcCertificateBase64);
        config.setLocalCertificate(QSslCertificate(certificate,QSsl::Der));
        config.setPrivateKey(QSslKey(QByteArray(PlankTlsFixture::EcTestOnlyPrivateKeyPem),QSsl::Ec,QSsl::Pem));
        config.setProtocol(QSsl::TlsV1_3); config.setPeerVerifyMode(QSslSocket::VerifyNone); server.setSslConfiguration(config);
        QVERIFY(server.listen(QHostAddress::LocalHost,0)); int requests=0; qint64 uploadBytes=0;
        connect(&server,&QSslServer::pendingConnectionAvailable,this,[&] {
            while (auto socket=server.nextPendingConnection()) {
                auto pending=std::make_shared<QByteArray>();auto responded=std::make_shared<bool>(false);
                connect(socket,&QTcpSocket::readyRead,socket,[&,socket,pending,responded] {
                    if (*responded) {socket->readAll();return;} *pending+=socket->readAll();
                    const int end=pending->indexOf("\r\n\r\n");if (end<0) return;
                    if (pending->startsWith("POST")) {
                        if (pending->size()-end-4<65536) return;
                        const auto data=pending->mid(end+4);for (const char c:data) QVERIFY(c==0);
                        uploadBytes=data.size();
                    }
                    *responded=true;++requests;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 16\r\nConnection: close\r\n\r\n0123456789abcdef"); socket->disconnectFromHost();
                }); connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            }
        });
        auto g=grant();
        const auto denied=DiagnosticNetworkProbe::transferForTest({"127.0.0.1",server.serverPort(),{QString(64,'0')}},g,false,300);
        QCOMPARE(requests,0); QCOMPARE(denied["bytes"].toInteger(),0); QVERIFY(!denied["tls_verified"].toBool());
        const auto accepted=DiagnosticNetworkProbe::transferForTest({"127.0.0.1",server.serverPort(),{PlankBroker::spkiSha256Hex(certificate)}},g,false,300);
        QCOMPARE(requests,1); QVERIFY(accepted["tls_verified"].toBool()); QCOMPARE(accepted["bytes"].toInteger(),16);
        const auto upload=DiagnosticNetworkProbe::transferForTest({"127.0.0.1",server.serverPort(),{PlankBroker::spkiSha256Hex(certificate)}},g,true,300);
        QVERIFY(upload["tls_verified"].toBool()); QCOMPARE(requests,2); QVERIFY(uploadBytes>=65536);
    }
};
QTEST_GUILESS_MAIN(DiagnosticNetworkProbeTest)
#include "test_diagnosticnetworkprobe.moc"
