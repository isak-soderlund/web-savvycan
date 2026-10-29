#include "framestore.h"

#include <QReadLocker>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>

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
    const quint64 n = quint64(mChunks.front().offsets.size());
    mChunks.pop_front();
    mFirstSeq += n;
    mTotalWrapped += n;
}

//The chunk the next frame goes into: drops the oldest chunk when full, starts a new chunk when the newest is.
FrameStore::Chunk &FrameStore::chunkForAppend(qsizetype &dropped)
{
    dropped = 0;
    //full: drop the oldest chunk, so the store holds between capacity - chunk and capacity frames.
    //(Only with two chunks or more: a capacity below two chunks is exceeded rather than emptied.)
    if (size() >= mCapacity && mChunks.size() >= 2)
    {
        dropped = qsizetype(mChunks.front().offsets.size());
        dropFrontChunk();
    }
    if (mChunks.empty() || qsizetype(mChunks.back().offsets.size()) == mChunk)
    {
        if (!mChunks.empty()) mChunks.back().words.shrink_to_fit(); //a full chunk keeps no spare room
        mChunks.emplace_back();
        Chunk &c = mChunks.back();
        c.offsets.reserve(size_t(mChunk));
        c.words.reserve(size_t(mChunk) * (HeaderWords + 1)); //room for classic frames; grows for longer ones
    }
    return mChunks.back();
}

qsizetype FrameStore::append(const CommFrame &frame)
{
    const QByteArray &payload = frame.payload();
    return append(frame, payload.constData(), payload.size());
}

qsizetype FrameStore::append(const CommFrame &header, const char *payload, qsizetype len)
{
    if (len < 0) len = 0;
    if (len > 0xFFFF) len = 0xFFFF;
    qsizetype dropped;
    Chunk &c = chunkForAppend(dropped);
    const size_t at = c.words.size();
    //offsets are 32-bit: a chunk of 65536 frames holds at most 65536 * (3 + 8192) words
    c.offsets.push_back(quint32(at));
    const size_t payloadWords = size_t(len + 7) / 8;
    c.words.resize(at + HeaderWords + payloadWords);
    quint64 *w = c.words.data() + at;
    //the time as CommFrame keeps it: seconds (32 bits) and microseconds; seconds past 32 bits are folded
    //into the microseconds (the same time)
    const CommFrame::TimeStamp ts = header.timeStamp();
    qint64 secs = ts.seconds(), usecs = ts.microSeconds();
    if (secs < INT32_MIN || secs > INT32_MAX)
    {
        usecs += secs * 1000000LL;
        secs = 0;
    }
    w[0] = quint64(usecs);
    w[1] = quint64(quint32(qint32(secs))) | (quint64(header.getFrameCount()) << 32);
    w[2] = header.packedBits() | (quint64(header.getBus()) << 38) | (quint64(len) << 46);
    if (payloadWords)
    {
        w[HeaderWords + payloadWords - 1] = 0; //the padding of the last word
        memcpy(w + HeaderWords, payload, size_t(len));
    }
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

const quint64 *FrameStore::record(quint64 seq) const
{
    const quint64 off = seq - mFirstSeq;
    const Chunk &c = mChunks[size_t(off / quint64(mChunk))];
    return c.words.data() + c.offsets[size_t(off % quint64(mChunk))];
}

quint64 *FrameStore::record(quint64 seq)
{
    return const_cast<quint64 *>(static_cast<const FrameStore *>(this)->record(seq));
}

CommFrame FrameStore::bySeq(quint64 seq) const
{
    const quint64 *w = record(seq);
    CommFrame f;
    const qsizetype len = qsizetype((w[2] >> 46) & 0xFFFF);
    if (len) f.setPayload(QByteArray(reinterpret_cast<const char *>(w + HeaderWords), len));
    f.setPackedBits(w[2]); //after the payload: setPayload() may set the FD flag, the record has it as it was
    f.setBus(quint8((w[2] >> 38) & 0xFF));
    f.setTimeStamp(CommFrame::TimeStamp(qint64(qint32(quint32(w[1]))), qint64(w[0])));
    f.setFrameCount(quint32(w[1] >> 32));
    return f;
}

CommFrame::TimeStamp FrameStore::timeStampBySeq(quint64 seq) const
{
    const quint64 *w = record(seq);
    return CommFrame::TimeStamp(qint64(qint32(quint32(w[1]))), qint64(w[0]));
}

void FrameStore::setTimeStamp(quint64 seq, CommFrame::TimeStamp ts)
{
    quint64 *w = record(seq);
    qint64 secs = ts.seconds(), usecs = ts.microSeconds();
    if (secs < INT32_MIN || secs > INT32_MAX)
    {
        usecs += secs * 1000000LL;
        secs = 0;
    }
    w[0] = quint64(usecs);
    w[1] = quint64(quint32(qint32(secs))) | (w[1] & 0xFFFFFFFF00000000ULL);
}

qsizetype FrameStore::memoryBytes() const
{
    size_t bytes = 0;
    for (const Chunk &c : mChunks)
        bytes += c.offsets.capacity() * sizeof(quint32) + c.words.capacity() * sizeof(quint64);
    return qsizetype(bytes);
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
