#ifndef FRAMESTORE_H
#define FRAMESTORE_H

/*
 * web-savvycan: the frame store behind CommFrameModel (fixes M3, M4, M5 of resolve-can
 * docs/research/savvycan-improve.md).
 *
 * FrameStore is a chunked ring: frames live in fixed-size chunks that are never reallocated, so a
 * frame does not move while it is stored. When the store is full, the oldest whole chunk is
 * dropped (O(chunk) destructor calls, no memmove of the rest). Every frame gets a sequence number
 * (seq) that only grows; the filtered view of the model is a list of seqs (8 bytes per row
 * instead of a second copy of the frame).
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

    // Drop everything. Sequence numbers keep growing (a seq is never reused), so a reader holding
    // an old seq can tell it is gone.
    void clear();

    CommFrame &bySeqMutable(quint64 seq);

    // ---- reader side ----
    quint64 firstSeq() const { return mFirstSeq; }
    quint64 endSeq() const { return mEndSeq; }
    qsizetype size() const { return qsizetype(mEndSeq - mFirstSeq); }
    bool isEmpty() const { return mEndSeq == mFirstSeq; }
    bool contains(quint64 seq) const { return seq >= mFirstSeq && seq < mEndSeq; }
    const CommFrame &bySeq(quint64 seq) const; // unchecked: contains(seq) must be true
    const CommFrame &atIndex(qsizetype i) const { return bySeq(mFirstSeq + quint64(i)); }

    // Total frames ever appended and ever dropped by wrapping (since construction).
    quint64 totalAppended() const { return mTotalAppended; }
    quint64 totalWrapped() const { return mTotalWrapped; }

    QReadWriteLock &lock() const { return mLock; }

private:
    using Chunk = std::vector<CommFrame>;
    void dropFrontChunk();

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
