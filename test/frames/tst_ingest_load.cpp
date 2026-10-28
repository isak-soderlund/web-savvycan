// web-savvycan: replay load test. A fake device produces a full bus as GVRET or slcan bytes; they
// go through the real receive path: parser (connection thread, or the GUI thread with sliced
// parsing as in the single-threaded wasm build) -> the connection's queue -> CANConManager's 20 ms
// drain -> CommFrameModel (ring) -> a QTableView, with the GUI tick (sendBulkRefresh) every 250 ms
// and a reader thread reading the trace like the frame sender.
//
// Asserts: no crash; every generated frame is either in the model (stored or wrapped out of the
// ring) or counted as dropped; the ring stays within MaximumFrames; memory stops growing once the
// ring is full; the GUI thread keeps turning.
//
// WSC_LOAD_SECONDS (default 10) sets the length of each run.
#include <QtTest>
#include <QTableView>
#include <QHeaderView>
#include <QScrollBar>
#include <QThread>
#include <atomic>
#include <memory>

#include "canframemodel.h"
#include "canconmanager.h"
#include "gvretserial.h"
#include "lawicel_serial.h"
#include "testframes.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
static constexpr bool kSanitized = true;
#else
static constexpr bool kSanitized = false;
#endif

static constexpr int kMaxFrames = 40000;    // the ring (MaximumFrames) for these runs: wraps after ~5 s

// The bytes of generated frame n (id, payload and length derive from n, so the trace can be checked)
struct BusGen
{
    bool fd = false;
    bool slcan = false;
    int len = 8;
    QByteArray frameBytes(quint64 n) const
    {
        const quint32 id = quint32(n % 0x7F0);
        QByteArray d(len, 0);
        d[0] = char(quint8(id));
        for (int i = 1; i < len; i++) d[i] = char(quint8(n >> (8 * ((i - 1) % 8))));
        if (slcan) return tf::slcanLine(id, d, fd, fd, false);
        return tf::gvretFrame(id, d, fd, fd, 0, quint32(n * 100));
    }
};

class TestIngestLoad : public QObject
{
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> settingsDir;
    int seconds = 10;

private slots:
    void initTestCase()
    {
        settingsDir.reset(tf::isolateSettings(kMaxFrames));
        seconds = qEnvironmentVariableIntValue("WSC_LOAD_SECONDS");
        if (seconds <= 0) seconds = 10;
    }

