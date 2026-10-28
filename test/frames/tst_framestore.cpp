// web-savvycan: FrameStore (ring), FrameListView, and CommFrameModel on top of them.
#include <QtTest>
#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QThread>
#include <atomic>
#include <memory>

#include "framestore.h"
#include "canframemodel.h"
#include "testframes.h"

using tf::makeFrame;

class TestFrameStore : public QObject
{
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> settingsDir;
    static QVector<CommFrame> batch(quint32 firstId, int n, int len = 8, bool fd = false)
    {
        QVector<CommFrame> v;
        for (int i = 0; i < n; i++) v.append(makeFrame(firstId + quint32(i), len, fd, fd, 0, qint64(firstId + quint32(i))));
        return v;
    }
    static quint32 rowId(const CommFrameModel &m, int row)
    {
        // the row's frame as the windows see it (the ID column shows 11-bit IDs masked); the display
        // path itself must give a value for every row
        if (!m.data(m.index(row, int(Column::FrameId)), Qt::DisplayRole).isValid()) return 0xFFFFFFFFu;
        return m.getFilteredListReference()->at(row).frameId();
    }

private slots:
    void initTestCase() { settingsDir.reset(tf::isolateSettings(1000)); }

    // ---------------- FrameStore ----------------
    void store_appendAndIndex()
    {
        FrameStore s(1000, 100);
        for (int i = 0; i < 950; i++) QCOMPARE(s.append(makeFrame(quint32(i), 8)), qsizetype(0));
        QCOMPARE(s.size(), qsizetype(950));
        QCOMPARE(s.firstSeq(), quint64(0));
        QCOMPARE(s.endSeq(), quint64(950));
        for (int i = 0; i < 950; i++) QCOMPARE(s.atIndex(i).frameId(), quint32(i));
        QVERIFY(s.contains(949));
        QVERIFY(!s.contains(950));
    }

    void store_wrapKeepsNewest()
    {
        FrameStore s(1000, 100);
        qsizetype dropped = 0;
        for (int i = 0; i < 10000; i++) dropped += s.append(makeFrame(quint32(i), 8));
        QVERIFY(s.size() <= 1000);
        QVERIFY(s.size() >= 900);
        QCOMPARE(s.totalAppended(), quint64(10000));
        QCOMPARE(s.totalWrapped(), quint64(dropped));
        QCOMPARE(quint64(dropped) + quint64(s.size()), quint64(10000));
        QCOMPARE(s.atIndex(s.size() - 1).frameId(), quint32(9999));
        QCOMPARE(s.atIndex(0).frameId(), quint32(s.firstSeq())); // seq == id in this test
        QCOMPARE(s.firstSeq() % 100, quint64(0));                 // whole chunks dropped
        QVERIFY(!s.contains(s.firstSeq() - 1));
    }

    void store_framesDontMove()
    {
        FrameStore s(100000, 1000);
        s.append(makeFrame(7, 8));
        const CommFrame *p = &s.bySeq(0);
        for (int i = 0; i < 50000; i++) s.append(makeFrame(quint32(i), 8));
        QCOMPARE(p, &s.bySeq(0));
        QCOMPARE(p->frameId(), quint32(7));
    }

    void store_clearKeepsSeqsUnique()
    {
        FrameStore s(1000, 100);
        for (int i = 0; i < 150; i++) s.append(makeFrame(quint32(i), 8));
        const quint64 oldEnd = s.endSeq();
        s.clear();
        QVERIFY(s.isEmpty());
        QVERIFY(!s.contains(0));
        QVERIFY(!s.contains(oldEnd - 1));
        s.append(makeFrame(1, 8));
        QVERIFY(s.firstSeq() >= oldEnd);
        QCOMPARE(s.size(), qsizetype(1));
    }

