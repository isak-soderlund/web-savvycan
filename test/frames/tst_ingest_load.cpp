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
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>

#include "canframemodel.h"
#include "tracerowheader.h"
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

// QTableView with its expensive entry points timed (the longest call of each)
class TimedTableView : public QTableView
{
public:
    // the map lives outside the view: the headers (children) still get events while the view dies
    explicit TimedTableView(QMap<QString, qint64> *costs) : maxMs(*costs) {}
    QMap<QString, qint64> &maxMs;
    template <typename F> void timed(const char *what, F f)
    {
        QElapsedTimer t; t.start();
        f();
        qint64 &m = maxMs[what];
        m = std::max(m, t.elapsed());
    }
    void doItemsLayout() override { timed("doItemsLayout", [&] { QTableView::doItemsLayout(); }); }
    void updateGeometries() override { timed("updateGeometries", [&] { QTableView::updateGeometries(); }); }
    void paintEvent(QPaintEvent *e) override { timed("paintEvent", [&] { QTableView::paintEvent(e); }); }
    void timerEvent(QTimerEvent *e) override { timed("timerEvent", [&] { QTableView::timerEvent(e); }); }
    void rowsInserted(const QModelIndex &p, int a, int b) override { timed("rowsInserted", [&] { QTableView::rowsInserted(p, a, b); }); }
    void rowsAboutToBeRemoved(const QModelIndex &p, int a, int b) override { timed("rowsAboutToBeRemoved", [&] { QTableView::rowsAboutToBeRemoved(p, a, b); }); }
    bool viewportEvent(QEvent *e) override { bool r; timed("viewportEvent", [&] { r = QTableView::viewportEvent(e); }); return r; }
    bool event(QEvent *e) override { bool r; timed(qPrintable(QString("event%1").arg(int(e->type()))), [&] { r = QTableView::event(e); }); return r; }
};

class TimedHeader : public TraceRowHeader
{
public:
    TimedHeader(Qt::Orientation o, QMap<QString, qint64> *m, const char *n) : TraceRowHeader(o), mMax(m), mName(n) {}
    void paintEvent(QPaintEvent *e) override
    {
        const bool outside = orientation() == Qt::Vertical &&
            (visualIndexAt(e->rect().top()) < 0 || visualIndexAt(e->rect().bottom()) < 0);
        QElapsedTimer t; t.start();
        TraceRowHeader::paintEvent(e);
        qint64 &m = (*mMax)[QString(mName) + (outside ? ".paintOutside" : ".paint")];
        m = std::max(m, t.elapsed());
        if (outside) (*mMax)[QString(mName) + ".outsidePaints"]++;
    }
    bool viewportEvent(QEvent *e) override
    {
        QElapsedTimer t; t.start();
        bool r = QHeaderView::viewportEvent(e);
        qint64 &m = (*mMax)[QString(mName) + ".vp" + QString::number(int(e->type()))];
        m = std::max(m, t.elapsed());
        return r;
    }
    QSize sizeHint() const override
    {
        QElapsedTimer t; t.start();
        QSize r = QHeaderView::sizeHint();
        qint64 &m = (*mMax)[QString(mName) + ".sizeHint"];
        m = std::max(m, t.elapsed());
        return r;
    }
    QMap<QString, qint64> *mMax;
    const char *mName;
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
        const bool showView = !qEnvironmentVariableIsSet("WSC_LOAD_NOVIEW");

        QElapsedTimer runClock;
        runClock.start();

        // the device: rate frames/s, delivered every 2 ms (USB delivers in packets)
        std::atomic<quint64> generated{0};
        std::atomic<bool> stopGen{false};
        auto produce = [&]() -> QByteArray {
            const quint64 due = quint64(double(runClock.nsecsElapsed()) * 1e-9 * rate);
            QByteArray bytes;
            quint64 n = generated.load();
            for (; n < due; n++) bytes += gen.frameBytes(n);
            generated.store(n);
            return bytes;
        };

