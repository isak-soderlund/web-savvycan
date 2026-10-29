#ifndef RETOOLS_H
#define RETOOLS_H

/*
 * web-savvycan: what the live reverse-engineering tools (Flow View, Range State, Single/Multi State; stage 4) share,
 * without any UI, so tests/desktop can run it natively (tst_retools):
 * - NewFrames: the frames one framesUpdated(n) tick added to the trace. The trace is a ring (framestore.h): a row
 *   number does not stay valid as old frames go, and n may be more than the ring keeps. New frames are found by the
 *   newest frame taken, not by an index.
 * - IdFrames: the payloads (and times) of one ID's frames, compact (the ID's longest frame as the row size, no
 *   CommFrame or QByteArray per frame), optionally bounded as a ring of the newest frames.
 * - rangeSearch(): Range State's search (SavvyCAN's processSignal(), with 64-bit arithmetic and CAN FD start bits).
 * - discreteLogged(), discreteRealtime(): Single/Multi State's search, which SavvyCAN left empty.
 * - StateSampler: the frames Single/Multi State records per ID and state in real time, within a memory budget.
 */

#include "framestore.h"
#include "can_structs.h"

#include <QColor>
#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <vector>

namespace ReTools {

// A frame's time in µs (the trace keeps seconds and µs apart).
inline qint64 frameTimeUs(const CommFrame &f) { return f.timeStamp().seconds() * 1000000LL + f.timeStamp().microSeconds(); }

// A frame with an ID and data (not an error frame, not a remote frame).
inline bool hasData(const CommFrame &f) { return f.frameType() == CommFrame::CANDataFrame || f.frameType() == CommFrame::CANFDDataFrame; }

bool sameFrame(const CommFrame &a, const CommFrame &b);

// SavvyCAN's eight byte graph colours (they repeat every 8 bytes: CAN FD has up to 64); in dark, lighter ones and
// light grey for black. Frame Data Analysis and Flow View.
QColor byteColor(int byte);

class NewFrames
{
public:
    void reset() { mHave = false; }
    // Everything in the list now counts as taken (a window that has just read the whole list).
    void markTaken(const FrameListView *list);
    // The frames added since the last call (or markTaken), oldest first. numFrames: framesUpdated's count.
    QVector<CommFrame> take(const FrameListView *list, int numFrames);
private:
    void remember(const CommFrame &f) { mLast = f; mHave = true; }
    CommFrame mLast;
    bool mHave = false;
};

// One ID's frames: time, length and the payload in rows of the longest frame so far.
class IdFrames
{
public:
    explicit IdFrames(int maxFrames = 0) : mMax(maxFrames) {}   // 0: unbounded; else a ring of the newest maxFrames
    void clear();
    void append(const CommFrame &f) { append(frameTimeUs(f), reinterpret_cast<const quint8 *>(f.payload().constData()), int(f.payload().size())); }
    void append(qint64 timeUs, const quint8 *data, int len);
    int size() const { return int(mLen.size()); }
    bool isEmpty() const { return mLen.empty(); }
    qint64 timeUs(int i) const { return mTime[size_t(i)]; }
    int length(int i) const { return mLen[size_t(i)]; }
    const quint8 *data(int i) const { return mBytes.data() + size_t(i) * size_t(mStride); }
    QByteArray payload(int i) const { return QByteArray::fromRawData(reinterpret_cast<const char *>(data(i)), length(i)); } // no copy
    int maxLength() const { return mStride; }
    int maxFrames() const { return mMax; }
    qint64 dropped() const { return mDropped; }     // frames dropped from the front (oldest) since clear()
    size_t memoryBytes() const { return mBytes.capacity() + mTime.capacity() * sizeof(qint64) + mLen.capacity(); }
    // Keeps every other frame (Single/Multi State's sampler: uniform over time, half the memory).
    void halve();
private:
    void restride(int stride);
    int mMax;
    int mStride = 0;
    qint64 mDropped = 0;
    std::vector<quint8> mBytes;
    std::vector<qint64> mTime;
    std::vector<quint8> mLen;
};

// ---- Range State
struct RangeParams
{
    int minBits = 8, maxBits = 16, granularity = 1;
    int sigType = 3;      // 1 big endian, 2 little endian, 3 both (SavvyCAN's combo index + 1)
    int signedType = 3;   // 1 signed, 2 unsigned, 3 both
    int sensitivity = 50; // 10..250
};
struct RangeCandidate
{
    quint32 id = 0;
    int startBit = 0, bits = 0;
    bool isSigned = false, bigEndian = false;
};
// SavvyCAN's test of one signal: does it range smoothly enough?
bool rangeSignalLooksGood(const IdFrames &frames, int startBit, int bitLength, int sensitivity, bool bigEndian, bool isSigned);
// Every candidate of one ID, in SavvyCAN's order (largest size first, then start bit). keepAlive is called between
// sizes (the UI's event loop).
void rangeSearch(quint32 id, const IdFrames &frames, const RangeParams &p, std::vector<RangeCandidate> &out,
                 const std::function<void()> &keepAlive = {});
// The signal's values over the ID's frames (Range State's graph).
QVector<double> signalValues(const IdFrames &frames, int startBit, int bitLength, bool bigEndian, bool isSigned);

// ---- Single/Multi State
struct DiscreteMatch
{
    quint32 id = 0;
    bool ext = false;
    int startBit = 0, bits = 0;   // Intel (little-endian) bit numbering: bit = byte * 8 + bit in the byte
    std::vector<int> values;      // per state: the field's value (realtime), or the values seen (logged)
    double share = 1.0;           // realtime: the smallest share of a state's frames with that state's value
    int frames = 0;               // frames looked at
};
// The bits (Intel numbering) that are not the same in every frame: 64 bytes of masks.
void varyingBits(const IdFrames &frames, quint8 mask[64]);
// "From logged data": fields of minBits..maxBits bits that take exactly `states` values over the frames and whose
// first and last bits both change (the tightest field: a toggling bit is reported once, not in every wider field).
void discreteLogged(quint32 id, bool ext, const IdFrames &frames, int states, int minBits, int maxBits,
                    std::vector<DiscreteMatch> &out);
// "Realtime": perState[k] holds the frames recorded while the user held state k. A field matches when in every
// state at least minShare of its frames have one value, and those values differ from state to state.
void discreteRealtime(quint32 id, bool ext, const std::vector<IdFrames> &perState, int minBits, int maxBits,
                      std::vector<DiscreteMatch> &out, double minShare = 0.9, int minFramesPerState = 3);

// Frames per (ID, state) for the realtime search, bounded: each buffer keeps an even spread of at most perBufferCap
// frames (when full, every other frame goes and from then on every second is taken, and so on); all buffers together
// stay within budgetBytes (halving every buffer's cap when over).
class StateSampler
{
public:
    explicit StateSampler(size_t budgetBytes = 32u << 20, int perBufferCap = 2048) : mBudget(budgetBytes), mCapStart(perBufferCap) {}
    void start(int states);
    void add(const CommFrame &f, int state);
    int states() const { return mStates; }
    struct Entry { quint32 id; bool ext; std::vector<IdFrames> perState; std::vector<int> stride, skip; };
    const QHash<quint64, Entry> &entries() const { return mEntries; }
    size_t memoryBytes() const;
    qint64 framesAdded() const { return mAdded; }
    qint64 framesKept() const;
    int cap() const { return mCap; }
private:
    QHash<quint64, Entry> mEntries;
    size_t mBudget;
    int mCapStart, mCap = 0, mStates = 0;
    qint64 mAdded = 0;
    size_t mBytes = 0;
};

} // namespace ReTools

#endif // RETOOLS_H
