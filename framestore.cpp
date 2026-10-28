#include "framestore.h"

#include <QReadLocker>
#include <algorithm>

FrameStore::FrameStore(qsizetype capacityFrames, qsizetype chunkFrames)
    : mChunk(chunkFrames > 0 ? chunkFrames : DefaultChunkFrames),
      mCapacity(capacityFrames),
      mAutoChunk(chunkFrames <= 0)
{
    if (mAutoChunk) setCapacity(capacityFrames);
}

void FrameStore::setCapacity(qsizetype capacityFrames)
{
    mCapacity = capacityFrames;
    if (mChunks.empty() && mAutoChunk)
    {
        //while empty, size the chunks to the window: a wrap drops 1/16 of it (at most 64k frames)
        mChunk = std::clamp<qsizetype>(capacityFrames / 16, 256, DefaultChunkFrames);
    }
    //shrink now if needed: whole chunks from the front (the front chunk is full when there are two or more)
    while (size() > mCapacity && mChunks.size() >= 2)
        dropFrontChunk();
}

void FrameStore::dropFrontChunk()
{
    const quint64 n = quint64(mChunks.front().size());
    mChunks.pop_front();
    mFirstSeq += n;
    mTotalWrapped += n;
}

qsizetype FrameStore::append(const CommFrame &frame)
{
    qsizetype dropped = 0;
    //full: drop the oldest chunk, so the store holds between capacity - chunk and capacity frames.
    //(Only with two chunks or more: a capacity below two chunks is exceeded rather than emptied.)
    if (size() >= mCapacity && mChunks.size() >= 2)
    {
        dropped = qsizetype(mChunks.front().size());
        dropFrontChunk();
    }
    if (mChunks.empty() || qsizetype(mChunks.back().size()) == mChunk)
    {
        mChunks.emplace_back();
        mChunks.back().reserve(size_t(mChunk)); // never reallocated after this: frames don't move
    }
    mChunks.back().push_back(frame);
    ++mEndSeq;
    ++mTotalAppended;
    return dropped;
}

void FrameStore::clear()
{
    mChunks.clear();
    // keep seqs unique and chunk aligned: the next chunk starts at the next multiple of mChunk
    const quint64 c = quint64(mChunk);
    mEndSeq = ((mEndSeq + c - 1) / c) * c;
    mFirstSeq = mEndSeq;
}

const CommFrame &FrameStore::bySeq(quint64 seq) const
{
    const quint64 off = seq - mFirstSeq;
    return mChunks[size_t(off / quint64(mChunk))][size_t(off % quint64(mChunk))];
}

CommFrame &FrameStore::bySeqMutable(quint64 seq)
{
    const quint64 off = seq - mFirstSeq;
    return mChunks[size_t(off / quint64(mChunk))][size_t(off % quint64(mChunk))];
}

// ---------------- FrameListView ----------------

qsizetype FrameListView::size() const
{
    QReadLocker lk(const_cast<QReadWriteLock *>(mLock));
    return sizeLocked();
}

CommFrame FrameListView::at(qsizetype i) const
{
    QReadLocker lk(const_cast<QReadWriteLock *>(mLock));
    CommFrame f(CommFrame::InvalidFrame);
    if (!frameLocked(i, f)) return CommFrame(CommFrame::InvalidFrame);
    return f;
}

CommFrame FrameListView::last() const
{
    QReadLocker lk(const_cast<QReadWriteLock *>(mLock));
    CommFrame f(CommFrame::InvalidFrame);
    if (!frameLocked(sizeLocked() - 1, f)) return CommFrame(CommFrame::InvalidFrame);
    return f;
}

QVector<CommFrame> FrameListView::mid(qsizetype pos, qsizetype len) const
{
    QReadLocker lk(const_cast<QReadWriteLock *>(mLock));
    const qsizetype n = sizeLocked();
    if (pos < 0) pos = 0;
    if (pos > n) pos = n;
    qsizetype end = (len < 0) ? n : std::min(n, pos + len);
    QVector<CommFrame> out;
    out.reserve(end - pos);
    CommFrame f;
    for (qsizetype i = pos; i < end; i++)
        if (frameLocked(i, f)) out.append(f);
    return out;
}

QVector<CommFrame> FrameListView::tail(qsizetype n) const
{
    QReadLocker lk(const_cast<QReadWriteLock *>(mLock));
    const qsizetype sz = sizeLocked();
    if (n > sz) n = sz;
    if (n < 0) n = 0;
    QVector<CommFrame> out;
    out.reserve(n);
    CommFrame f;
    for (qsizetype i = sz - n; i < sz; i++)
        if (frameLocked(i, f)) out.append(f);
    return out;
}

bool FrameStoreView::frameLocked(qsizetype i, CommFrame &out) const
{
    if (i < 0 || i >= mStore->size()) return false;
    out = mStore->atIndex(i);
    return true;
}
