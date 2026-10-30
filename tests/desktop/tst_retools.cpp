// The live reverse-engineering tools' shared code (re/retools.cpp, stage 4) and the sniffer's per-ID item, natively
// (also under ASan/UBSan and TSan with tests/desktop/run.sh address|thread):
// - NewFrames: every frame of a ring-buffer trace taken exactly once, across the ring's wraps, with ticks larger
//   than the ring;
// - IdFrames: compact rows, the bound of a ring, a longer frame later (CAN FD), halving;
// - rangeSearch: the planted signals found, constants and noise not, start bits past 255 (CAN FD), and the same
//   candidates as SavvyCAN's processIntegerSignal-based test for random frames;
// - discreteLogged / discreteRealtime: exactly the planted fields (tightest only), multi-state values, a contaminated
//   state still found, counters and noise not;
// - StateSampler: within its budget, an even spread;
// - SnifferItem: frames whose length changes 8 <-> 64 (every buffer initialised), notch masks changes.
#include <QtTest>
#include <random>

#include "framestore.h"
#include "re/retools.h"
#include "re/sniffer/snifferitem.h"
#include "utility.h"
#include "testframes.h"

using namespace ReTools;

namespace {

CommFrame frameWith(quint32 id, const QByteArray &data, qint64 tsUs, bool ext = false)
{
    CommFrame f = tf::makeFrame(id, data.size(), data.size() > 8, data.size() > 8, 0, tsUs, ext);
    f.setPayload(data);
    return f;
}

QByteArray bytes(std::initializer_list<int> v)
{
    QByteArray b;
    for (int x : v) b.append(char(quint8(x)));
    return b;
}

} // namespace

class TestReTools : public QObject
{
    Q_OBJECT
private slots:
    void newFramesAcrossWraps();
    void newFramesFilteredTail();
    void idFramesRingAndStride();
    void rangeFindsPlanted();
    void rangeMatchesSavvyCanTest();
    void discreteLoggedTightest();
    void discreteRealtimeStates();
    void samplerBudget();
    void snifferItemLengths();
};

// A trace of 1000 frames (chunks of 256): ticks of every size, some larger than the ring, all frames taken once.
void TestReTools::newFramesAcrossWraps()
{
    FrameStore store(1000, 256);
    FrameStoreView view(&store);
    NewFrames nf;
    std::mt19937 rng(7);
    quint32 sent = 0, taken = 0, expectNext = 0;
    for (int tick = 0; tick < 400; tick++)
    {
        const int n = int(rng() % (tick % 50 == 49 ? 3000 : 300));
        for (int i = 0; i < n; i++)
        {
            QByteArray d(4, 0);
            qToLittleEndian<quint32>(sent, d.data());
            store.append(frameWith(0x100 + (sent % 7), d, qint64(sent) * 100));
            sent++;
        }
        const QVector<CommFrame> fresh = nf.take(&view, n);
        for (const CommFrame &f : fresh)
        {
            const quint32 seq = qFromLittleEndian<quint32>(f.payload().constData());
            // frames the ring dropped before they were taken are skipped; none twice, none out of order
            QVERIFY2(seq >= expectNext, qPrintable(QString("seq %1 after %2").arg(seq).arg(expectNext)));
            expectNext = seq + 1;
            taken++;
        }
        if (n <= int(store.size()) - 256) QCOMPARE(expectNext, sent); // a tick the ring holds: every frame of it
    }
    QVERIFY(store.totalWrapped() > 50000);
    QVERIFY(taken <= sent);
    QCOMPARE(expectNext, sent);
}

// A list whose last rows were already taken (a window that read the whole list, then a tick for the same frames).
void TestReTools::newFramesFilteredTail()
{
    FrameStore store(100000, 0);
    FrameStoreView view(&store);
    NewFrames nf;
    for (int i = 0; i < 10; i++) store.append(frameWith(0x200, bytes({i}), i));
    nf.markTaken(&view);
    QCOMPARE(nf.take(&view, 10).size(), 0);   // the same 10 announced later: nothing new
    for (int i = 10; i < 15; i++) store.append(frameWith(0x200, bytes({i}), i));
    const QVector<CommFrame> fresh = nf.take(&view, 8); // announced as 8 (3 already read): only the 5 new
    QCOMPARE(fresh.size(), 5);
    QCOMPARE(int(quint8(fresh.first().payload()[0])), 10);
}

