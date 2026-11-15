#include <QtTest>
#include <QFile>
#include <QtEndian>

class WindowsResourcesTest : public QObject
{
    Q_OBJECT
private slots:
    void packagedShadersAreValidDxbc_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto* name : {"d3d11_vertex.fxc", "d3d11_overlay_pixel.fxc",
                                 "d3d11_yuv420_pixel.fxc", "d3d11_ayuv_pixel.fxc",
                                 "d3d11_y410_pixel.fxc"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void packagedShadersAreValidDxbc()
    {
        QFETCH(QString, name);
        QFile file(QStringLiteral(":/data/") + name);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        const auto bytes = file.readAll();
        QVERIFY(bytes.size() >= 32);
        QCOMPARE(bytes.first(4), QByteArray("DXBC"));
        const auto word = [&bytes](qsizetype offset) {
            return qFromLittleEndian<quint32>(bytes.constData() + offset);
        };
        QCOMPARE(word(24), quint32(bytes.size()));
        const auto count = word(28);
        QVERIFY(count > 0);
        QVERIFY(count <= quint32((bytes.size() - 32) / 4));
        for (quint32 index = 0; index < count; ++index) {
            const auto offset = word(32 + index * 4);
            QVERIFY(offset <= quint32(bytes.size() - 8));
            const auto size = word(offset + 4);
            QVERIFY(size <= quint32(bytes.size()) - offset - 8);
        }
    }
};

QTEST_GUILESS_MAIN(WindowsResourcesTest)
#include "test_windowsresources.moc"
