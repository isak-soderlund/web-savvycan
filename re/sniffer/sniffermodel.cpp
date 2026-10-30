#include <QDebug>
#include <Qt>
#include <QApplication>
#include "sniffermodel.h"
#include "snifferwindow.h"
#include "SnifferDelegate.h"
#include "theme.h"

/*
 * web-savvycan (stage 4): RE Tools > Sniffer.
 * - Theme: the colours are chosen by the theme in effect when drawn (SavvyCAN read the palette's text colour once,
 *   when the window was made, so a theme switch left light grey fading on white or black on dark).
 * - Rows: index() looks the row up in a vector (it walked the map from the start for every cell: O(rows) a cell).
 * - Error frames are not IDs (they were listed under their error class as an ID); remote frames have no data to
 *   sniff (one emptied its ID's bytes).
 * - At most kMaxIds IDs; every frame is counted (framesSeen) for the tests.
 * CAN FD: SnifferItem has 64 bytes (MAX_BYTES), the window 64 data columns.
 */

SnifferModel::SnifferModel(QObject *parent)
    : QAbstractItemModel(parent),
      mFilter(false),
      mNeverExpire(false),
      mFadeInactive(false),
      mMuteNotched(false),
      mTimeSequence(0),
      mExpireInterval(5000)
{
}

void SnifferModel::rebuildRows()
{
    const QMap<quint32, SnifferItem*>& map = mFilter ? mFilters : mMap;
    mRows.clear();
    mRows.reserve(map.size());
    for (SnifferItem *item : map) mRows.append(item);
}

SnifferModel::~SnifferModel()
{
    qDeleteAll(mMap);
    mMap.clear();
    mFilters.clear();
}

void SnifferModel::setExpireInterval(int newVal)
{
    mExpireInterval = newVal;
}

int SnifferModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : tc::LAST+1;
}


int SnifferModel::rowCount(const QModelIndex &parent) const
{
    const QMap<quint32, SnifferItem*>& map = mFilter ? mFilters : mMap;
    return parent.isValid() ? 0 : map.size();
}


QVariant SnifferModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    SnifferItem *item = static_cast<SnifferItem*>(index.internalPointer());
    if(!item) return QVariant();

    int col = index.column();

    switch(role)
    {
        case Qt::DisplayRole:
        {
            switch(col)
            {
                case tc::DELTA:
                    return QString::number(item->getDelta(), 'f');
                case tc::FREQUENCY:
                    if (item->getDelta() == 0) return QString("0 hz");
                    return QString("%1 hz").arg(qRound(1.00 / item->getDelta()));
                case tc::ID:
                    return "0x" + QString("%1").arg(item->getId(), 5, 16, QLatin1Char('0')).toUpper();
                default:
                    break;
            }
            if(tc::DATA_0<=col && col < tc::LAST)
            {
                int data = item->getData(col-tc::DATA_0);
                if(data >= 0)
                {
                    return QString("%1").arg((uint8_t)data, 2, 16, QLatin1Char('0')).toUpper();
                }
            }
            break;
        }
        case Qt::ForegroundRole:
        {
            const bool mDarkMode = Theme::isDark();
            // web-savvycan: the data columns start at tc::DATA_0 (SavvyCAN used col < 2 / col - 2: the ID column faded
            // with byte 0's age, and each byte with the age of the byte before it)
            if (!mFadeInactive || col < tc::DATA_0) return QApplication::palette().brush(QPalette::Text);
            int v = item->getSeqInterval(col - tc::DATA_0) * 10;
            //qDebug() << "mTS: " << mTimeSequence << " gDT(" << (col - 2) << ") " << item->getDataTimestamp(col - 2);
            if (v > 225) v = 225;
            if (v < 0) v = 0;

            if (!mDarkMode) //text defaults to being dark
            {
                return QBrush(QColor(v,v,v,255));
            }
            else //text defaults to being light
            {
                return QBrush(QColor(255-v,255-v,255-v,255));
            }
        }

        case Qt::BackgroundRole:
        {
            const bool mDarkMode = Theme::isDark();
            if(tc::ID==col)
            {
                if(item->elapsed() > 4000)
                {
                    if (!mDarkMode) return QBrush(Qt::red);
                    return QBrush(QColor(128,0,0));
                }
            }
            else if(tc::DATA_0 <= col && col < tc::LAST)
            {
                dc change = item->dataChange(col-tc::DATA_0);
                switch(change)
                {
                    case dc::INC:
                        if (!mDarkMode) return QBrush(Qt::green);
                        return QBrush(QColor(0,128,0));
                    case dc::DEINC:
                        if (!mDarkMode) return QBrush(Qt::red);
                        return QBrush(QColor(128,0,0));
                    default:
                        return QApplication::palette().brush(QPalette::Base);
                }
            }
            break;
        }
    }

    return QVariant();
}


Qt::ItemFlags SnifferModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    return QAbstractItemModel::flags(index);
}


QVariant SnifferModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
    {
        switch(section)
        {
            case tc::DELTA:
                return QString("Delta");
            case tc::FREQUENCY:
                return QString("Freq");
            case tc::ID:
                return QString("ID");
            default:
                break;
        }
        if(tc::DATA_0<=section && section < tc::LAST)
            return QString::number(section-tc::DATA_0);
    }

    return QVariant();
}