void TestReTools::idFramesRingAndStride()
{
    IdFrames f(1000);
    for (int i = 0; i < 2500; i++)
    {
        QByteArray d(i == 1700 ? 64 : 8, char(i & 0xff));
        f.append(frameWith(0x300, d, i));
    }
    QVERIFY(f.size() <= 1000);
    QVERIFY(f.size() >= 875);
    QCOMPARE(f.dropped() + f.size(), qint64(2500));
    QCOMPARE(f.maxLength(), 64);
    QCOMPARE(f.timeUs(f.size() - 1), qint64(2499));
    QCOMPARE(f.length(f.size() - 1), 8);
    QCOMPARE(int(f.data(f.size() - 1)[0]), 2499 & 0xff);
    QCOMPARE(int(f.data(f.size() - 1)[8]), 0);      // past its length: zeros
    const int at1700 = int(1700 - f.dropped());
    QCOMPARE(f.length(at1700), 64);
    QCOMPARE(int(f.data(at1700)[63]), 1700 & 0xff);
    QVERIFY2(f.memoryBytes() <= 1000u * (64 + 9), qPrintable(QString::number(f.memoryBytes()))); // no doubling past the bound

    IdFrames h;
    for (int i = 0; i < 101; i++) h.append(i, reinterpret_cast<const quint8 *>("\x01\x02"), 2);
    h.halve();
    QCOMPARE(h.size(), 51);
    QCOMPARE(h.timeUs(1), qint64(2));
    QCOMPARE(h.timeUs(50), qint64(100));
}

// A triangle in bytes 0-1 (16-bit little endian), constants, a random byte; and in a 64-byte frame the same triangle
// at start bit 400.
void TestReTools::rangeFindsPlanted()
{
    IdFrames classic, fd;
    std::mt19937 rng(3);
    for (int k = 0; k < 2000; k++)
    {
        const int tri = 1000 + 10 * std::abs(((k + 200) % 400) - 200);
        classic.append(frameWith(0x121, bytes({tri & 0xff, tri >> 8, 0x5a, int(rng() & 0xff), 0, 0, 0, 0}), k * 10000));
        QByteArray d(64, 0);
        d[50] = char(tri & 0xff);
        d[51] = char(tri >> 8);
        d[63] = char(0xa5);
        fd.append(frameWith(0x18ff0001, d, k * 10000, true));
    }
    RangeParams p;
    p.minBits = 16;
    p.maxBits = 16;
    p.granularity = 8;
    p.sigType = 2;    // little endian
    p.signedType = 2; // unsigned
    p.sensitivity = 10; // the window's default
    std::vector<RangeCandidate> out;
    rangeSearch(0x121, classic, p, out);
    QVERIFY(std::any_of(out.begin(), out.end(), [](const RangeCandidate &c) { return c.startBit == 0 && c.bits == 16; }));
    for (const RangeCandidate &c : out) QVERIFY2(c.startBit < 16 || c.startBit >= 32, "nothing from the constant or random byte alone");
    out.clear();
    rangeSearch(0x18ff0001, fd, p, out);
    QVERIFY(std::any_of(out.begin(), out.end(), [](const RangeCandidate &c) { return c.startBit == 400 && c.bits == 16 && c.id == 0x18ff0001; }));
    for (const RangeCandidate &c : out) QVERIFY2(c.startBit >= 384 && c.startBit <= 408, "only around the triangle (bytes 48..52)");
    // the graph's values: the triangle
    const QVector<double> v = signalValues(fd, 400, 16, false, false);
    QCOMPARE(*std::min_element(v.begin(), v.end()), 1000.0);
    QCOMPARE(*std::max_element(v.begin(), v.end()), 3000.0);
}