        // the connection, its parser and the device timer, all in one thread (the connection's), as a
        // serial port's readyRead: threaded = desktop (a thread of its own, plain std::thread so every
        // hand-off is visible to TSan); else the GUI thread with sliced parsing, as in the wasm build
        std::atomic<qsizetype> left{1};           // bytes received, not parsed yet
        std::atomic<bool> quitConn{false};
        std::mutex connMutex;
        std::condition_variable connCv;
        CANConnection *conn = nullptr;
        bool connRemoved = false;
        auto makeConn = [&](bool ownThread, std::function<void(const QByteArray &)> &feed,
                            std::function<qsizetype()> &pending) -> CANConnection * {
            if (gen.slcan)
            {
                auto *l = new LAWICELSerial("load", 0, 500000, fd, 2000000, false);
                l->setRxSliceBudgetMs(ownThread ? 0 : 8);
                feed = [l](const QByteArray &b) { l->processRxBytes(b); };
                pending = [l] { return l->pendingRxBytes(); };
                return l;
            }
            auto *g = new GVRetSerial("load", false, false);
            g->setRxSliceBudgetMs(ownThread ? 0 : 8);
            feed = [g](const QByteArray &b) { g->processRxBytes(b); };
            pending = [g] { return g->pendingRxBytes(); };
            return g;
        };
        auto deviceTick = [&](const std::function<void(const QByteArray &)> &feed, const std::function<qsizetype()> &pending) {
            if (!stopGen.load())
            {
                QByteArray b = produce();
                if (!b.isEmpty()) feed(b);
            }
            left = pending();
        };

        std::thread connThread;
        std::function<void(const QByteArray &)> mainFeed;
        std::function<qsizetype()> mainPending;
        QTimer mainDevice;
        if (threaded)
        {
            connThread = std::thread([&] {
                QEventLoop loop; // first: gives this (adopted) thread its event dispatcher, needed by timers
                std::function<void(const QByteArray &)> feed;
                std::function<qsizetype()> pending;
                CANConnection *c = makeConn(true, feed, pending);
                QTimer device;
                QObject::connect(&device, &QTimer::timeout, [&] { deviceTick(feed, pending); });
                device.start(2);
                QTimer quitPoll;
                QObject::connect(&quitPoll, &QTimer::timeout, [&] { if (quitConn.load()) loop.quit(); });
                quitPoll.start(5);
                {
                    std::lock_guard<std::mutex> lk(connMutex);
                    conn = c;
                }
                connCv.notify_all();
                loop.exec();
                device.stop();
                quitPoll.stop();
                {
                    std::unique_lock<std::mutex> lk(connMutex);
                    connCv.wait(lk, [&] { return connRemoved; }); // the GUI side lets go of it first
                }
                delete c;
            });
            std::unique_lock<std::mutex> lk(connMutex);
            connCv.wait(lk, [&] { return conn != nullptr; });
        }
        else
        {
            conn = makeConn(false, mainFeed, mainPending);
            QObject::connect(&mainDevice, &QTimer::timeout, [&] { deviceTick(mainFeed, mainPending); });
            mainDevice.start(2);
        }

        CANConManager *mgr = CANConManager::getInstance();
        mgr->add(conn);
        CommFrameModel model;
        // as MainWindow's direct connection, timed
        qint64 maxAddMs = 0;
        auto ingest = QObject::connect(mgr, &CANConManager::framesReceived, &model, [&](CANConnection *c, QVector<CommFrame> &v) {
            QElapsedTimer t; t.start();
            model.addFrames(c, v);
            maxAddMs = std::max(maxAddMs, t.elapsed());
        });

        QMap<QString, qint64> viewCosts;
        TimedTableView view(&viewCosts);
        view.setVerticalHeader(new TimedHeader(Qt::Vertical, &view.maxMs, "vhdr"));
        view.setHorizontalHeader(new TimedHeader(Qt::Horizontal, &view.maxMs, "hhdr"));
        view.setModel(&model);
        // as MainWindow sets up canFramesView
        view.verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        view.verticalHeader()->setDefaultSectionSize(20);
        view.horizontalHeader()->setStretchLastSection(true);
        view.setAlternatingRowColors(true);
        view.setWordWrap(false);
        view.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        view.resize(1000, 600);
        if (showView) view.show();

        // GUI tick as MainWindow::tickGUIUpdate (250 ms): publish rows, scroll, signal the windows
        int ticks = 0;
        qint64 maxTickMs = 0, maxGapMs = 0, maxGapAfterStartMs = 0;
        QTimer guiTick;
        QObject::connect(&guiTick, &QTimer::timeout, [&] {
            QElapsedTimer t; t.start();
            if (model.sendBulkRefresh() > 0) view.scrollToBottom();
            ticks++;
            maxTickMs = std::max(maxTickMs, t.elapsed());
        });
        guiTick.start(250);

