// web-savvycan: the GVRET and LAWICEL (slcan) receive parsers, fed bytes directly (no port).
#include <QtTest>
#include <QSignalSpy>
#include <memory>

#include "gvretserial.h"
#include "lawicel_serial.h"
#include "testframes.h"

using tf::gvretFrame;
using tf::slcanLine;

static QVector<CommFrame> drain(CANConnection *c)
{
    QVector<CommFrame> out;
    CommFrame *f;
    while ((f = c->getQueue().peek()))
    {
        out.append(*f);
        c->getQueue().dequeue();
    }
    return out;
}

static QByteArray pattern(int n, quint8 seed)
{
    QByteArray d(n, 0);
    for (int i = 0; i < n; i++) d[i] = char(quint8(seed + i * 7));
    return d;
}

class TestParsers : public QObject
{
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> settingsDir;
    std::unique_ptr<GVRetSerial> gvret() { return std::make_unique<GVRetSerial>("test", false, false); }
    std::unique_ptr<LAWICELSerial> slcan() { return std::make_unique<LAWICELSerial>("test", 0, 500000, true, 2000000, false); }

private slots:
    void initTestCase() { settingsDir.reset(tf::isolateSettings(1000)); }

    // ---------------- GVRET ----------------
    void gvret_classic()
    {
        auto g = gvret();
        QByteArray bytes;
        bytes += gvretFrame(0x123, pattern(8, 1), false, false, 0, 1000);
        bytes += gvretFrame(0x1ABCDEF0, pattern(3, 2), false, false, 1, 2000, true);
        bytes += gvretFrame(0x7FF, QByteArray(), false, false, 0, 3000); // zero length
        g->processRxBytes(bytes);
        const auto fr = drain(g.get());
        QCOMPARE(fr.size(), 3);
        QCOMPARE(fr[0].frameId(), quint32(0x123));
        QCOMPARE(fr[0].payload(), pattern(8, 1));
        QCOMPARE(fr[0].frameType(), CommFrame::CANDataFrame);
        QVERIFY(!fr[0].hasFlexibleDataRateFormat());
        QCOMPARE(fr[0].timeStamp().microSeconds(), qint64(1000));
        QCOMPARE(fr[1].frameId(), quint32(0x1ABCDEF0));
        QVERIFY(fr[1].hasExtendedFrameFormat());
        QCOMPARE(fr[1].getBus(), 1);
        QCOMPARE(fr[1].payload().size(), 3);
        QCOMPARE(fr[2].payload().size(), 0);
        QVERIFY(fr[2].isReceived());
    }

    void gvret_fd_data()
    {
        QTest::addColumn<int>("len");
        QTest::addColumn<bool>("brs");
        for (int len : {0, 1, 8, 12, 16, 20, 24, 32, 48, 64})
            for (bool brs : {false, true})
                QTest::newRow(qPrintable(QString("len%1%2").arg(len).arg(brs ? "_brs" : ""))) << len << brs;
    }
    void gvret_fd()
    {
        QFETCH(int, len);
        QFETCH(bool, brs);
        auto g = gvret();
        g->processRxBytes(gvretFrame(0x18DAF110, pattern(len, quint8(len)), true, brs, 2, 123456, true));
        const auto fr = drain(g.get());
        QCOMPARE(fr.size(), 1);
        QCOMPARE(fr[0].frameType(), CommFrame::CANFDDataFrame);
        QVERIFY(fr[0].hasFlexibleDataRateFormat());
        QCOMPARE(fr[0].hasBitrateSwitch(), brs);
        QVERIFY(!fr[0].hasErrorStateIndicator());
        QCOMPARE(fr[0].payload(), pattern(len, quint8(len)));
        QCOMPARE(fr[0].getBus(), 2);
        QVERIFY(fr[0].hasExtendedFrameFormat());
        QCOMPARE(fr[0].frameId(), quint32(0x18DAF110));
    }

    // The device's 32-bit µs time past 2^32 (71.6 min): classic and FD times go on increasing; a frame a little
    // older than the one before stays a little older.
    void gvret_timeWrap()
    {
        auto g = gvret();
        const quint32 raw[] = {0xFFFFF000u, 0xFFFFFF00u, 0x00000100u, 0x00002000u, 0x00001F00u};
        const qint64 want[] = {0xFFFFF000LL, 0xFFFFFF00LL, 0x100000100LL, 0x100002000LL, 0x100001F00LL};
        QByteArray bytes;
        for (int i = 0; i < 5; i++) bytes += gvretFrame(0x100 + i, pattern(8, quint8(i)), i % 2 == 1, false, 0, raw[i]);
        g->processRxBytes(bytes);
        const auto fr = drain(g.get());
        QCOMPARE(fr.size(), 5);
        for (int i = 0; i < 5; i++) QCOMPARE(fr[i].timeStamp().microSeconds(), want[i]);
        QCOMPARE(fr[1].frameType(), CommFrame::CANFDDataFrame);
    }

