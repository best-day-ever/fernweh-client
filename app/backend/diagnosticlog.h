#pragma once

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QString>

// The support collector receives only this bounded, redacted application log.
// No broker request can name a file, change logging verbosity or execute code.
class DiagnosticLog
{
public:
    static constexpr int MaxBytes = 64 * 1024;
    static constexpr int MaxLines = 512;
    static constexpr int MaxLineBytes = 1024;

    static void setContext(const QString& identity, bool enabled)
    {
        auto& s = state(); QMutexLocker lock(&s.mutex);
        if (s.identity != identity || !enabled) {
            s.lines.clear(); s.pending.clear(); s.bytes = 0; s.discarding = false;
        }
        s.identity = identity;
        s.enabled = enabled && !identity.isEmpty();
    }

    static QString redact(QString line)
    {
        // Drop the entire entry, including continuation chunks, for content
        // which could carry authentication, clipboard or file payloads.
        static const QRegularExpression sensitive(QStringLiteral(
            R"((?i)\b(authorization|bearer|cookie|password|passwd|passphrase|token|secret|otp|totp|assertion|credential|clipboard|payload|ap-req|private[ _-]?key|encryption[ _-]?key|api[ _-]?key|access[ _-]?key|refresh[ _-]?key)\b)"));
        if (sensitive.match(line).hasMatch()) return QStringLiteral("[sensitive log entry omitted]");
        static const QRegularExpression url(QStringLiteral(R"((?i)\b[a-z][a-z0-9+.-]*://[^\s<>]+)"));
        static const QRegularExpression email(QStringLiteral(R"((?i)\b[a-z0-9._%+-]+@[a-z0-9.-]+\.[a-z]{2,}\b)"));
        static const QRegularExpression address(QStringLiteral(R"((?<![\w.])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?::[0-9]{1,5})?(?![\w.])|\[[0-9a-fA-F:.%]+:[0-9a-fA-F:.%]*\](?::[0-9]+)?)"));
        static const QRegularExpression path(QStringLiteral(R"((?:[A-Za-z]:[\\/]|\\\\)[^\s<>"']+|(?:^|[\s"'=])(?:~?/)[^\s<>"']+)"));
        static const QRegularExpression hex(QStringLiteral(R"((?i)\b[0-9a-f]{32,}\b)"));
        static const QRegularExpression encoded(QStringLiteral(R"([A-Za-z0-9+/=_-]{48,})"));
        static const QRegularExpression control(QStringLiteral("[\\x00-\\x08\\x0b-\\x1f\\x7f]"));
        line.replace(url, QStringLiteral("[endpoint]"));
        line.replace(email, QStringLiteral("[account]"));
        line.replace(address, QStringLiteral("[address]"));
        line.replace(path, QStringLiteral(" [path]"));
        line.replace(hex, QStringLiteral("[identifier]"));
        line.replace(encoded, QStringLiteral("[encoded data]"));
        line.replace(control, QStringLiteral(" "));
        // Apply the byte bound in linear time without splitting UTF-8.
        const QByteArray bytes = line.toUtf8();
        int end = qMin(int(bytes.size()), MaxLineBytes);
        if (end < bytes.size()) while (end > 0 && (static_cast<unsigned char>(bytes[end]) & 0xc0) == 0x80) --end;
        return QString::fromUtf8(bytes.constData(), end);
    }

    static void append(const QString& chunk)
    {
        auto& s = state(); QMutexLocker lock(&s.mutex);
        if (!s.enabled) return;
        // Join library continuation fragments before redacting. Never retain
        // or repeatedly process an unbounded unterminated line.
        for (const QChar c : chunk) {
            if (c == QLatin1Char('\n')) {
                const QByteArray line = (s.discarding ? QStringLiteral("[oversized log entry omitted]") : redact(s.pending)).toUtf8() + '\n';
                s.lines.append(line); s.bytes += line.size();
                s.pending.clear(); s.discarding = false;
                while (s.bytes > MaxBytes || s.lines.size() > MaxLines) s.bytes -= s.lines.takeFirst().size();
            } else if (!s.discarding) {
                if (s.pending.size() < 8192) s.pending.append(c);
                else { s.pending.clear(); s.discarding = true; }
            }
        }
    }

    static QString tail()
    {
        auto& s = state(); QMutexLocker lock(&s.mutex);
        QByteArray bytes; bytes.reserve(s.bytes);
        for (const auto& line : s.lines) bytes.append(line);
        return QString::fromUtf8(bytes);
    }

private:
    struct State {
        QMutex mutex;
        QString identity, pending;
        QList<QByteArray> lines;
        int bytes = 0;
        bool enabled = false, discarding = false;
    };
    static State& state() { static State s; return s; }
};