    void store_autoChunk()
    {
        FrameStore small(1000);
        QCOMPARE(small.chunkFrames(), qsizetype(256));
        FrameStore big(10000000);
        QCOMPARE(big.chunkFrames(), FrameStore::DefaultChunkFrames);
        for (int i = 0; i < 5000; i++) small.append(makeFrame(quint32(i), 8));
        QVERIFY(small.size() <= 1000 && small.size() > 1000 - 256);
        QCOMPARE(small.totalWrapped() + quint64(small.size()), quint64(5000));
    }

    void store_shrinkCapacity()
    {
        FrameStore s(10000, 100);
        for (int i = 0; i < 5000; i++) s.append(makeFrame(quint32(i), 8));
        s.setCapacity(1000);
        QVERIFY(s.size() <= 1000);
        QCOMPARE(s.atIndex(s.size() - 1).frameId(), quint32(4999));
        QCOMPARE(s.totalWrapped() + quint64(s.size()), quint64(5000));
    }

    // ---------------- FrameListView ----------------
    void view_api()
    {
        FrameStore s(1000, 100);
        FrameStoreView v(&s);
        QVERIFY(v.isEmpty());
        QCOMPARE(v.at(0).frameType(), CommFrame::InvalidFrame);
        for (int i = 0; i < 300; i++) s.append(makeFrame(quint32(i), i % 65, true, true));
        QCOMPARE(v.count(), qsizetype(300));
        QCOMPARE(v.length(), qsizetype(300));
        QCOMPARE(v.at(5).frameId(), quint32(5));
        QCOMPARE(v.first().frameId(), quint32(0));
        QCOMPARE(v.last().frameId(), quint32(299));
        QCOMPARE(v.at(300).frameType(), CommFrame::InvalidFrame);
        QCOMPARE(v.at(-1).frameType(), CommFrame::InvalidFrame);
        QCOMPARE(v.mid(10, 5).size(), qsizetype(5));
        QCOMPARE(v.mid(10, 5).at(0).frameId(), quint32(10));
        QCOMPARE(v.mid(295).size(), qsizetype(5));
        QCOMPARE(v.tail(3).at(0).frameId(), quint32(297));
        QCOMPARE(v.tail(1000).size(), qsizetype(300));
        QCOMPARE(v.toVector().size(), qsizetype(300));
        QCOMPARE(v.at(64).payload().size(), 64);
        QVERIFY(v.at(64).hasBitrateSwitch());
    }

    // ---------------- CommFrameModel ----------------
    void model_rowsPublishedOnTickWithoutReset()
    {
        CommFrameModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy inserted(&m, &QAbstractItemModel::rowsInserted);
        QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
        m.addFrames(nullptr, batch(0x100, 50));
        m.addFrames(nullptr, batch(0x200, 50));
        QCOMPARE(m.rowCount(), 0);                 // not yet announced
        QCOMPARE(m.getFilteredListReference()->count(), qsizetype(100)); // but readable by windows
        QCOMPARE(m.sendBulkRefresh(), 100);
        QCOMPARE(m.rowCount(), 100);
        QCOMPARE(inserted.count(), 1);             // one insert for the whole tick
        QCOMPARE(inserted.at(0).at(1).toInt(), 0);
        QCOMPARE(inserted.at(0).at(2).toInt(), 99);
        QCOMPARE(reset.count(), 0);                // no model reset per tick (M9)
        QCOMPARE(m.sendBulkRefresh(), 0);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(rowId(m, 0), quint32(0x100));
        QCOMPARE(rowId(m, 99), quint32(0x231));
    }

    void model_wrapRemovesRows()
    {
        CommFrameModel m; // MaximumFrames 1000 -> chunks of 256
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy removed(&m, &QAbstractItemModel::rowsRemoved);
        QSignalSpy trimmed(&m, &CommFrameModel::framesTrimmed);
        quint32 id = 0;
        for (int b = 0; b < 50; b++)
        {
            m.addFrames(nullptr, batch(id, 100));
            id += 100;
            m.sendBulkRefresh();
            QVERIFY(m.rowCount() <= 1000);
            QCOMPARE(qsizetype(m.rowCount()), m.getFilteredListReference()->count());
        }
        QVERIFY(removed.count() > 0);
        QVERIFY(trimmed.count() > 0);
        QCOMPARE(m.totalFrameCount(), m.rowCount());
        QCOMPARE(rowId(m, m.rowCount() - 1), id - 1);
        for (int r = 1; r < m.rowCount(); r++) QCOMPARE(rowId(m, r), rowId(m, r - 1) + 1);
        QCOMPARE(m.framesWrapped() + quint64(m.totalFrameCount()), quint64(id));
    }

