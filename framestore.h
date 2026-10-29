#ifndef FRAMESTORE_H
#define FRAMESTORE_H

/*
 * web-savvycan: the frame store behind CommFrameModel (fixes M3, M4, M5 of resolve-can
 * docs/research/savvycan-improve.md).
 *
 * FrameStore is a chunked ring: frames live in chunks of a fixed number of frames. When the store is
 * full, the oldest whole chunk is dropped (one free, no memmove of the rest). Every frame gets a
 * sequence number (seq) that only grows; the filtered view of the model is a list of seqs (8 bytes
 * per row instead of a second copy of the frame).
 *
 * Compact records (web-savvycan, the leaner log loader): a chunk keeps its frames packed one after the
 * other in one block of 8-byte words, not as CommFrames: the time (16 bytes: seconds as 32 bits, the
 * microseconds as 64), the frame count of overwrite mode, the ID / type / flags (CommFrame::packedBits),
 * the bus and the length (8 bytes), then the payload inline, rounded up to 8 bytes; plus a 4-byte offset
 * per frame. A classic 8-byte frame takes 36 bytes, a 64-byte CAN FD frame 92 (a CommFrame with its own
 * QByteArray took ~100 and ~160 in the browser, more on 64-bit). Frames are given out as CommFrames made
 * on access (bySeq(), atIndex(), the views): the same frame bit for bit, except the time delta of
 * overwrite mode (0; only overwrite mode's own rows have one). Payloads over 65535 bytes are cut there.
 *
 * FrameListView is the read-only, QVector-like face the rest of SavvyCAN sees
 * (CommFrameModel::getListReference() / getFilteredListReference()): count(), at(i),
 * isEmpty(), mid(), tail(), toVector(). Every call takes the store's read lock and
 * returns frames by value, so a reader on another thread (the frame sender) never sees a frame
 * being written, moved or freed. at() of a row that is gone (trimmed or cleared since the reader
 * took its count) returns an InvalidFrame instead of reading freed memory.
 *
 * Locking: the owner (the model, UI thread) takes the write lock for every change and never
 * emits a Qt signal while holding it. Readers on the owner's thread may read the store without
 * the lock (nothing else writes).
 */

#include <QReadWriteLock>
#include <QVector>
#include <deque>
#include <vector>

#include "can_structs.h"

class FrameStore
{
public:
    static constexpr qsizetype DefaultChunkFrames = 65536;

    // chunkFrames <= 0: chunk size follows the capacity (capacity / 16, 256 .. 65536), recomputed by
    // setCapacity() whenever the store is empty.
    explicit FrameStore(qsizetype capacityFrames = 1000000, qsizetype chunkFrames = 0);

    // ---- writer side (caller holds lock() for writing when other threads may read) ----

    // Maximum number of frames kept. Once it has wrapped the store holds between capacity - chunk
    // and capacity frames (a capacity below two chunks is exceeded up to two chunks).
    void setCapacity(qsizetype capacityFrames);
    qsizetype capacity() const { return mCapacity; }
    qsizetype chunkFrames() const { return mChunk; }

    // Append one frame. Returns the number of frames dropped from the front to make room (0 or a
    // whole chunk).
    qsizetype append(const CommFrame &frame);
    // The same with the payload given apart (the frame's own payload is not read): no QByteArray per frame
    // (the log loaders' fast path).
    qsizetype append(const CommFrame &header, const char *payload, qsizetype len);

    // Drop everything. Sequence numbers keep growing (a seq is never reused), so a reader holding
    // an old seq can tell it is gone.
    void clear();

    // The time of a stored frame changed in place (normalizeTiming). contains(seq) must be true.
    void setTimeStamp(quint64 seq, CommFrame::TimeStamp ts);

