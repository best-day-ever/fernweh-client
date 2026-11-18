#pragma once
#include <QJsonObject>
#include <QStringList>
#include <atomic>
#include <memory>

class DiagnosticNetworkProbe {
public:
    struct Endpoint { QString host; quint16 port; QStringList pins; };
    static QJsonObject run(const Endpoint& endpoint, const QJsonObject& grant,
                          const std::shared_ptr<std::atomic_bool>& cancelled);
private:
    friend class DiagnosticNetworkProbeTest;
    static QJsonObject runImpl(const Endpoint&, const QJsonObject&,
                              const std::shared_ptr<std::atomic_bool>&, int durationMs);
    static QJsonObject transferForTest(const Endpoint&, const QJsonObject&, bool upload, int maximumMs);
    static QByteArray packetForTest(const QByteArray&, const QByteArray&, quint32, quint64, int);
};