    void model_wrapBetweenTicks()
    {
        // more than the whole window arrives between two GUI ticks: rows never announced are dropped
        // silently, announced ones with rowsRemoved; nothing reads freed frames
        CommFrameModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
        m.addFrames(nullptr, batch(0, 500));
        m.sendBulkRefresh();
        m.addFrames(nullptr, batch(500, 3000));
        for (int r = 0; r < m.rowCount(); r++) m.data(m.index(r, int(Column::Data)), Qt::DisplayRole);
        m.sendBulkRefresh();
        QVERIFY(m.rowCount() <= 1000);
        QCOMPARE(rowId(m, m.rowCount() - 1), quint32(3499));
    }

    void model_filteredIsIndexList()
    {
        CommFrameModel m;
        QVector<CommFrame> v;
        for (int i = 0; i < 300; i++) v.append(makeFrame(i % 3 == 0 ? 0x10 : 0x20, 8, false, false, 0, i));
        m.addFrames(nullptr, v);
        m.sendBulkRefresh();
        QCOMPARE(m.rowCount(), 300);
        m.setFilterState(0x20, false);
        QCOMPARE(m.rowCount(), 100);
        for (int r = 0; r < m.rowCount(); r++) QCOMPARE(rowId(m, r), quint32(0x10));
        QCOMPARE(m.getFilteredListReference()->count(), qsizetype(100));
        QCOMPARE(m.getListReference()->count(), qsizetype(300));
        // new frames of the hidden ID stay out, others come in
        m.addFrames(nullptr, {makeFrame(0x20, 8), makeFrame(0x10, 8)});
        m.sendBulkRefresh();
        QCOMPARE(m.rowCount(), 101);
        m.setFilterState(0x20, true);
        QCOMPARE(m.rowCount(), 302);
    }

    void model_overwriteMode()
    {
        CommFrameModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVector<CommFrame> v;
        for (int i = 0; i < 90; i++) v.append(makeFrame(0x100 + i % 3, 8, false, false, 0, i * 10));
        m.addFrames(nullptr, v);
        m.sendBulkRefresh();
        m.setOverwriteMode(true);
        QCOMPARE(m.rowCount(), 3);
        QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
        m.addFrames(nullptr, {makeFrame(0x100, 8, false, false, 0, 1000), makeFrame(0x103, 8, false, false, 0, 1001)});
        m.sendBulkRefresh();
        QCOMPARE(m.rowCount(), 4);
        QVERIFY(changed.count() >= 1);
        QCOMPARE(m.data(m.index(0, int(Column::FrameCount)), Qt::DisplayRole).toString(), QString("31"));
        QCOMPARE(m.getFilteredListReference()->count(), qsizetype(4));
        // overwrite rows survive the ring wrapping underneath
        for (int b = 0; b < 30; b++) { m.addFrames(nullptr, batch(0x100, 3)); m.sendBulkRefresh(); }
        m.addFrames(nullptr, batch(0, 2000));
        m.sendBulkRefresh();
        QVERIFY(m.rowCount() >= 4);
        m.setOverwriteMode(false);
        QCOMPARE(m.rowCount(), m.totalFrameCount());
    }

    void model_sortThenWrap()
    {
        CommFrameModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVector<CommFrame> v;
        for (int i = 0; i < 600; i++) v.append(makeFrame(quint32((i * 37) % 600), 8, false, false, 0, i));
        m.addFrames(nullptr, v);
        m.sendBulkRefresh();
        m.sortByColumn(int(Column::FrameId)); // ascending
        for (int r = 1; r < m.rowCount(); r++) QVERIFY(rowId(m, r - 1) <= rowId(m, r));
        m.addFrames(nullptr, batch(5000, 900));   // wraps
        m.sendBulkRefresh();
        QVERIFY(m.rowCount() <= 1000);
        for (int r = 0; r < m.rowCount(); r++)
            QVERIFY(m.data(m.index(r, int(Column::FrameId)), Qt::DisplayRole).isValid());
    }