QModelIndex SnifferModel::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid())
        return QModelIndex();

    if(column>tc::LAST || row < 0 || row>=mRows.size())
        return QModelIndex();

    return createIndex(row, column, mRows[row]);
}


QModelIndex SnifferModel::parent(const QModelIndex &) const
{
    return QModelIndex();
}

bool SnifferModel::getNeverExpire()
{
    return mNeverExpire;
}

bool SnifferModel::getFadeInactive()
{
    return mFadeInactive;
}

bool SnifferModel::getMuteNotched()
{
    return mMuteNotched;
}

void SnifferModel::setNeverExpire(bool val)
{
    mNeverExpire = val;
}

void SnifferModel::setFadeInactive(bool val)
{
    mFadeInactive = val;
}

void SnifferModel::setMuteNotched(bool val)
{
    mMuteNotched = val;
}

void SnifferModel::clear()
{
    beginResetModel();
    qDeleteAll(mMap);
    mMap.clear();
    mFilters.clear();
    mFilter = false;
    mRows.clear();
    mFramesSeen = 0;
    mHiddenFrames = 0;
    endResetModel();
}

void SnifferModel::updateNotchPoint()
{
    QMap<quint32, SnifferItem*>::iterator i;

    /* update markers */
    for (i = mMap.begin(); i != mMap.end(); ++i)
    {
        i.value()->updateMarker();
    }
}

//Called from window with a timer (currently 200ms)
void SnifferModel::refresh()
{
    QMap<quint32, SnifferItem*>::iterator i;
    QVector<quint32> toRemove;
    SnifferItem* item;

    mTimeSequence++;

    for (i = mMap.begin(); i != mMap.end(); ++i)
    {
        //i.value()->updateMarker();
        if(i.value()->elapsed() > (int)mExpireInterval && !mNeverExpire)
            toRemove.append(i.key());
    }

    if(toRemove.size())
    {
        beginResetModel();
        foreach(quint32 id, toRemove)
        {
            /* remove element */
            item = mMap.take(id);
            mFilters.remove(id);
            delete item;
            /* send notification */
            emit idChange(id, false);
        }
        rebuildRows();
        endResetModel();
    }
    else
        /* refresh data */
        emit dataChanged(createIndex(0, 0),
                    createIndex(rowCount()-1, columnCount()-1), QVector<int>(Qt::DisplayRole));
}


void SnifferModel::filter(fltType pType, int pId)
{
    beginResetModel();
    switch(pType)
    {
        case fltType::NONE:
            /* erase everything */
            mFilter = true;
            mFilters.clear();
            break;
        case fltType::ADD:
            /* add filter to list */
            mFilter = true;
            // web-savvycan: only an ID that is there (mMap[pId] put a null item into the map for an expired one)
            if (mMap.contains(pId)) mFilters[pId] = mMap.value(pId);
            break;
        case fltType::REMOVE:
            /* remove filter */
            if(!mFilter)
                mFilters = mMap;
            mFilter = true;
            mFilters.remove(pId);
            break;
        case fltType::ALL:
            /* stop filtering */
            mFilter = false;
            mFilters.clear();
            break;
    }
    rebuildRows();
    endResetModel();
}


/***********************************************/
/**********         slots       ****************/
/***********************************************/

void SnifferModel::update(CANConnection*, QVector<CommFrame>& pFrames)
{
    for (const CommFrame& frame : pFrames)
    {
        // web-savvycan: data frames only (an error frame is no ID, a remote frame has no data)
        if (frame.frameType() != CommFrame::CANDataFrame && frame.frameType() != CommFrame::CANFDDataFrame) continue;
        mFramesSeen++;
        auto it = mMap.find(frame.frameId());
        if (it == mMap.end())
        {
            if (mMap.size() >= kMaxIds)
            {
                mHiddenFrames++;
                continue;
            }
            SnifferItem *item = new SnifferItem(frame, mTimeSequence);
            item->update(frame, mTimeSequence, mMuteNotched);
            // web-savvycan: a new ID is listed checked, so it is shown also while filtering (SavvyCAN announced the
            // row in the full map's order while the view showed the filtered one)
            QMap<quint32, SnifferItem*> &shown = mFilter ? mFilters : mMap;
            int index = std::distance(shown.begin(), shown.lowerBound(frame.frameId()));
            /* add the frame */
            beginInsertRows(QModelIndex(), index, index);
            mMap.insert(frame.frameId(), item);
            if (mFilter) mFilters.insert(frame.frameId(), item);
            mRows.insert(index, item);
            endInsertRows();

            emit idChange(frame.frameId(), true);
        }
        else
            //updateData
            it.value()->update(frame, mTimeSequence, mMuteNotched);
    }
}

void SnifferModel::notch()
{
    QMap<quint32, SnifferItem*>& map = mFilter ? mFilters : mMap;

    foreach(SnifferItem* item, map)
        item->notch(true);
}

void SnifferModel::unNotch()
{
    QMap<quint32, SnifferItem*>& map = mFilter ? mFilters : mMap;

    foreach(SnifferItem* item, map)
        item->notch(false);
}


