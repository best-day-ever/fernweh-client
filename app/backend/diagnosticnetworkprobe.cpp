#include "diagnosticnetworkprobe.h"
#include "plankbroker.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostInfo>
#include <QMessageAuthenticationCode>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
#include <array>
#include <limits>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#else
#include <sys/resource.h>
#ifdef Q_OS_MACOS
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#endif
#endif

namespace {
constexpr int DurationMs=120000, GraceMs=2000, MaximumPackets=8192;
constexpr quint64 TransferBytes=250000000;
constexpr int TransferMs=10000, RateKbps=200000;
int phase(int ms) { return ms<10000?0:ms<20000?1:ms<32000?2:ms<50000?3:ms<62000?4:ms<80000?5:ms<100000?6:7; }
bool sameMac(const QByteArray& a,const QByteArray& b) {
    if (a.size()!=b.size()) return false;
    volatile unsigned char difference=0;
    for (qsizetype i=0;i<a.size();++i) difference=static_cast<unsigned char>(difference | static_cast<unsigned char>(a[i]^b[i]));
    return difference==0;
}
QByteArray sign(const QByteArray& data,const QByteArray& secret) {
    return QMessageAuthenticationCode::hash(data,secret,QCryptographicHash::Sha256);
}
QByteArray packet(const QByteArray& id,const QByteArray& secret,quint32 sequence,quint64 sent,int bytes) {
    QByteArray result(bytes,'\0'); memcpy(result.data(),"FWNP",4); result[4]=1;
    result.replace(8,16,id); qToBigEndian(sequence,reinterpret_cast<uchar*>(result.data()+24));
    qToBigEndian(sent,reinterpret_cast<uchar*>(result.data()+28));
    result.replace(bytes-32,32,sign(result.left(bytes-32),secret)); return result;
}
QJsonValue optionalCounter(quint64 value,bool known) { return known?QJsonValue(qint64(value)):QJsonValue(QJsonValue::Null); }
qint64 processCpuUs() {
#ifdef Q_OS_WIN
    FILETIME created,exited,kernel,user;
    if (!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) return -1;
    ULARGE_INTEGER k,u; k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
    u.LowPart=user.dwLowDateTime; u.HighPart=user.dwHighDateTime;
    return qint64((k.QuadPart+u.QuadPart)/10);
#else
    rusage usage{}; if (getrusage(RUSAGE_SELF,&usage)!=0) return -1;
    return qint64(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*1000000+usage.ru_utime.tv_usec+usage.ru_stime.tv_usec;
#endif
}
QJsonObject link(const QNetworkInterface& nic) {
    QJsonObject result{{"known",nic.isValid()},{"interface_type",nic.isValid()?int(nic.type()):-1},
        {"mtu",qMax(0,nic.maximumTransmissionUnit())},{"point_to_point",bool(nic.flags()&QNetworkInterface::IsPointToPoint)},
        {"receive_link_bps",QJsonValue::Null},{"transmit_link_bps",QJsonValue::Null},
        {"receive_errors",QJsonValue::Null},{"transmit_errors",QJsonValue::Null},
        {"receive_discards",QJsonValue::Null},{"transmit_discards",QJsonValue::Null}};
#ifdef Q_OS_WIN
    MIB_IF_ROW2 row{}; row.InterfaceIndex=ULONG(nic.index());
    if (nic.isValid() && GetIfEntry2(&row)==NO_ERROR) {
        result["receive_link_bps"]=optionalCounter(row.ReceiveLinkSpeed,row.ReceiveLinkSpeed>0);
        result["transmit_link_bps"]=optionalCounter(row.TransmitLinkSpeed,row.TransmitLinkSpeed>0);
        result["receive_errors"]=qint64(row.InErrors); result["transmit_errors"]=qint64(row.OutErrors);
        result["receive_discards"]=qint64(row.InDiscards); result["transmit_discards"]=qint64(row.OutDiscards);
    }
#elif defined(Q_OS_MACOS)
    ifaddrs* addresses=nullptr;
    if (getifaddrs(&addresses)==0) {
        for (auto item=addresses;item;item=item->ifa_next) if (item->ifa_addr && item->ifa_data &&
                item->ifa_addr->sa_family==AF_LINK && QString::fromUtf8(item->ifa_name)==nic.name()) {
            const auto* data=static_cast<const if_data*>(item->ifa_data);
            const bool speed=data->ifi_baudrate>0 && data->ifi_baudrate<std::numeric_limits<quint32>::max();
            result["receive_link_bps"]=optionalCounter(data->ifi_baudrate,speed);
            result["transmit_link_bps"]=optionalCounter(data->ifi_baudrate,speed);
            result["receive_errors"]=qint64(data->ifi_ierrors); result["transmit_errors"]=qint64(data->ifi_oerrors);
            result["receive_discards"]=qint64(data->ifi_iqdrops); break;
        }
        freeifaddrs(addresses);
    }
#endif
    return result;
}
QHostAddress resolve(const QString& host,const std::shared_ptr<std::atomic_bool>& cancelled) {
    QHostAddress result(host);
    if (result.protocol()==QAbstractSocket::IPv4Protocol) return result;
    result.clear(); QEventLoop loop; QTimer deadline,cancel; deadline.setSingleShot(true);
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    QObject::connect(&cancel,&QTimer::timeout,&loop,[&] { if (cancelled->load()) loop.quit(); });
    const int lookup=QHostInfo::lookupHost(host,&loop,[&](const QHostInfo& info) {
        for (const auto& a:info.addresses()) if (a.protocol()==QAbstractSocket::IPv4Protocol) { result=a; break; }
        loop.quit();
    });
    deadline.start(3000); cancel.start(25); loop.exec(); QHostInfo::abortHostLookup(lookup); return result;
}

// A generated, paced body. It never reads an employee file or keeps test data.
class ZeroBody : public QIODevice {
public:
    explicit ZeroBody(QObject* parent):QIODevice(parent) {
        open(QIODevice::ReadOnly); clock.start(); timer.setInterval(10); timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&timer,&QTimer::timeout,this,[this] {
            const auto now=clock.elapsed(); if (now-lastTick>30) ++lateTicks; lastTick=now;
            emit readyRead();
        }); timer.start();
    }
    bool isSequential() const override { return true; }
    bool seek(qint64) override { return false; } // one-use stream; never replay a transfer.
    bool reset() override { return false; }
    bool atEnd() const override { return sent>=TransferBytes; }
    qint64 bytesAvailable() const override {
        const auto now=clock.nsecsElapsed()/1000;
        credit=qMin<quint64>(250000,credit+quint64(now-lastCreditUs)*quint64(RateKbps)/8000);
        lastCreditUs=now;
        return qint64(qMin(TransferBytes-sent,credit))+QIODevice::bytesAvailable();
    }
    quint32 lateTicks=0;
protected:
    qint64 readData(char* out,qint64 maximum) override {
        bytesAvailable();
        const auto count=qMin<qint64>(qMin<quint64>(65536,TransferBytes-sent),qMin<qint64>(maximum,qint64(credit)));
        if (count>0) { memset(out,0,size_t(count)); sent+=quint64(count); credit-=quint64(count); } return count;
    }
    qint64 writeData(const char*,qint64) override { return -1; }
private:
    quint64 sent=0; qint64 lastTick=0; QElapsedTimer clock; QTimer timer;
    mutable quint64 credit=65536; mutable qint64 lastCreditUs=0;
};

class Transfer {
public:
    Transfer(const DiagnosticNetworkProbe::Endpoint& endpoint,const QJsonObject& grant,const QByteArray& secret,
             bool upload,int elapsed):isUpload(upload),startMs(elapsed) {
        clock.start();
        QUrl url; url.setScheme("https"); url.setHost(endpoint.host); url.setPort(endpoint.port);
        const QString direction=upload?QStringLiteral("upload"):QStringLiteral("download");
        const QString id=grant.value("probe_id").toString();
        url.setPath("/v1/diagnostics/network/throughput/"+id+"/"+direction);
        QNetworkRequest request(url); request.setPriority(QNetworkRequest::LowPriority);
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute,false);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::AlwaysNetwork);
        request.setAttribute(QNetworkRequest::CacheSaveControlAttribute,false);
        request.setRawHeader("Cache-Control","no-store"); request.setRawHeader("Accept-Encoding","identity");
        const QByteArray message="FWNB1\n"+id.toLatin1()+"\n"+direction.toLatin1();
        request.setRawHeader("X-Fernweh-Probe-Proof",sign(message,secret).toHex());
        QSslConfiguration tls=QSslConfiguration::defaultConfiguration(); tls.setProtocol(QSsl::TlsV1_3OrLater);
        tls.setCaCertificates({}); tls.setPeerVerifyMode(QSslSocket::VerifyPeer); request.setSslConfiguration(tls);
        const auto pins=PlankBroker::normalizePins(endpoint.pins);
        const auto matches=[pins](const QSslConfiguration& config) {
            const auto certificate=config.peerCertificate();
            return !certificate.isNull() && PlankBroker::spkiPinMatches(certificate.toDer(),pins);
        };
        QObject::connect(&manager,&QNetworkAccessManager::sslErrors,&manager,[this,matches](QNetworkReply* r,const QList<QSslError>& errors) {
            if (matches(r->sslConfiguration())) r->ignoreSslErrors(errors); else { rejected=true; r->abort(); }
        });
        QObject::connect(&manager,&QNetworkAccessManager::encrypted,&manager,[this,matches](QNetworkReply* r) {
            verified=matches(r->sslConfiguration()) && r->sslConfiguration().sessionProtocol()==QSsl::TlsV1_3;
            if (!verified) { rejected=true; r->abort(); }
        });
        if (upload) {
            body=std::make_unique<ZeroBody>(&manager);
            request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute,true);
            request.setHeader(QNetworkRequest::ContentLengthHeader,qint64(TransferBytes));
            request.setHeader(QNetworkRequest::ContentTypeHeader,"application/octet-stream");
            reply=manager.post(request,body.get());
        } else reply=manager.get(request);
        reply->setReadBufferSize(upload?65536:1048576);
        QObject::connect(reply,&QNetworkReply::readyRead,&manager,[this] { drain(); });
        QObject::connect(reply,&QNetworkReply::uploadProgress,&manager,[this](qint64 sent,qint64) {
            if (isUpload && verified && sent>=0) { bytes=quint64(sent); firstByte=firstByte<0?clock.elapsed():firstByte; }
        });
        QObject::connect(reply,&QNetworkReply::finished,&manager,[this] { drain(); done=true; duration=int(clock.elapsed()); });
    }
    ~Transfer() {
        if (reply) { QObject::disconnect(reply,nullptr,&manager,nullptr); reply->abort(); }
        QObject::disconnect(&manager,nullptr,&manager,nullptr);
    }
    void drain() {
        if (!reply || !verified || rejected) return;
        if (isUpload) {
            response+=reply->read(65537-response.size());
            if (response.size()>65536) { rejected=true; reply->abort(); }
        } else {
            // Bound work per callback so UDP timing and cancellation keep running.
            const auto data=reply->read(qMin<quint64>(1048576,TransferBytes+1-bytes));
            if (!data.isEmpty()) {
                firstByte=firstByte<0?clock.elapsed():firstByte; bytes+=quint64(data.size());
                bins[qMin(12,int(clock.elapsed()/1000))]+=quint64(data.size());
            }
            if (bytes>TransferBytes) { rejected=true; reply->abort(); }
        }
    }
    void tick() {
        if (!done && clock.elapsed()>=12000) { timedOut=true; reply->abort(); }
        if (!isUpload) drain();
    }
    void abort() { if (reply && !done) reply->abort(); }
    QJsonObject result() const {
        QJsonArray values; for (auto value:bins) values.append(qint64(value));
        return {{"attempted",true},{"tls_verified",verified&&!rejected},{"bytes",qint64(bytes)},
            {"duration_ms",duration?duration:int(clock.elapsed())},{"first_byte_ms",firstByte<0?QJsonValue(QJsonValue::Null):QJsonValue(int(firstByte))},
            {"start_ms",startMs},{"timed_out",timedOut},{"error_code",reply?int(reply->error()):0},
            {"http_status",reply?reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt():0},
            {"producer_late_ticks",body?int(body->lateTicks):0},{"bins",values}};
    }