    void load_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<bool>("fd");
        QTest::addColumn<int>("len");
        QTest::addColumn<int>("rate");
        QTest::addColumn<bool>("threaded");
        // classic 500 kbit/s at full load ~ 8000 frames/s; CAN FD 64 bytes at 8 Mbit/s data ~ 9000/s
        QTest::newRow("gvret_classic_8000_threaded")  << "gvret" << false << 8 << 8000 << true;
        QTest::newRow("gvret_fd64_9000_threaded")     << "gvret" << true << 64 << 9000 << true;
        QTest::newRow("gvret_fd64_9000_singlethread") << "gvret" << true << 64 << 9000 << false;
        QTest::newRow("slcan_fd64_9000_threaded")     << "slcan" << true << 64 << 9000 << true;
        QTest::newRow("slcan_classic_8000_singlethread") << "slcan" << false << 8 << 8000 << false;
    }

    void load()
    {
        QFETCH(QString, protocol);
        QFETCH(bool, fd);
        QFETCH(int, len);
        QFETCH(int, rate);
        QFETCH(bool, threaded);

        BusGen gen;
        gen.fd = fd;
        gen.len = len;
        gen.slcan = (protocol == "slcan");

        // the connection: threaded = desktop (own QThread); else as in wasm (GUI thread, sliced parsing)
        CANConnection *conn;
        std::function<void(const QByteArray &)> feed;   // runs in the connection's thread
        std::function<qsizetype()> pending;
        if (gen.slcan)
        {
            auto *l = new LAWICELSerial("load", 0, 500000, fd, 2000000, threaded);
            if (!threaded) l->setRxSliceBudgetMs(8);
            feed = [l](const QByteArray &b) { l->processRxBytes(b); };
            pending = [l] { return l->pendingRxBytes(); };
            conn = l;
        }
        else
        {
            auto *g = new GVRetSerial("load", false, threaded);
            if (!threaded) g->setRxSliceBudgetMs(8);
            feed = [g](const QByteArray &b) { g->processRxBytes(b); };
            pending = [g] { return g->pendingRxBytes(); };
            conn = g;
        }
        auto inConnThread = [&](std::function<void()> fn) {
            if (threaded) QMetaObject::invokeMethod(conn, fn, Qt::BlockingQueuedConnection);
            else fn();
        };

        CANConManager *mgr = CANConManager::getInstance();
        mgr->add(conn);
        CommFrameModel model;
        QObject::connect(mgr, &CANConManager::framesReceived, &model, &CommFrameModel::addFrames);

        QTableView view;
        view.setModel(&model);
        view.verticalHeader()->setDefaultSectionSize(20);
        view.resize(1000, 600);
        view.show();

        // GUI tick as MainWindow::tickGUIUpdate (250 ms): publish rows, scroll, signal the windows
        int ticks = 0;
        QTimer guiTick;
        QObject::connect(&guiTick, &QTimer::timeout, [&] {
            if (model.sendBulkRefresh() > 0) view.scrollToBottom();
            ticks++;
        });
        guiTick.start(250);

        // GUI thread responsiveness: the longest gap between 10 ms timer events
        qint64 maxGapMs = 0;
        QElapsedTimer gapClock;
        gapClock.start();
        QTimer gapTimer;
        QObject::connect(&gapTimer, &QTimer::timeout, [&] {
            maxGapMs = std::max(maxGapMs, gapClock.restart());
        });
        gapTimer.start(10);

        // a reader thread, as the frame sender (stage 5) reading the trace
        std::atomic<bool> stopReader{false};
        std::atomic<qint64> readerBad{0}, readerReads{0};
        QThread *reader = QThread::create([&] {
            const FrameListView *rows = model.getFilteredListReference();
            while (!stopReader.load())
            {
                for (const CommFrame &f : rows->tail(32))
                    if (f.payload().size() != len || quint8(f.payload().at(0)) != quint8(f.frameId())) readerBad++;
                readerReads++;
                QThread::msleep(1);
            }
        });
        reader->start();

        // the device: rate frames/s, in bursts every 2 ms (USB delivers in packets)
        std::atomic<quint64> generated{0};
        std::atomic<bool> stopGen{false};
        QElapsedTimer runClock;
        runClock.start();
        auto produce = [&]() -> QByteArray {
            const quint64 due = quint64(double(runClock.nsecsElapsed()) * 1e-9 * rate);
            QByteArray bytes;
            quint64 n = generated.load();
            for (; n < due; n++) bytes += gen.frameBytes(n);
            generated.store(n);
            return bytes;
        };
        QThread *device = nullptr;
        QTimer deviceTimer;
        if (threaded)
        {
            device = QThread::create([&] {
                while (!stopGen.load())
                {
                    QByteArray b = produce();
                    if (!b.isEmpty())
                        QMetaObject::invokeMethod(conn, [feed, b] { feed(b); }, Qt::QueuedConnection);
                    QThread::msleep(2);
                }
            });
            device->start();
        }
        else
        {
            // wasm: Web Serial's read loop hands bytes to the GUI thread
            QObject::connect(&deviceTimer, &QTimer::timeout, [&] {
                QByteArray b = produce();
                if (!b.isEmpty()) feed(b);
            });
            deviceTimer.start(2);
        }

        // run, sampling memory
        qint64 rssAfterFill = 0, rssMax = 0;
        const qint64 rssStart = tf::rssKb();
        while (runClock.elapsed() < seconds * 1000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (!rssAfterFill && model.framesWrapped() > 0) rssAfterFill = tf::rssKb();
            rssMax = std::max(rssMax, tf::rssKb());
            QVERIFY(model.totalFrameCount() <= kMaxFrames);
        }

        // stop the device, let everything in flight arrive
        stopGen = true;
        deviceTimer.stop();
        if (device) { device->wait(); delete device; }
        const quint64 total = generated.load();
        QElapsedTimer settle;
        settle.start();
        qsizetype left = 1;
        while (settle.elapsed() < 30000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            inConnThread([&] { left = pending(); });
            if (left == 0 && conn->getQueue().peek() == nullptr) break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        model.sendBulkRefresh();
        stopReader = true;
        reader->wait();
        delete reader;
        guiTick.stop();
        gapTimer.stop();

        const quint64 inModel = model.framesWrapped() + quint64(model.totalFrameCount());
        const quint64 dropped = conn->getDroppedFrames();
        const double secs = runClock.elapsed() / 1000.0;
        qInfo().noquote() << QString("%1: %2 frames in %3 s (%4/s), in model %5 (ring %6, wrapped %7), dropped %8, "
                                     "rows %9, ticks %10, max GUI gap %11 ms, RSS start %12 MB, after ring full %13 MB, "
                                     "max %14 MB, end %15 MB, reader reads %16")
                                 .arg(QTest::currentDataTag()).arg(total).arg(secs, 0, 'f', 1).arg(total / secs, 0, 'f', 0)
                                 .arg(inModel).arg(model.totalFrameCount()).arg(model.framesWrapped()).arg(dropped)
                                 .arg(model.rowCount()).arg(ticks).arg(maxGapMs)
                                 .arg(rssStart / 1024).arg(rssAfterFill / 1024).arg(rssMax / 1024).arg(tf::rssKb() / 1024)
                                 .arg(readerReads.load());

        // every frame accounted for
        QCOMPARE(left, qsizetype(0));
        QCOMPARE(inModel + dropped, total);
        QVERIFY(total >= quint64(rate) * quint64(seconds) * 9 / 10);
        // the ring stays bounded, and the rows match it
        QVERIFY(model.totalFrameCount() <= kMaxFrames);
        QCOMPARE(qsizetype(model.rowCount()), qsizetype(model.totalFrameCount()));
        QCOMPARE(readerBad.load(), qint64(0));
        // the newest row is the last frame that got through
        const CommFrame last = model.getListReference()->last();
        QCOMPARE(last.payload().size(), len);
        QCOMPARE(last.hasFlexibleDataRateFormat(), fd);
        QCOMPARE(last.hasBitrateSwitch(), fd);
        // content of the whole ring
        quint64 badContent = 0;
        for (const CommFrame &f : model.getListReference()->toVector())
            if (f.payload().size() != len || quint8(f.payload().at(0)) != quint8(f.frameId())) badContent++;
        QCOMPARE(badContent, quint64(0));
        if (!kSanitized)
        {
            // memory: once the ring is full it must not keep growing (a little slack for the allocator)
            QVERIFY2(rssAfterFill == 0 || rssMax - rssAfterFill < 64 * 1024,
                     qPrintable(QString("RSS grew %1 MB after the ring was full").arg((rssMax - rssAfterFill) / 1024)));
            QVERIFY2(maxGapMs < 1000, qPrintable(QString("GUI thread stalled %1 ms").arg(maxGapMs)));
            QCOMPARE(dropped, quint64(0));
        }

        QObject::disconnect(mgr, &CANConManager::framesReceived, &model, &CommFrameModel::addFrames);
        mgr->remove(conn);
        delete conn;
    }
};

QTEST_MAIN(TestIngestLoad)
#include "tst_ingest_load.moc"