        // GUI thread responsiveness: the longest gap between 10 ms timer events
        QElapsedTimer gapClock;
        gapClock.start();
        QTimer gapTimer;
        QObject::connect(&gapTimer, &QTimer::timeout, [&] {
            const qint64 g = gapClock.restart();
            maxGapMs = std::max(maxGapMs, g);
            if (runClock.elapsed() > 1000) maxGapAfterStartMs = std::max(maxGapAfterStartMs, g);
        });
        gapTimer.start(10);

        // a reader thread, as the frame sender (stage 5) reading the trace
        std::atomic<bool> stopReader{false};
        std::atomic<qint64> readerBad{0}, readerReads{0};
        std::thread reader([&] {
            const FrameListView *rows = model.getFilteredListReference();
            while (!stopReader.load())
            {
                for (const CommFrame &f : rows->tail(32))
                    if (f.payload().size() != len || quint8(f.payload().at(0)) != quint8(f.frameId())) readerBad++;
                readerReads++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        // run, sampling memory
        qint64 rssAfterFill = 0, rssMax = 0;
        const qint64 rssStart = tf::rssKb();
        bool ringBounded = true;
        while (runClock.elapsed() < seconds * 1000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (!rssAfterFill && model.framesWrapped() > 0) rssAfterFill = tf::rssKb();
            rssMax = std::max(rssMax, tf::rssKb());
            if (model.totalFrameCount() > kMaxFrames) ringBounded = false;
        }

        // stop the device, let everything in flight arrive
        stopGen = true;
        mainDevice.stop();
        if (!threaded) left = mainPending();
        const quint64 total = generated.load();
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 30000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (!threaded) left = mainPending();
            if (left.load() == 0 && conn->getQueue().peek() == nullptr) break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        model.sendBulkRefresh();
        stopReader = true;
        reader.join();
        guiTick.stop();
        gapTimer.stop();

        const quint64 inModel = model.framesWrapped() + quint64(model.totalFrameCount());
        const quint64 dropped = conn->getDroppedFrames();
        const double secs = runClock.elapsed() / 1000.0;
        qInfo().noquote() << QString("%1: %2 frames in %3 s (%4/s), in model %5 (ring %6, wrapped %7), dropped %8, "
                                     "rows %9, ticks %10, max GUI gap %11 ms, RSS start %12 MB, after ring full %13 MB, "
                                     "max %14 MB, end %15 MB, reader reads %16; longest addFrames %17 ms, GUI tick %18 ms, gap after 1st s %19 ms")
                                 .arg(QTest::currentDataTag()).arg(total).arg(secs, 0, 'f', 1).arg(total / secs, 0, 'f', 0)
                                 .arg(inModel).arg(model.totalFrameCount()).arg(model.framesWrapped()).arg(dropped)
                                 .arg(model.rowCount()).arg(ticks).arg(maxGapMs)
                                 .arg(rssStart / 1024).arg(rssAfterFill / 1024).arg(rssMax / 1024).arg(tf::rssKb() / 1024)
                                 .arg(readerReads.load()).arg(maxAddMs).arg(maxTickMs).arg(maxGapAfterStartMs);
        if (showView)
        {
            QStringList costs;
            for (auto it = view.maxMs.cbegin(); it != view.maxMs.cend(); ++it)
                if (it.key().endsWith("outsidePaints")) costs << QString("%1 %2").arg(it.key()).arg(it.value());
                else if (it.value() >= 5) costs << QString("%1 %2 ms").arg(it.key()).arg(it.value());
            qInfo().noquote() << "view, longest calls:" << costs.join(", ");
        }

        // take the connection down before any check can return early
        QObject::disconnect(ingest);
        mgr->remove(conn);
        const qsizetype leftAtEnd = left.load();
        const bool queueEmpty = conn->getQueue().peek() == nullptr;
        if (threaded)
        {
            quitConn = true;
            {
                std::lock_guard<std::mutex> lk(connMutex);
                connRemoved = true;
            }
            connCv.notify_all();
            connThread.join();
        }
        else
            delete conn;

        // every frame accounted for
        QCOMPARE(leftAtEnd, qsizetype(0));
        QVERIFY(queueEmpty);
        QCOMPARE(inModel + dropped, total);
        QVERIFY(total >= quint64(rate) * quint64(seconds) * 9 / 10);
        // the ring stays bounded, and the rows match it
        QVERIFY(ringBounded);
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
            QVERIFY2(maxGapAfterStartMs < 250, qPrintable(QString("GUI thread stalled %1 ms").arg(maxGapAfterStartMs)));
            QCOMPARE(dropped, quint64(0));
        }
    }
};

QTEST_MAIN(TestIngestLoad)
#include "tst_ingest_load.moc"