    // The time sync (command 1) answered just before the wrap: frames after it are timed from the app's time basis
    // (near 0, not 2^32 µs off) and keep their spacing across the wrap.
    void gvret_timeSyncBeforeWrap()
    {
        auto g = gvret();
        QByteArray bytes;
        bytes.append(char(0xF1));
        bytes.append(char(1));
        tf::putLE32(bytes, 0xFFF00000u);                       // 1.05 s before the wrap
        bytes += gvretFrame(0x100, pattern(8, 1), false, false, 0, 0xFFF00000u + 1000);
        bytes += gvretFrame(0x101, pattern(8, 2), true, true, 0, 0x00001000u);
        g->processRxBytes(bytes);
        const auto fr = drain(g.get());
        QCOMPARE(fr.size(), 2);
        const qint64 t0 = fr[0].timeStamp().microSeconds(), t1 = fr[1].timeStamp().microSeconds();
        QVERIFY2(t0 > -1000000 && t0 < 600000000, qPrintable(QString::number(t0)));
        QCOMPARE(t1 - t0, qint64(0x100000000LL + 0x1000 - 0xFFF00000LL - 1000));
    }

    void gvret_fdThenClassicClearsFlags()
    {
        auto g = gvret();
        g->processRxBytes(gvretFrame(0x10, pattern(64, 3), true, true, 0, 1) + gvretFrame(0x11, pattern(8, 4), false, false, 0, 2));
        const auto fr = drain(g.get());
        QCOMPARE(fr.size(), 2);
        QVERIFY(!fr[1].hasFlexibleDataRateFormat());
        QVERIFY(!fr[1].hasBitrateSwitch());
        QCOMPARE(fr[1].frameType(), CommFrame::CANDataFrame);
    }

    void gvret_splitAnywhere()
    {
        QByteArray all;
        for (int i = 0; i < 20; i++) all += gvretFrame(0x100 + i, pattern(i % 2 ? 64 : 8, quint8(i)), i % 2, true, 0, i);
        for (int chunk : {1, 2, 3, 7, 13, 64, 1000})
        {
            auto g = gvret();
            for (int p = 0; p < all.size(); p += chunk) g->processRxBytes(all.mid(p, chunk));
            const auto fr = drain(g.get());
            QCOMPARE(fr.size(), 20);
            for (int i = 0; i < 20; i++)
            {
                QCOMPARE(fr[i].frameId(), quint32(0x100 + i));
                QCOMPARE(fr[i].payload(), pattern(i % 2 ? 64 : 8, quint8(i)));
            }
        }
    }

    void gvret_invalidLengthsResync()
    {
        auto g = gvret();
        QByteArray bad;
        bad += gvretFrame(0x1, pattern(8, 0), true, false, 0, 0);
        bad[10] = char(70);                        // FD length 70: invalid
        QByteArray badClassic = gvretFrame(0x2, pattern(8, 0), false, false, 0, 0);
        badClassic[10] = char(0x0C);               // classic length 12: invalid
        QByteArray badBus = gvretFrame(0x3, pattern(8, 0), true, false, 9, 0); // bus 9 of 3
        QByteArray good = gvretFrame(0x4, pattern(8, 5), false, false, 0, 0);
        g->processRxBytes(QByteArray("\x00\x13\x37garbage", 10) + bad + good + badClassic + good + badBus + good);
        const auto fr = drain(g.get());
        QCOMPARE(fr.size(), 3);
        for (const auto &f : fr) QCOMPARE(f.frameId(), quint32(0x4));
    }

    void gvret_queueFullCountsDrops()
    {
        auto g = gvret();
        QByteArray bytes;
        const int n = CANConnection::SerialQueueLen + 500;
        for (int i = 0; i < n; i++) bytes += gvretFrame(quint32(i & 0x7FF), pattern(8, 0), false, false, 0, i);
        g->processRxBytes(bytes);
        const auto fr = drain(g.get());
        QCOMPARE(quint64(fr.size()) + g->getDroppedFrames(), quint64(n));
        QCOMPARE(g->getDroppedFrames(), quint64(n - (CANConnection::SerialQueueLen - 1)));
        g->resetDroppedFrames();
        QCOMPARE(g->getDroppedFrames(), quint64(0));
    }

    void gvret_slicedParsingYields()
    {
        auto g = gvret();
        g->setRxSliceBudgetMs(1);
        QByteArray bytes;
        for (int i = 0; i < 40000; i++) bytes += gvretFrame(0x100, pattern(64, quint8(i)), true, true, 0, i);
        QElapsedTimer t;
        t.start();
        g->processRxBytes(bytes);              // ~3 MB: must return after its slice
        const qint64 firstCall = t.elapsed();
        QVERIFY2(g->pendingRxBytes() > 0, "parsing did not yield");
        QVERIFY2(firstCall < 200, qPrintable(QString("first slice took %1 ms").arg(firstCall)));
        int frames = 0;
        QElapsedTimer guard;
        guard.start();
        while (g->pendingRxBytes() > 0 && guard.elapsed() < 60000)
        {
            QCoreApplication::processEvents();
            frames += drain(g.get()).size();
        }
        frames += drain(g.get()).size();
        QCOMPARE(frames + int(g->getDroppedFrames()), 40000);
        QCOMPARE(g->getDroppedFrames(), quint64(0));
    }