// SavvyCAN's processSignal() as it was (32-bit arithmetic is fine for these sizes), on random walks: the same answer.
static bool savvycanProcessSignal(const QVector<QByteArray> &frames, int startBit, int bitLength, int sensitivity, bool bigEndian, bool isSigned)
{
    QVector<int> scaledVals, diff1, diff2;
    int64_t highestValue = -1000000000000LL, lowestValue = 1000000000000LL;
    const double lerpPoint = ((double)sensitivity - 10.0) / 240.0;
    const int numFrames = frames.count();
    for (int i = 0; i < numFrames; i++)
    {
        const int64_t valu = Utility::processIntegerSignal(frames[i], startBit, bitLength, !bigEndian, isSigned);
        lowestValue = qMin(lowestValue, valu);
        highestValue = qMax(highestValue, valu);
    }
    if (lowestValue == highestValue) return false;
    const int64_t range = highestValue - lowestValue;
    const int64_t maxRange = isSigned ? (1 << (bitLength - 1)) : (1 << bitLength);
    const int64_t requiredRange = Utility::Lerp(maxRange * 0.01, maxRange * 0.2, lerpPoint);
    if (range < requiredRange) return false;
    for (int i = 0; i < numFrames; i++)
        scaledVals.append((int)(Utility::processIntegerSignal(frames[i], startBit, bitLength, !bigEndian, isSigned) - lowestValue));
    for (int i = 1; i < numFrames; i++) diff1.append(scaledVals[i - 1] - scaledVals[i]);
    for (int i = 1; i < diff1.count(); i++) diff2.append(diff1[i - 1] - diff1[i]);
    int comparisonValue = Utility::Lerp((double)range * 0.55, 0, lerpPoint);
    int overValues = 0;
    for (int d : diff1) if (abs(d) > comparisonValue) overValues++;
    int maxOvers = Utility::Lerp(numFrames / 30.0, 2, lerpPoint);
    if (overValues > maxOvers) return false;
    comparisonValue = Utility::Lerp((double)range * 0.20, 1, lerpPoint);
    maxOvers = Utility::Lerp(8, 2, lerpPoint);
    overValues = 0;
    for (int d : diff2) if (abs(d) > comparisonValue) overValues++;
    return overValues <= maxOvers;
}

void TestReTools::rangeMatchesSavvyCanTest()
{
    std::mt19937 rng(11);
    for (int run = 0; run < 6; run++)
    {
        IdFrames frames;
        QVector<QByteArray> raw;
        int walk = 128;
        for (int k = 0; k < 400; k++)
        {
            walk = qBound(0, walk + int(rng() % 7) - 3, 255);
            QByteArray d = bytes({walk, int(rng() & 0xff), k & 0xff, 0x40, walk / 2, 0, int(k / 50), 7});
            raw.append(d);
            frames.append(frameWith(0x400, d, k));
        }
        for (int sens : {10, 50, 130, 250})
            for (int size : {1, 4, 8, 12, 16})
                for (int start = 0; start < 64; start += 3)
                    for (int big = 0; big < 2; big++)
                        for (int sgn = 0; sgn < 2; sgn++)
                            QCOMPARE(rangeSignalLooksGood(frames, start, size, sens, big, sgn),
                                     savvycanProcessSignal(raw, start, size, sens, big, sgn));
    }
}

void TestReTools::discreteLoggedTightest()
{
    IdFrames f;
    for (int k = 0; k < 300; k++)
    {
        const int state = (k / 25) % 3;       // a 3-state value in bits 26-27
        const int button = (k / 40) % 2;      // a button at bit 19
        f.append(frameWith(0x120, bytes({k & 0xff, 0x5a, button << 3, state << 2, 0x11, 0x22, 0x33, 0x44}), k));
    }
    std::vector<DiscreteMatch> out;
    discreteLogged(0x120, false, f, 3, 1, 8, out);
    // bits 26-27 take 0,1,2; a 1-bit field can't take 3 values; the counter byte's fields with exactly 3 values?
    bool found = false;
    for (const DiscreteMatch &m : out)
    {
        if (m.startBit == 26 && m.bits == 2) { found = true; QCOMPARE(m.values, std::vector<int>({0, 1, 2})); }
        QVERIFY2(m.startBit >= 8 || m.startBit + m.bits <= 8, "no field across the counter and the constant");
    }
    QVERIFY(found);
    out.clear();
    discreteLogged(0x120, false, f, 2, 1, 1, out);
    QVERIFY(std::any_of(out.begin(), out.end(), [](const DiscreteMatch &m) { return m.startBit == 19 && m.bits == 1; }));
    // the 3-state field's bits alone are 2-valued too; the constant bytes give nothing
    for (const DiscreteMatch &m : out) QVERIFY(m.startBit < 8 || m.startBit == 19 || m.startBit == 26 || m.startBit == 27);
}