    void model_clearAndInsertGrows()
    {
        CommFrameModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
        m.insertFrames(batch(0, 3000));     // a loaded file is kept whole
        QCOMPARE(m.rowCount(), 3000);
        QCOMPARE(m.totalFrameCount(), 3000);
        QCOMPARE(rowId(m, 0), quint32(0));
        m.clearFrames();
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(m.getListReference()->count(), qsizetype(0));
        QCOMPARE(m.storeCapacity(), qsizetype(1000));
        m.addFrames(nullptr, batch(0, 3000));
        m.sendBulkRefresh();
        QVERIFY(m.rowCount() <= 1000);
    }

    void model_fdFrames()
    {
        CommFrameModel m;
        CommFrame f = makeFrame(0x123, 64, true, true, 1, 5);
        f.setErrorStateIndicator(true);
        m.addFrames(nullptr, {f, makeFrame(0x124, 12, true, false, 0, 6), makeFrame(0x125, 8, false, false, 0, 7)});
        m.sendBulkRefresh();
        QCOMPARE(m.data(m.index(0, int(Column::Type)), Qt::DisplayRole).toString(), QString("DBE"));
        QCOMPARE(m.data(m.index(0, int(Column::Length)), Qt::DisplayRole).toString(), QString("64"));
        QCOMPARE(m.data(m.index(1, int(Column::Type)), Qt::DisplayRole).toString(), QString("D"));
        QCOMPARE(m.data(m.index(2, int(Column::Type)), Qt::DisplayRole).toString(), QString("C"));
        const CommFrame back = m.getListReference()->at(0);
        QCOMPARE(back.payload(), f.payload());
        QVERIFY(back.hasBitrateSwitch());
        QVERIFY(back.hasErrorStateIndicator());
        QCOMPARE(back.getBus(), 1);
    }

    // A reader thread (as the frame sender) reads the views while the GUI thread appends, wraps,
    // filters and clears. Run under TSan.
    void model_concurrentReaders()
    {
        CommFrameModel m;
        std::atomic<bool> stop{false};
        std::atomic<qint64> reads{0}, bad{0};
        QThread *reader = QThread::create([&] {
            const FrameListView *all = m.getListReference();
            const FrameListView *rows = m.getFilteredListReference();
            while (!stop.load())
            {
                for (const CommFrame &f : rows->tail(64))
                    if (f.payload().size() != 8 || quint8(f.payload().at(0)) != quint8(f.frameId())) bad++;
                const qsizetype n = all->count();
                const CommFrame f = all->at(n / 2);
                if (f.frameType() != CommFrame::InvalidFrame && f.payload().size() != 8) bad++;
                (void)rows->last();
                reads++;
            }
        });
        reader->start();
        QElapsedTimer t;
        t.start();
        quint32 id = 0;
        int round = 0;
        while (t.elapsed() < 2000)
        {
            QVector<CommFrame> v;
            for (int i = 0; i < 200; i++, id++) v.append(makeFrame(id & 0x7FF, 8, false, false, 0, id, false, quint8(id & 0x7FF)));
            m.addFrames(nullptr, v);
            m.sendBulkRefresh();
            if (++round % 50 == 0) m.clearFrames();
            if (round % 70 == 0) { m.setFilterState(5, false); m.setFilterState(5, true); }
        }
        stop = true;
        reader->wait();
        delete reader;
        qInfo() << "concurrent reads:" << reads.load() << "frames written:" << id;
        QCOMPARE(bad.load(), qint64(0));
        QVERIFY(reads.load() > 0);
    }
};

QTEST_MAIN(TestFrameStore)
#include "tst_framestore.moc"