private:
    bool isUpload,verified=false,rejected=false,done=false,timedOut=false;
    int startMs=0,duration=0; qint64 firstByte=-1; quint64 bytes=0;
    std::array<quint64,13> bins{}; QElapsedTimer clock;
    QNetworkAccessManager manager; std::unique_ptr<ZeroBody> body;
    QNetworkReply* reply=nullptr; QByteArray response;
};
}

QJsonObject DiagnosticNetworkProbe::run(const Endpoint& endpoint,const QJsonObject& grant,const std::shared_ptr<std::atomic_bool>& cancelled) {
    return runImpl(endpoint,grant,cancelled,DurationMs);
}

QJsonObject DiagnosticNetworkProbe::transferForTest(const Endpoint& endpoint,const QJsonObject& grant,bool upload,int maximumMs) {
    Transfer transfer(endpoint,grant,QByteArray::fromHex(grant.value("probe_secret").toString().toLatin1()),upload,0);
    QElapsedTimer elapsed; elapsed.start();
    while (elapsed.elapsed()<maximumMs) { QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents,1); transfer.tick(); QThread::msleep(1); }
    transfer.abort(); return transfer.result();
}
QByteArray DiagnosticNetworkProbe::packetForTest(const QByteArray& id,const QByteArray& key,quint32 sequence,quint64 stamp,int bytes) {
    return packet(id,key,sequence,stamp,bytes);
}