    // Drops the oldest chunk, whatever the capacity (a staging store handing its frames on, chunk by chunk).
    void dropFront() { if (!mChunks.empty()) dropFrontChunk(); }
    // One past the seq of the oldest chunk's last frame (= endSeq() with one chunk, = firstSeq() when empty).
    quint64 frontChunkEndSeq() const
    { return mChunks.empty() ? mFirstSeq : mFirstSeq + quint64(mChunks.front().offsets.size()); }

    // ---- reader side ----
    quint64 firstSeq() const { return mFirstSeq; }
    quint64 endSeq() const { return mEndSeq; }
    qsizetype size() const { return qsizetype(mEndSeq - mFirstSeq); }
    bool isEmpty() const { return mEndSeq == mFirstSeq; }
    bool contains(quint64 seq) const { return seq >= mFirstSeq && seq < mEndSeq; }
    CommFrame bySeq(quint64 seq) const; // unchecked: contains(seq) must be true. A copy, made from the record
    CommFrame atIndex(qsizetype i) const { return bySeq(mFirstSeq + quint64(i)); }
    CommFrame::TimeStamp timeStampBySeq(quint64 seq) const; // unchecked, as bySeq(), without making the frame
    // Bytes the chunks hold (allocated), for the tests' memory numbers.
    qsizetype memoryBytes() const;

    // Total frames ever appended and ever dropped by wrapping (since construction).
    quint64 totalAppended() const { return mTotalAppended; }
    quint64 totalWrapped() const { return mTotalWrapped; }

    QReadWriteLock &lock() const { return mLock; }

private:
    struct Chunk {
        std::vector<quint32> offsets; // per frame: its record's first word in words
        std::vector<quint64> words;   // the records (header 3 words, then the payload)
    };
    static constexpr int HeaderWords = 3;
    void dropFrontChunk();
    Chunk &chunkForAppend(qsizetype &dropped);
    const quint64 *record(quint64 seq) const;
    quint64 *record(quint64 seq);

    std::deque<Chunk> mChunks;
    quint64 mFirstSeq = 0;       // seq of mChunks.front()[0] (chunk aligned)
    quint64 mEndSeq = 0;         // one past the last stored frame
    qsizetype mChunk;
    qsizetype mCapacity;
    bool mAutoChunk;
    quint64 mTotalAppended = 0;
    quint64 mTotalWrapped = 0;
    mutable QReadWriteLock mLock;
};

// Read-only view of frames, with the QVector members SavvyCAN's windows call. Thread safe (see above).
// Returns copies.
class FrameListView
{
public:
    virtual ~FrameListView() = default;

    qsizetype size() const;
    qsizetype count() const { return size(); }
    qsizetype length() const { return size(); }
    bool isEmpty() const { return size() == 0; }

    // Bounds checked: an index out of range (or a row that has just been trimmed) gives a frame of
    // type InvalidFrame.
    CommFrame at(qsizetype i) const;
    CommFrame first() const { return at(0); }
    CommFrame last() const;

    // Snapshots, each taken under one lock, so they are consistent with each other row for row.
    QVector<CommFrame> mid(qsizetype pos, qsizetype len = -1) const;
    QVector<CommFrame> tail(qsizetype n) const;   // the newest n rows (fewer if there aren't n)
    QVector<CommFrame> toVector() const { return mid(0); }

protected:
    explicit FrameListView(const QReadWriteLock *lock) : mLock(lock) {}
    // Called with the store's read lock held (or by the owner thread).
    virtual qsizetype sizeLocked() const = 0;
    // Returns false if row i is not available (out of range or trimmed).
    virtual bool frameLocked(qsizetype i, CommFrame &out) const = 0;

    const QReadWriteLock *mLock;
};

// Every stored frame, oldest first.
class FrameStoreView : public FrameListView
{
public:
    explicit FrameStoreView(const FrameStore *store) : FrameListView(&store->lock()), mStore(store) {}
protected:
    qsizetype sizeLocked() const override { return mStore->size(); }
    bool frameLocked(qsizetype i, CommFrame &out) const override;
    const FrameStore *mStore;
};

#endif // FRAMESTORE_H
