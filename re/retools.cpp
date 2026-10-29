#include "retools.h"
#include "utility.h"
#include "theme.h"

#include <algorithm>
#include <bitset>
#include <cstdlib>

namespace ReTools {

bool sameFrame(const CommFrame &a, const CommFrame &b)
{
    return a.timeStamp().seconds() == b.timeStamp().seconds() && a.timeStamp().microSeconds() == b.timeStamp().microSeconds()
           && a.frameId() == b.frameId() && a.hasExtendedFrameFormat() == b.hasExtendedFrameFormat() && a.getBus() == b.getBus()
           && a.frameType() == b.frameType() && a.payload() == b.payload();
}

QColor byteColor(int byte)
{
    static const QColor light[8] = {
        Qt::blue, Qt::green,  Qt::black, Qt::red, //0 1 2 3
        Qt::gray, Qt::darkYellow, Qt::cyan,  Qt::darkMagenta, //4 5 6 7
    };
    static const QColor dark[8] = {
        QColor(0x6c, 0x9c, 0xff), QColor(0x40, 0xe0, 0x40), QColor(0xd0, 0xd0, 0xd0), QColor(0xff, 0x60, 0x60),
        QColor(0xa0, 0xa0, 0xa0), QColor(0xe0, 0xc8, 0x40), QColor(Qt::cyan), QColor(0xe0, 0x70, 0xe0),
    };
    return (Theme::isDark() ? dark : light)[byte & 7];
}

// ---------------------------------------------------------------- NewFrames

void NewFrames::markTaken(const FrameListView *list)
{
    if (list->isEmpty()) { mHave = false; return; }
    remember(list->last());
}

QVector<CommFrame> NewFrames::take(const FrameListView *list, int numFrames)
{
    const qsizetype count = list->count();
    if (count == 0 || numFrames <= 0) return {};
    // The n new frames are the newest rows (fewer if the ring dropped some). Rows already taken (a filtered list's
    // tail, rows read by a rebuild) are told apart by the newest frame taken.
    qsizetype start = qMax<qsizetype>(0, count - numFrames);
    if (mHave)
    {
        const qsizetype before = count - numFrames - 1;
        if (before >= 0 && sameFrame(list->at(before), mLast))
            start = before + 1;
        else
        {
            for (qsizetype i = count - 1; i >= qMax<qsizetype>(0, before); i--)
            {
                if (sameFrame(list->at(i), mLast))
                {
                    start = i + 1;
                    break;
                }
            }
        }
    }
    if (start >= count) return {};
    QVector<CommFrame> fresh = list->mid(start, count - start);
    if (!fresh.isEmpty()) remember(fresh.last());
    return fresh;
}

// ---------------------------------------------------------------- IdFrames

void IdFrames::clear()
{
    mStride = 0;
    mDropped = 0;
    std::vector<quint8>().swap(mBytes);
    std::vector<qint64>().swap(mTime);
    std::vector<quint8>().swap(mLen);
}

void IdFrames::restride(int stride)
{
    std::vector<quint8> bytes;
    if (mMax > 0) bytes.reserve(mLen.capacity() * size_t(stride));
    bytes.resize(size_t(size()) * size_t(stride), 0);
    for (int i = 0; i < size(); i++)
        std::copy_n(mBytes.data() + size_t(i) * size_t(mStride), mStride, bytes.data() + size_t(i) * size_t(stride));
    mBytes.swap(bytes);
    mStride = stride;
}

void IdFrames::append(qint64 timeUs, const quint8 *data, int len)
{
    len = qBound(0, len, 64);
    if (len > mStride) restride(len);
    if (mMax > 0 && size() >= mMax)
    {
        // A ring of the newest mMax frames: the oldest eighth goes at once (one move per mMax / 8 frames).
        const int drop = qMax(1, mMax / 8);
        mBytes.erase(mBytes.begin(), mBytes.begin() + ptrdiff_t(drop) * mStride);
        mTime.erase(mTime.begin(), mTime.begin() + drop);
        mLen.erase(mLen.begin(), mLen.begin() + drop);
        mDropped += drop;
    }
    if (mMax > 0 && mLen.size() == mLen.capacity())
    {
        // a bounded ring grows to at most mMax frames (no vector doubling past the bound)
        const size_t frames = size_t(qMin(mMax, qMax(1024, 2 * size())));
        mBytes.reserve(frames * size_t(mStride));
        mTime.reserve(frames);
        mLen.reserve(frames);
    }
    const size_t at = mBytes.size();
    mBytes.resize(at + size_t(mStride), 0);
    if (len) std::copy_n(data, len, mBytes.data() + at);
    mTime.push_back(timeUs);
    mLen.push_back(quint8(len));
}

void IdFrames::halve()
{
    int kept = 0;
    for (int i = 0; i < size(); i += 2, kept++)
    {
        if (kept == i) continue;
        std::copy_n(data(i), mStride, mBytes.data() + size_t(kept) * size_t(mStride));
        mTime[size_t(kept)] = mTime[size_t(i)];
        mLen[size_t(kept)] = mLen[size_t(i)];
    }
    mBytes.resize(size_t(kept) * size_t(mStride));
    mTime.resize(size_t(kept));
    mLen.resize(size_t(kept));
    mBytes.shrink_to_fit();
    mTime.shrink_to_fit();
    mLen.shrink_to_fit();
}

// ---------------------------------------------------------------- Range State

static inline qint64 value(const IdFrames &frames, int i, int startBit, int bitLength, bool bigEndian, bool isSigned)
{
    return Utility::processIntegerSignal(frames.payload(i), startBit, bitLength, !bigEndian, isSigned);
}

// SavvyCAN's RangeStateWindow::processSignal(), line for line, with 64-bit values (its int overflowed at 31+ bits:
// maxRange 1 << bitLength, the scaled values and differences), the values read once instead of twice.
bool rangeSignalLooksGood(const IdFrames &frames, int startBit, int bitLength, int sensitivity, bool bigEndian, bool isSigned)
{
    const int numFrames = frames.size();
    if (numFrames < 1) return false;
    const double lerpPoint = (double(sensitivity) - 10.0) / 240.0;

    std::vector<qint64> vals(static_cast<size_t>(numFrames));
    qint64 highestValue = -1000000000000LL, lowestValue = 1000000000000LL;
    for (int i = 0; i < numFrames; i++)
    {
        const qint64 v = value(frames, i, startBit, bitLength, bigEndian, isSigned);
        vals[size_t(i)] = v;
        if (v < lowestValue) lowestValue = v;
        if (v > highestValue) highestValue = v;
    }
    if (lowestValue == highestValue) return false; //a signal that never changes is worthless and not a range signal

    const qint64 range = highestValue - lowestValue;
    const qint64 maxRange = isSigned ? (qint64(1) << (bitLength - 1)) : (qint64(1) << bitLength);
    //at highest sensitivity require signal to at least range 20% of max range
    //at lowest  sensitivity require signal to at least range 1%  of max range
    const qint64 requiredRange = qint64(Utility::Lerp(maxRange * 0.01, maxRange * 0.2, lerpPoint));
    if (range < requiredRange) return false; //doesn't range enough.

    // first order differences (of the values less the lowest, as SavvyCAN: the same differences)
    int comparisonValue = int(Utility::Lerp(double(range) * 0.55, 0, lerpPoint));
    int overValues = 0;
    for (int i = 1; i < numFrames; i++)
        if (std::llabs(vals[size_t(i - 1)] - vals[size_t(i)]) > comparisonValue) overValues++;
    int maxOvers = int(Utility::Lerp(numFrames / 30.0, 2, lerpPoint));
    if (overValues > maxOvers) return false;

    //now look at the second order differentials. This is acceleration. There shouldn't be hard acceleration in values
    //for a ranging signal
    comparisonValue = int(Utility::Lerp(double(range) * 0.20, 1, lerpPoint));
    maxOvers = int(Utility::Lerp(8, 2, lerpPoint)); //really clamp down on second order over limits
    overValues = 0;
    for (int i = 2; i < numFrames; i++)
    {
        const qint64 d1a = vals[size_t(i - 2)] - vals[size_t(i - 1)];
        const qint64 d1b = vals[size_t(i - 1)] - vals[size_t(i)];
        if (std::llabs(d1a - d1b) > comparisonValue) overValues++;
    }
    return overValues <= maxOvers;
}

void rangeSearch(quint32 id, const IdFrames &frames, const RangeParams &p, std::vector<RangeCandidate> &out,
                 const std::function<void()> &keepAlive)
{
    if (frames.isEmpty() || p.granularity < 1) return;
    // SavvyCAN took the first frame's length; the longest is the same for a fixed-length ID and covers an ID whose
    // first frames were shorter (frames too short for a signal give 0, as SavvyCAN's processIntegerSignal does).
    const int maxBits = frames.maxLength() * 8;
    for (int sigSize = p.maxBits; sigSize >= p.minBits; sigSize -= p.granularity)
    {
        if (keepAlive) keepAlive();
        for (int startBit = 0; startBit < maxBits; startBit += p.granularity)
        {
            const auto test = [&](bool bigEndian, bool isSigned) {
                if (rangeSignalLooksGood(frames, startBit, sigSize, p.sensitivity, bigEndian, isSigned))
                    out.push_back({id, startBit, sigSize, isSigned, bigEndian});
            };
            if (p.sigType & 1)
            {
                if (p.signedType & 1) test(true, true);
                if (p.signedType & 2) test(true, false);
            }
            if (p.sigType & 2)
            {
                if (p.signedType & 1) test(false, true);
                if (p.signedType & 2) test(false, false);
            }
        }
    }
}

QVector<double> signalValues(const IdFrames &frames, int startBit, int bitLength, bool bigEndian, bool isSigned)
{
    QVector<double> v(frames.size());
    for (int i = 0; i < frames.size(); i++) v[i] = double(value(frames, i, startBit, bitLength, bigEndian, isSigned));
    return v;
}

// ---------------------------------------------------------------- Single/Multi State

void varyingBits(const IdFrames &frames, quint8 mask[64])
{
    std::fill_n(mask, 64, quint8(0));
    if (frames.isEmpty()) return;
    const quint8 *first = frames.data(0);
    const int stride = frames.maxLength();
    for (int i = 1; i < frames.size(); i++)
    {
        const quint8 *d = frames.data(i);
        for (int b = 0; b < stride; b++) mask[b] |= quint8(d[b] ^ first[b]);
    }
}

static inline bool bitSet(const quint8 mask[64], int bit) { return bit < 512 && (mask[bit / 8] >> (bit % 8)) & 1; }

// Does a field contain one found already (a smaller one: the fields are tried smallest first)? Then it is only that
// field with more bits around it and is not reported.
static bool containsFound(const std::vector<DiscreteMatch> &out, size_t from, int start, int bits)
{
    for (size_t i = from; i < out.size(); i++)
        if (out[i].startBit >= start && out[i].startBit + out[i].bits <= start + bits) return true;
    return false;
}

// A little-endian field of at most 8 bits (Intel numbering); -1 if the frame is too short for it.
static inline int field(const quint8 *d, int len, int startBit, int bits)
{
    if ((startBit + bits + 7) / 8 > len) return -1;
    int v = 0;
    for (int k = 0; k < bits; k++)
    {
        const int bit = startBit + k;
        if ((d[bit / 8] >> (bit % 8)) & 1) v |= 1 << k;
    }
    return v;
}

void discreteLogged(quint32 id, bool ext, const IdFrames &frames, int states, int minBits, int maxBits,
                    std::vector<DiscreteMatch> &out)
{
    if (frames.size() < 2) return;
    minBits = qBound(1, minBits, 8);
    maxBits = qBound(minBits, maxBits, 8);
    quint8 mask[64];
    varyingBits(frames, mask);
    const int totalBits = frames.maxLength() * 8;
    const size_t first = out.size();
    for (int bits = minBits; bits <= maxBits; bits++)
    {
        for (int start = 0; start + bits <= totalBits; start++)
        {
            if (!bitSet(mask, start) || !bitSet(mask, start + bits - 1)) continue; // not the tightest field
            if (containsFound(out, first, start, bits)) continue;
            std::bitset<256> seen;
            int unique = 0, n = 0;
            for (int i = 0; i < frames.size() && unique <= states; i++)
            {
                const int v = field(frames.data(i), frames.length(i), start, bits);
                if (v < 0) continue;
                n++;
                if (!seen[size_t(v)]) { seen[size_t(v)] = true; unique++; }
            }
            if (unique != states) continue;
            DiscreteMatch m;
            m.id = id;
            m.ext = ext;
            m.startBit = start;
            m.bits = bits;
            m.frames = n;
            for (int v = 0; v < 256; v++) if (seen[size_t(v)]) m.values.push_back(v);
            out.push_back(m);
        }
    }
}

void discreteRealtime(quint32 id, bool ext, const std::vector<IdFrames> &perState, int minBits, int maxBits,
                      std::vector<DiscreteMatch> &out, double minShare, int minFramesPerState)
{
    const int states = int(perState.size());
    if (states < 2) return;
    minBits = qBound(1, minBits, 8);
    maxBits = qBound(minBits, maxBits, 8);
    // The bits that vary over all the states' frames together.
    quint8 mask[64] = {};
    int stride = 0;
    const quint8 *ref = nullptr;
    for (const IdFrames &f : perState)
    {
        if (f.size() < minFramesPerState) return; // too few frames of this ID in a state to tell
        stride = qMax(stride, f.maxLength());
    }
    std::vector<quint8> firstRow(size_t(stride), 0);
    std::copy_n(perState[0].data(0), perState[0].maxLength(), firstRow.data());
    ref = firstRow.data();
    for (const IdFrames &f : perState)
        for (int i = 0; i < f.size(); i++)
        {
            const quint8 *d = f.data(i);
            for (int b = 0; b < f.maxLength(); b++) mask[b] |= quint8(d[b] ^ ref[b]);
        }
    const int totalBits = stride * 8;
    std::vector<int> hist(256);
    const size_t first = out.size();
    for (int bits = minBits; bits <= maxBits; bits++)
    {
        for (int start = 0; start + bits <= totalBits; start++)
        {
            if (!bitSet(mask, start) || !bitSet(mask, start + bits - 1)) continue;
            if (containsFound(out, first, start, bits)) continue;
            DiscreteMatch m;
            m.id = id;
            m.ext = ext;
            m.startBit = start;
            m.bits = bits;
            bool ok = true;
            for (int s = 0; s < states && ok; s++)
            {
                std::fill(hist.begin(), hist.end(), 0);
                const IdFrames &f = perState[size_t(s)];
                int n = 0;
                for (int i = 0; i < f.size(); i++)
                {
                    const int v = field(f.data(i), f.length(i), start, bits);
                    if (v < 0) continue;
                    hist[size_t(v)]++;
                    n++;
                }
                if (n < minFramesPerState) { ok = false; break; }
                const int best = int(std::max_element(hist.begin(), hist.end()) - hist.begin());
                const double share = double(hist[size_t(best)]) / n;
                if (share < minShare) { ok = false; break; }
                if (std::find(m.values.begin(), m.values.end(), best) != m.values.end()) { ok = false; break; } // same value as another state
                m.values.push_back(best);
                m.share = qMin(m.share, share);
                m.frames += n;
            }
            if (ok) out.push_back(m);
        }
    }
}

// ---------------------------------------------------------------- StateSampler

void StateSampler::start(int states)
{
    mEntries.clear();
    mStates = qMax(1, states);
    mCap = mCapStart;
    mAdded = 0;
    mBytes = 0;
}

void StateSampler::add(const CommFrame &f, int state)
{
    if (state < 0 || state >= mStates || !hasData(f)) return;
    const quint64 key = quint64(f.frameId()) | (f.hasExtendedFrameFormat() ? (quint64(1) << 32) : 0);
    auto it = mEntries.find(key);
    if (it == mEntries.end())
    {
        Entry e;
        e.id = f.frameId();
        e.ext = f.hasExtendedFrameFormat();
        e.perState.assign(size_t(mStates), IdFrames());
        e.stride.assign(size_t(mStates), 1);
        e.skip.assign(size_t(mStates), 0);
        it = mEntries.insert(key, e);
    }
    mAdded++;
    Entry &e = it.value();
    const size_t s = size_t(state);
    if (e.skip[s] > 0) { e.skip[s]--; return; }  // an even spread: every stride-th frame
    e.skip[s] = e.stride[s] - 1;
    IdFrames &buf = e.perState[s];
    const size_t before = buf.memoryBytes();
    buf.append(f);
    if (buf.size() >= mCap)
    {
        buf.halve();
        e.stride[s] *= 2;
    }
    mBytes += buf.memoryBytes() - qMin(before, buf.memoryBytes());
    if ((mAdded & 4095) == 0) mBytes = memoryBytes(); // the true sum now and then (halving gives memory back)
    if (mBytes > mBudget && mCap > 16)
    {
        // Over the budget: every buffer keeps half its frames, and the cap halves.
        mCap /= 2;
        for (Entry &x : mEntries)
            for (int k = 0; k < mStates; k++)
                if (x.perState[size_t(k)].size() >= mCap)
                {
                    x.perState[size_t(k)].halve();
                    x.stride[size_t(k)] *= 2;
                }
        mBytes = memoryBytes();
    }
}

size_t StateSampler::memoryBytes() const
{
    size_t n = 0;
    for (const Entry &e : mEntries)
        for (const IdFrames &f : e.perState) n += f.memoryBytes();
    return n;
}

qint64 StateSampler::framesKept() const
{
    qint64 n = 0;
    for (const Entry &e : mEntries)
        for (const IdFrames &f : e.perState) n += f.size();
    return n;
}

} // namespace ReTools