    void gvret_debugTextOnlyWithConsole()
    {
        // M8: no debugOutput unless something listens (the console); a listener gets it
        auto g = gvret();
        QSignalSpy spy(g.get(), &CANConnection::debugOutput); // a connected listener, as the console
        g->setBusSettings(0, CANBus()); // exercises sendToSerial's debug path (port not open)
        QVERIFY(spy.count() > 0);
    }

    // ---------------- LAWICEL / slcan ----------------
    void slcan_classicAndExtended()
    {
        auto l = slcan();
        l->processRxBytes(slcanLine(0x123, pattern(8, 1), false, false, false) +
                          slcanLine(0x1ABCDEF0, pattern(2, 9), false, false, true) +
                          slcanLine(0x7FF, QByteArray(), false, false, false));
        const auto fr = drain(l.get());
        QCOMPARE(fr.size(), 3);
        QCOMPARE(fr[0].frameId(), quint32(0x123));
        QCOMPARE(fr[0].payload(), pattern(8, 1));
        QCOMPARE(fr[0].frameType(), CommFrame::CANDataFrame);
        QVERIFY(fr[1].hasExtendedFrameFormat());
        QCOMPARE(fr[1].frameId(), quint32(0x1ABCDEF0));
        QCOMPARE(fr[2].payload().size(), 0);
    }

    void slcan_fd_data()
    {
        QTest::addColumn<int>("len");
        QTest::addColumn<bool>("brs");
        QTest::addColumn<bool>("ext");
        for (int len : {0, 8, 12, 16, 20, 24, 32, 48, 64})
            for (bool brs : {false, true})
                for (bool ext : {false, true})
                    QTest::newRow(qPrintable(QString("len%1%2%3").arg(len).arg(brs ? "_brs" : "").arg(ext ? "_ext" : "")))
                        << len << brs << ext;
    }
    void slcan_fd()
    {
        QFETCH(int, len);
        QFETCH(bool, brs);
        QFETCH(bool, ext);
        auto l = slcan();
        const quint32 id = ext ? 0x18DAF110 : 0x456;
        l->processRxBytes(slcanLine(id, pattern(len, 3), true, brs, ext, 0x1234));
        const auto fr = drain(l.get());
        QCOMPARE(fr.size(), 1);
        QCOMPARE(fr[0].frameType(), CommFrame::CANFDDataFrame);
        QVERIFY(fr[0].hasFlexibleDataRateFormat());
        QCOMPARE(fr[0].hasBitrateSwitch(), brs);
        QCOMPARE(fr[0].hasExtendedFrameFormat(), ext);
        QCOMPARE(fr[0].frameId(), id);
        QCOMPARE(fr[0].payload(), pattern(len, 3));
    }

    void slcan_badInput()
    {
        auto l = slcan();
        QByteArray junk(500, 'A');                       // no CR: dropped once over the line limit
        QByteArray classic9 = "t1239112233445566778899\r"; // classic DLC 9: at most 8 bytes kept
        QByteArray good = slcanLine(0x321, pattern(4, 0), false, false, false);
        l->processRxBytes(junk + "\r" + good + "\a" + "z\r" + classic9 + good + "\r");
        const auto fr = drain(l.get());
        QCOMPARE(fr.size(), 3);
        QCOMPARE(fr[0].frameId(), quint32(0x321));
        QCOMPARE(fr[1].payload().size(), 8);
        QCOMPARE(fr[2].frameId(), quint32(0x321));
    }

    void slcan_split()
    {
        QByteArray all;
        for (int i = 0; i < 30; i++) all += slcanLine(0x100 + i, pattern(i % 2 ? 64 : 8, quint8(i)), i % 2, true, i % 3 == 0);
        for (int chunk : {1, 5, 17, 300})
        {
            auto l = slcan();
            for (int p = 0; p < all.size(); p += chunk) l->processRxBytes(all.mid(p, chunk));
            const auto fr = drain(l.get());
            QCOMPARE(fr.size(), 30);
            for (int i = 0; i < 30; i++) QCOMPARE(fr[i].payload(), pattern(i % 2 ? 64 : 8, quint8(i)));
        }
    }

    void slcan_queueFullCountsDrops()
    {
        auto l = slcan();
        QByteArray bytes;
        const int n = CANConnection::SerialQueueLen + 100;
        const QByteArray line = slcanLine(0x10, pattern(8, 0), false, false, false);
        for (int i = 0; i < n; i++) bytes += line;
        l->processRxBytes(bytes);
        const auto fr = drain(l.get());
        QCOMPARE(quint64(fr.size()) + l->getDroppedFrames(), quint64(n));
        QVERIFY(l->getDroppedFrames() > 0);
    }
};

QTEST_MAIN(TestParsers)
#include "tst_parsers.moc"