QJsonObject DiagnosticNetworkProbe::runImpl(const Endpoint& endpoint,const QJsonObject& grant,
                                          const std::shared_ptr<std::atomic_bool>& cancelled,int durationMs) {
    QJsonObject result{{"outcome","resolve_failed"},{"duration_ms",0},{"sent",0},{"received",0},
        {"duplicates",0},{"reordered",0},{"send_errors",0},{"late_sends",0},{"invalid_replies",0},{"last_socket_error",0},
        {"target_ipv4",QJsonValue::Null},{"jitter_us",0},{"max_server_processing_us",0},
        {"rtt_us",QJsonArray()},{"rtt_phases",QJsonArray()},{"bins",QJsonArray()},{"packet_sizes",QJsonArray()},
        {"dns_ms",0},{"api_ms",grant.value("api_ms").toInt()},{"max_scheduler_gap_ms",0},{"upload",QJsonValue::Null},{"download",QJsonValue::Null},
        {"local_link_before",link({})},{"local_link_after",link({})},
        {"cpu_core_percent",QJsonValue::Null},{"logical_cpus",qMax(1,QThread::idealThreadCount())}};
    const auto id=QByteArray::fromHex(grant.value("probe_id").toString().toLatin1());
    const auto secret=QByteArray::fromHex(grant.value("probe_secret").toString().toLatin1());
    const auto port=grant.value("port").toInt();
    if (grant.value("schema").toInt()!=1 || id.size()!=16 || secret.size()!=32 || port<1024 || port>65535 ||
        grant.value("duration_ms").toInt()!=DurationMs || grant.value("bandwidth_rate_kbps").toInt()!=RateKbps ||
        grant.value("bandwidth_ms").toInt()!=TransferMs || grant.value("bandwidth_bytes").toInteger()!=qint64(TransferBytes)) return result;
    QElapsedTimer dns; dns.start(); const auto target=resolve(endpoint.host,cancelled); result["dns_ms"]=int(dns.elapsed());
    if (cancelled->load()) { result["outcome"]="cancelled"; return result; }
    if (target.isNull()) return result;
    result["target_ipv4"]=target.toString();
    QUdpSocket socket;
    if (!socket.bind(QHostAddress::AnyIPv4,0,QUdpSocket::DontShareAddress)) {
        result["outcome"]="socket_failed"; result["last_socket_error"]=int(socket.error()); return result;
    }
    socket.connectToHost(target,quint16(port));
    QNetworkInterface nic;
    for (const auto& candidate:QNetworkInterface::allInterfaces())
        for (const auto& address:candidate.addressEntries()) if (address.ip()==socket.localAddress()) nic=candidate;
    result["local_link_before"]=link(nic);
    const auto cpuStart=processCpuUs();
    QElapsedTimer clock; clock.start(); qint64 nextSend=0; quint64 previousLoop=0,maxGap=0;
    std::array<quint64,MaximumPackets> stamps{}; std::array<int,MaximumPackets> sizes{};
    std::array<bool,MaximumPackets> sent{},seen{};
    quint32 sequence=0,totalSent=0,received=0,duplicates=0,reordered=0,late=0,errors=0,invalid=0,maxSequence=0;
    quint64 maxProcessing=0; double jitter=0,peakJitter=0; qint64 previousTransit=0; bool haveTransit=false;
    QJsonArray rtts,phases; std::array<QJsonObject,123> bins;
    for (int i=0;i<int(bins.size());++i) bins[i]={{"second",i},{"phase",phase(i*1000)},{"sent",0},{"received",0},
        {"rtt_min_us",0},{"rtt_max_us",0},{"send_errors",0},{"late_sends",0},{"jitter_us",0}};
    std::unique_ptr<Transfer> upload,download;
    while (clock.elapsed()<durationMs+GraceMs && !cancelled->load()) {
        const int ms=int(clock.elapsed()); const quint64 us=quint64(clock.nsecsElapsed()/1000);
        maxGap=qMax(maxGap,us-previousLoop); previousLoop=us;
        const int p=phase(ms); auto& bin=bins[qMin(122,ms/1000)];
        if (durationMs==DurationMs && ms>=20000 && ms<22000 && !upload) upload=std::make_unique<Transfer>(endpoint,grant,secret,true,ms);
        if (durationMs==DurationMs && ms>=50000 && ms<52000 && !download) download=std::make_unique<Transfer>(endpoint,grant,secret,false,ms);
        if (upload) upload->tick(); if (download) download->tick();
        if (ms<durationMs && qint64(us)>=nextSend && sequence<MaximumPackets) {
            const qint64 period=p==0||p==6?50000:20000;
            if (qint64(us)-nextSend>period/2) {
                const auto missed=qMax<qint64>(1,(qint64(us)-nextSend)/period);
                late+=quint32(missed); bin["late_sends"]=bin["late_sends"].toInt()+int(missed);
            }
            const int bytes=p==6?std::array<int,3>{128,1200,1400}[sequence%3]:(p==2||p==4?1200:128);
            const auto data=packet(id,secret,sequence,us,bytes);
            if (socket.write(data)==data.size()) {
                sent[sequence]=true; stamps[sequence]=us; sizes[sequence]=bytes; ++totalSent;
                bin["sent"]=bin["sent"].toInt()+1;
            } else { ++errors; bin["send_errors"]=bin["send_errors"].toInt()+1; result["last_socket_error"]=int(socket.error()); }
            ++sequence; nextSend=qint64(us)+period;
        }
        for (int drained=0;drained<64 && socket.hasPendingDatagrams();++drained) {
            QByteArray data(1401,'\0'); const auto length=socket.read(data.data(),data.size()); data.resize(qMax<qint64>(0,length));
            if (length!=128&&length!=1200&&length!=1400) { ++invalid; continue; }
            if (data.left(4)!="FWNP" || data[4]!=1 || data[5]!=1 || data[6]!=0 || data[7]!=0 || data.mid(8,16)!=id ||
                !sameMac(sign(data.left(data.size()-32),secret),data.right(32))) { ++invalid; continue; }
            const auto index=qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data.constData()+24));
            if (index>=sequence || !sent[index] || sizes[index]!=length || qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(data.constData()+28))!=stamps[index]) { ++invalid; continue; }
            if (seen[index]) { ++duplicates; continue; }
            const auto rx=quint64(clock.nsecsElapsed()/1000);
            const auto serverRx=qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(data.constData()+36));
            const auto serverTx=qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(data.constData()+44));
            if (rx<stamps[index] || rx-stamps[index]>60000000 || serverTx<serverRx || serverTx-serverRx>60000000) { ++invalid; continue; }
            seen[index]=true; if (index<maxSequence) ++reordered; maxSequence=qMax(maxSequence,index);
            const auto rtt=quint32(rx-stamps[index]); rtts.append(qint64(rtt)); phases.append(phase(int(stamps[index]/1000)));
            const qint64 transit=qint64(rx)-qint64(serverTx);
            if (haveTransit) jitter+=(double(qAbs(transit-previousTransit))-jitter)/16.;
            previousTransit=transit; haveTransit=true; peakJitter=qMax(peakJitter,jitter);
            auto& rxBin=bins[qMin(122,int(rx/1000000))];
            rxBin["rtt_min_us"]=rxBin["received"].toInt()?qMin(rxBin["rtt_min_us"].toInt(),int(rtt)):int(rtt);
            rxBin["rtt_max_us"]=qMax(rxBin["rtt_max_us"].toInt(),int(rtt));
            rxBin["jitter_us"]=qMax(rxBin["jitter_us"].toInt(),int(qMin(jitter,60000000.)));
            rxBin["received"]=rxBin["received"].toInt()+1; ++received;
            maxProcessing=qMax(maxProcessing,serverTx-serverRx);
        }
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents,1);
        const bool loading=(upload && ms<32000)||(download && ms<62000);
        if (!socket.hasPendingDatagrams()) socket.waitForReadyRead(loading?1:qMax(1,qMin(20,int((nextSend-qint64(clock.nsecsElapsed()/1000))/1000))));
    }
    if (upload) { upload->abort(); result["upload"]=upload->result(); }
    if (download) { download->abort(); result["download"]=download->result(); }
    QJsonArray values; for (const auto& b:bins) values.append(b);
    const auto cpuEnd=processCpuUs();
    if (cpuStart>=0&&cpuEnd>=cpuStart) result["cpu_core_percent"]=double(cpuEnd-cpuStart)/qMax<qint64>(1,clock.elapsed()*1000)*100.;
    result["local_link_after"]=link(nic);
    result["max_scheduler_gap_ms"]=int(qMin<quint64>(180000,maxGap/1000));
    result["outcome"]=cancelled->load()?"cancelled":"complete"; result["duration_ms"]=int(clock.elapsed());
    result["sent"]=int(totalSent); result["received"]=int(received); result["duplicates"]=int(duplicates);
    result["reordered"]=int(reordered); result["send_errors"]=int(errors); result["late_sends"]=int(late);
    result["invalid_replies"]=int(qMin(invalid,quint32(1000000))); result["jitter_us"]=int(qMin(peakJitter,60000000.));
    result["max_server_processing_us"]=qint64(maxProcessing); result["rtt_us"]=rtts; result["rtt_phases"]=phases; result["bins"]=values;
    QJsonArray bySize;
    for (const int size: {128,1200,1400}) {
        int nSent=0,nReceived=0;
        for (quint32 i=0;i<sequence;++i) if (sizes[i]==size && sent[i]) { ++nSent; if (seen[i]) ++nReceived; }
        bySize.append(QJsonObject{{"bytes",size},{"sent",nSent},{"received",nReceived}});
    }
    result["packet_sizes"]=bySize;
    return result;
}
