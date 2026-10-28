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

qsizetype FrameStore::maxChunks() const
{
    return std::max<qsizetype>(2, mCapacity / mChunk);
}

void FrameStore::setCapacity(qsizetype capacityFrames)
{
    mCapacity = capacityFrames;
    if (mChunks.empty() && mAutoChunk)
    {
        //while empty, size the chunks to the window: a wrap drops 1/16 of it (at most 64k frames)
        mChunk = std::clamp<qsizetype>(capacityFrames / 16, 256, DefaultChunkFrames);
    }
    // shrink now if needed (whole chunks from the front, never the chunk being filled)
    while (qsizetype(mChunks.size()) > maxChunks())
    {
        const quint64 n = std::min<quint64>(quint64(mChunks.front().size()), mEndSeq - mFirstSeq);
        mChunks.pop_front();
        mFirstSeq += quint64(mChunk);
        mTotalWrapped += n;
    }
}

qsizetype FrameStore::append(const CommFrame &frame)
{
    qsizetype dropped = 0;
    if (mChunks.empty() || qsizetype(mChunks.back().size()) == mChunk)
    {
        if (qsizetype(mChunks.size()) >= maxChunks())
        {
            dropped = qsizetype(mChunks.front().size());
            mChunks.pop_front();
            mFirstSeq += quint64(mChunk);
            mTotalWrapped += quint64(dropped);
        }
        mChunks.emplace_back();
        mChunks.back().reserve(size_t(mChunk)); // never reallocated after this: frames don't move
        if (mChunks.size() == 1) mFirstSeq = mEndSeq; // first chunk after construction or clear()
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