void TestReTools::discreteRealtimeStates()
{
    std::mt19937 rng(5);
    // three states; per state 200 frames; 5 % of state 2's frames still in state 1 (the user was late)
    std::vector<IdFrames> perState(3);
    int k = 0;
    for (int s = 0; s < 3; s++)
        for (int i = 0; i < 200; i++, k++)
        {
            const int held = (s == 1 && i < 10) ? 0 : s;
            QByteArray d(64, 0);
            d[0] = char(k & 0xff);                // counter
            d[1] = char(rng() & 0xff);            // noise
            d[2] = char((held & 1) << 3);         // bit 19: 0,1,0
            d[3] = char((held & 3) << 2);         // bits 26-27: 0,1,2
            d[33] = char((held == 2) << 6);       // bit 270: 0,0,1
            perState[size_t(s)].append(frameWith(0x18ff0001, d, k, true));
        }
    std::vector<DiscreteMatch> out;
    discreteRealtime(0x18ff0001, true, perState, 1, 8, out);
    QCOMPARE(out.size(), size_t(1));
    QCOMPARE(out[0].startBit, 26);
    QCOMPARE(out[0].bits, 2);
    QCOMPARE(out[0].values, std::vector<int>({0, 1, 2}));
    QVERIFY(out[0].share >= 0.94 && out[0].share < 0.96);
    QVERIFY(out[0].ext);
    // two states (the first two): bit 19 and bit 26 both follow it
    out.clear();
    perState.resize(2);
    discreteRealtime(0x18ff0001, true, perState, 1, 8, out);
    QCOMPARE(out.size(), size_t(2));
    QCOMPARE(out[0].startBit, 19);
    QCOMPARE(out[1].startBit, 26);
    QCOMPARE(out[0].bits, 1);
}

void TestReTools::samplerBudget()
{
    StateSampler s(1u << 20, 512);   // 1 MB
    s.start(4);
    for (int i = 0; i < 400000; i++)
    {
        QByteArray d(64, char(i & 0xff));
        s.add(frameWith(0x100 + (i % 40), d, i, false), (i / 1000) % 4);
    }
    QVERIFY2(s.memoryBytes() <= (1u << 20) + 64 * 1024, qPrintable(QString::number(s.memoryBytes())));
    QCOMPARE(s.framesAdded(), qint64(400000));
    QCOMPARE(int(s.entries().size()), 40);
    // an even spread: the kept frames of one ID and state cover the whole run
    const StateSampler::Entry &e = s.entries()[quint64(0x100)];
    const IdFrames &f = e.perState[0];
    QVERIFY(f.size() > 16);
    QVERIFY(f.timeUs(0) < 2000);
    QVERIFY(f.timeUs(f.size() - 1) > 350000);
}

void TestReTools::snifferItemLengths()
{
    SnifferItem item(frameWith(0x7e8, bytes({1, 2, 3, 4, 5, 6, 7, 8}), 0), 0);
    QCOMPARE(item.getData(7), 8);
    QCOMPARE(item.getData(8), -1);
    QByteArray big(64, 0);
    big[0] = 1;
    big[40] = 0x77;
    item.update(frameWith(0x7e8, big, 1000), 1, false);
    QCOMPARE(item.getData(40), 0x77);
    QCOMPARE(item.getData(63), 0);
    item.updateMarker();
    QCOMPARE(int(item.dataChange(40)), int(dc::INC));
    QCOMPARE(int(item.dataChange(63)), int(dc::NO));
    item.update(frameWith(0x7e8, bytes({9}), 2000), 2, false);
    QCOMPARE(item.getData(1), -1);
    // notch: the bits that changed in the last marker period are masked
    item.updateMarker();
    item.notch(true);
    QVERIFY(item.getNotchPattern(0) != 0);
    item.notch(false);
    QCOMPARE(int(item.getNotchPattern(0)), 0);
    QVERIFY(item.getDelta() > 0.0009f && item.getDelta() < 0.0011f);
}

QTEST_GUILESS_MAIN(TestReTools)
#include "tst_retools.moc"
