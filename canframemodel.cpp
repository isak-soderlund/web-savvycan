#include "canframemodel.h"

#include <QFile>
#include <QApplication>
#include <QPalette>
#include <QColor>
#include <QBrush>
#include <QDateTime>
#include <QSettings>
#include <QReadLocker>
#include <QWriteLocker>
#include <algorithm>
#include "utility.h"

/*
 * web-savvycan: frames live in a FrameStore (chunked ring, see framestore.h). The trace's rows are
 * filteredSeqs (normal mode: seqs into the store) or overwriteFrames (overwrite mode: one copy per
 * ID and bus). Incoming frames are added without telling the views; publishRows() (from
 * sendBulkRefresh(), on the GUI tick) announces the new rows with one beginInsertRows/endInsertRows,
 * and rows lost when the ring wraps are announced with beginRemoveRows/endRemoveRows. No model
 * reset per tick (M9), no trim by copying (M3), no second copy of every frame (M4), and the views
 * handed to other windows and to the frame sender's thread are locked (M5).
 */

CommFrameModel::~CommFrameModel()
{
    filters.clear();
    busFilters.clear();
}

int CommFrameModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return int(publishedRows);
}

int CommFrameModel::totalFrameCount()
{
    return int(store.size());
}

int CommFrameModel::columnCount(const QModelIndex &index) const
{
    Q_UNUSED(index);

    if(useHexAndDecMode)
        return (int)Column::NUM_COLUMN;
    else
        return (int)Column::NUM_COLUMN-1;
}

CommFrameModel::CommFrameModel(QObject *parent)
    : QAbstractTableModel(parent),
      store(),
      allView(&store),
      filteredView(this)
{
    int maxFramesDefault;
    if (QSysInfo::WordSize > 32)
    {
        qDebug() << "64 bit OS detected. Requesting a large preallocation";
        maxFramesDefault = 10000000;
    }
    else //if compiling for 32 bit you can't ask for gigabytes of preallocation so tone it down.
    {
        qDebug() << "32 bit OS detected. Requesting a much restricted prealloc";
        maxFramesDefault = 2000000;
    }

    QSettings settings;
    preallocSize = settings.value("Main/MaximumFrames", maxFramesDefault).toInt();
    if (preallocSize < 1000) preallocSize = 1000;

    //web-savvycan: the frame store is a ring of MaximumFrames frames, allocated chunk by chunk as frames
    //arrive (nothing is reserved up front). When it is full the oldest chunk is dropped.
    store.setCapacity(preallocSize);

    dbcHandler = DBCHandler::getReference();
    interpretFrames = false;
    overwriteDups = false;
    changingOnly = false;
    filtersPersistDuringClear = false;
    useHexMode = true;
    useColorsByCanId = false;
    useHexAndDecMode = false;
    timeStyle = TS_MICROS;
    timeOffset = 0;
    needFilterRefresh = false;
    ignoreDBCColors = false;
    lastUpdateNumFrames = 0;
    timeFormat =  "MMM-dd HH:mm:ss.zzz";
    sortDirAsc = false;
    bytesPerLine = 8;
}

qsizetype CommFrameModel::FilteredView::sizeLocked() const
{
    if (mModel->overwriteDups) return mModel->overwriteFrames.size();
    return qsizetype(mModel->filteredSeqs.size());
}

bool CommFrameModel::FilteredView::frameLocked(qsizetype i, CommFrame &out) const
{
    if (i < 0) return false;
    if (mModel->overwriteDups)
    {
        if (i >= mModel->overwriteFrames.size()) return false;
        out = mModel->overwriteFrames.at(i);
        return true;
    }
    if (i >= qsizetype(mModel->filteredSeqs.size())) return false;
    const quint64 seq = mModel->filteredSeqs[size_t(i)];
    if (!mModel->store.contains(seq)) return false;
    out = mModel->store.bySeq(seq);
    return true;
}

const CommFrame *CommFrameModel::frameForRow(int row) const
{
    if (row < 0 || row >= publishedRows) return nullptr;
    if (overwriteDups)
    {
        if (row >= overwriteFrames.size()) return nullptr;
        return &overwriteFrames.at(row);
    }
    if (size_t(row) >= filteredSeqs.size()) return nullptr;
    const quint64 seq = filteredSeqs[size_t(row)];
    if (!store.contains(seq)) return nullptr;
    return &store.bySeq(seq);
}

quint64 CommFrameModel::overwriteKey(const CommFrame &frame)
{
    //id in lower 29 bits, bus number shifted up 29 bits (as SavvyCAN's overwrite mode always did)
    return quint64(frame.frameId()) + (quint64(frame.getBus()) << 29);
}

void CommFrameModel::setBytesPerLine(int bpl)
{
    bytesPerLine = bpl;
}

void CommFrameModel::setHexMode(bool mode)
{
    if (useHexMode != mode)
    {
        this->beginResetModel();
        useHexMode = mode;
        Utility::decimalMode = !useHexMode;
        this->endResetModel();
    }
}

void CommFrameModel::setUseColorsByCanId(bool mode)
{
    if (useColorsByCanId != mode)
    {
        this->beginResetModel();
        useColorsByCanId = mode;
        this->endResetModel();
    }
}

void CommFrameModel::setHexAndDecMode(bool mode)
{
    if (useHexAndDecMode != mode)
    {
        this->beginResetModel();
        useHexAndDecMode = mode;
        this->endResetModel();
    }
}

void CommFrameModel::setTimeStyle(TimeStyle newStyle)
{
    if (timeStyle != newStyle)
    {
        this->beginResetModel();
        timeStyle = newStyle;
        Utility::timeStyle = newStyle;
        this->endResetModel();
    }
}

void CommFrameModel::setInterpretMode(bool mode)
{
    //if the state of interpretFrames changes then we need to reset the model
    //so that QT will refresh the view properly
    if (interpretFrames != mode)
    {
        this->beginResetModel();
        interpretFrames = mode;
        this->endResetModel();
    }
}

bool CommFrameModel::getInterpretMode()
{
    return interpretFrames;
}

void CommFrameModel::setTimeFormat(QString format)
{
    Utility::timeFormat = format;
    timeFormat = format;
    beginResetModel(); //reset model to show new time format
    endResetModel();
}

void CommFrameModel::setIgnoreDBCColors(bool mode)
{
    if(ignoreDBCColors != mode)
    {
        beginResetModel(); //reset model to update the view
        ignoreDBCColors = mode;
        endResetModel();
    }
}

/*
 * Scan all frames for the smallest timestamp and offset all timestamps so that smallest one is at 0
*/
void CommFrameModel::normalizeTiming()
{
    if (store.isEmpty()) return;

    beginResetModel();
    {
        QWriteLocker lk(&store.lock());
        timeOffset = store.bySeq(store.firstSeq()).timeStamp().microSeconds();
        qint64 prevStamp = 0;

        //find the absolute lowest timestamp in the whole time. Needed because maybe timestamp was reset in the middle.
        for (quint64 s = store.firstSeq(); s < store.endSeq(); s++)
        {
            if (store.bySeq(s).timeStamp().microSeconds() < timeOffset) timeOffset = store.bySeq(s).timeStamp().microSeconds();
        }

        for (quint64 s = store.firstSeq(); s < store.endSeq(); s++)
        {
            CommFrame &f = store.bySeqMutable(s);
            qint64 thisStamp = f.timeStamp().microSeconds() - timeOffset;
            if (thisStamp <= prevStamp)
            {
                timeOffset -= prevStamp;
            }
            f.setTimeStamp(CommFrame::TimeStamp(0, thisStamp));
        }

        //the filtered rows of normal mode are the same frames; overwrite mode keeps its own copies
        for (int i = 0; i < overwriteFrames.count(); i++)
        {
            overwriteFrames[i].setTimeStamp(CommFrame::TimeStamp(0, overwriteFrames[i].timeStamp().microSeconds() - timeOffset));
        }
    }
    endResetModel();
}

void CommFrameModel::setOverwriteMode(bool mode)
{
    if (overwriteDups == mode) return;
    if (mode)
    {
        //recalcOverwrite() fills overwriteFrames and resets the model
        {
            QWriteLocker lk(&store.lock());
            overwriteDups = true;
        }
        recalcOverwrite();
    }
    else
    {
        {
            QWriteLocker lk(&store.lock());
            overwriteDups = false;
            overwriteFrames.clear();
            overwriteRow.clear();
        }
        sendRefresh();
    }
}

void CommFrameModel::setClearMode(bool mode)
{
    filtersPersistDuringClear = mode;
}

void CommFrameModel::setFilterState(unsigned int ID, bool state)
{
    if (!filters.contains(ID)) return;
    filters[ID] = state;
    sendRefresh();
}

void CommFrameModel::setFilterStatesBatch(const QList<QPair<int,bool>> &updates)
{
    bool anyChanged = false;
    for (const auto &pair : updates)
    {
        if (filters.contains(pair.first))
        {
            filters[pair.first] = pair.second;
            anyChanged = true;
        }
    }
    if (anyChanged) sendRefresh();
}

void CommFrameModel::setBusFilterState(unsigned int BusID, bool state)
{
    if (!busFilters.contains(BusID)) return;
    busFilters[BusID] = state;
    sendRefresh();
}

void CommFrameModel::setAllFilters(bool state)
{
    QMap<int, bool>::iterator it;
    for (it = filters.begin(); it != filters.end(); ++it)
    {
        it.value() = state;
    }
    sendRefresh();
}

void CommFrameModel::setSearchFilter(const QString &text)
{
    // strip leading "0x" or "0X" to keep comparison clean
    QString stripped = text;
    if (stripped.startsWith("0x", Qt::CaseInsensitive))
        stripped = stripped.mid(2);
    m_searchFilter = stripped;
    sendRefresh();
}

/*
 * Sorting: the rows (index list, or the overwrite list) are sorted by a numeric key per column.
*/
uint64_t CommFrameModel::getCommFrameVal(const CommFrame &frame, Column col) const
{
    uint64_t temp = 0;
    switch (col)
    {
    case Column::TimeStamp:
        if (overwriteDups) return frame.getTimeDelta();
        return frame.timeStamp().microSeconds();
    case Column::FrameId:
        return frame.frameId();
    case Column::Type:
        if (frame.hasExtendedFrameFormat())  temp |= 1;
        temp = temp | ((int)frame.frameType() * 2);
        return temp;
    case Column::FrameCount:
        if (overwriteDups) return frame.getFrameCount();
        return 0;
    case Column::Direction:
        if (frame.isReceived()) return 1;
        return 0;
    case Column::Bus:
        return static_cast<uint64_t>(frame.getBus());
    case Column::Length:
        return static_cast<uint64_t>(frame.payload().length());
    case Column::ASCII: //sort both the same for now
    case Column::Data:
        for (int i = 0; i < std::min(static_cast<int>(frame.payload().length()), 8); i++)
            temp += (static_cast<uint64_t>(static_cast<unsigned char>(frame.payload()[i])) << (56 - (8 * i)));
        return temp;
    case Column::DataDec:
    case Column::NUM_COLUMN:
        return 0;
    }
    return 0;
}

void CommFrameModel::sortByColumn(int column)
{
    sortDirAsc = !sortDirAsc;
    const Column col = Column(column);
    const bool asc = sortDirAsc;
    pruneTrimmed();

    beginResetModel();
    {
        QWriteLocker lk(&store.lock());
        if (overwriteDups)
        {
            std::stable_sort(overwriteFrames.begin(), overwriteFrames.end(),
                             [&](const CommFrame &a, const CommFrame &b) {
                                 const uint64_t ka = getCommFrameVal(a, col), kb = getCommFrameVal(b, col);
                                 return asc ? ka < kb : ka > kb;
                             });
            overwriteRow.clear();
            for (int i = 0; i < overwriteFrames.count(); i++) overwriteRow.insert(overwriteKey(overwriteFrames[i]), i);
            publishedRows = overwriteFrames.count();
        }
        else
        {
            std::stable_sort(filteredSeqs.begin(), filteredSeqs.end(),
                             [&](quint64 a, quint64 b) {
                                 const uint64_t ka = getCommFrameVal(store.bySeq(a), col), kb = getCommFrameVal(store.bySeq(b), col);
                                 return asc ? ka < kb : ka > kb;
                             });
            filteredSorted = true;
            publishedRows = qsizetype(filteredSeqs.size());
        }
    }
    endResetModel();
}

//End of custom sorting code

void CommFrameModel::recalcOverwrite()
{
    if (!overwriteDups) return; //no need to do a thing if mode is disabled

    qDebug() << "recalcOverwrite called in model";

    //Look at the current list of frames and turn it into just a list of unique IDs
    QHash<quint64, int> rows;
    QVector<CommFrame> uniq;
    for (quint64 s = store.firstSeq(); s < store.endSeq(); s++)
    {
        CommFrame frame = store.bySeq(s);
        if (!frame.isDataFrame()) continue; //CAN and CAN FD data frames
        if (!passesFrameFilters(frame)) continue;

        const quint64 key = overwriteKey(frame);
        auto it = rows.constFind(key);
        if (it == rows.constEnd())
        {
            frame.setTimeDelta(0);
            frame.setFrameCount(1);
            rows.insert(key, uniq.count());
            uniq.append(frame);
        }
        else
        {
            CommFrame &prev = uniq[it.value()];
            frame.setTimeDelta(frame.timeStamp().microSeconds() - prev.timeStamp().microSeconds());
            frame.setFrameCount(prev.getFrameCount() + 1);
            prev = frame;
        }
    }

    if (changingOnly)
    {
        QVector<CommFrame> changingFiltered;
        QHash<quint64, QByteArray> newSeen;
        for (const CommFrame &f : std::as_const(uniq))
        {
            quint64 ckey = changingKey(f);
            if (!lastSeenData.contains(ckey) || lastSeenData[ckey] != f.payload())
                changingFiltered.append(f);
            newSeen[ckey] = f.payload();
        }
        uniq = changingFiltered;
        lastSeenData = newSeen;
        rows.clear();
        for (int i = 0; i < uniq.count(); i++) rows.insert(overwriteKey(uniq[i]), i);
    }

    beginResetModel();
    {
        QWriteLocker lk(&store.lock());
        overwriteFrames = uniq;
        overwriteRow = rows;
        filteredSeqs.clear();
        filteredSorted = false;
        pendingPrune = false;
        publishedRows = overwriteFrames.count();
    }
    endResetModel();
}

QVariant CommFrameModel::data(const QModelIndex &index, int role) const
{
    QString tempString;
    CommFrame thisFrame;
    QVariant ts;

    if (!index.isValid())
        return QVariant();

    const CommFrame *rowFrame = frameForRow(index.row());
    if (!rowFrame) //out of range, or the ring dropped this row a moment ago (its removal is on its way)
        return QVariant();

    thisFrame = *rowFrame;

    const unsigned char *data = reinterpret_cast<const unsigned char *>(thisFrame.payload().constData());
    int dataLen = thisFrame.payload().length();

    if (role == Qt::BackgroundRole)
    {
        if (dbcHandler != nullptr && interpretFrames && !ignoreDBCColors)
        {
            DBC_MESSAGE *msg = dbcHandler->findMessage(thisFrame);
            if (msg != nullptr)
            {
                return msg->bgColor;
            }
        }

        if (useColorsByCanId)
        {
            QString frameStr = Utility::formatCANID(thisFrame.frameId(), thisFrame.hasExtendedFrameFormat());
            // Color rows by the value in the "category" column
            return QBrush(Utility::colorForString(frameStr));
        }

        return (index.row() % 2) ?
            QApplication::palette().color(QPalette::Base) :
            QApplication::palette().color(QPalette::AlternateBase);
    }

    if (role == Qt::ForegroundRole)
    {
        if (dbcHandler != nullptr && interpretFrames && !ignoreDBCColors)
        {
            DBC_MESSAGE *msg = dbcHandler->findMessage(thisFrame);
            if (msg != nullptr)
            {
                return msg->fgColor;
            }
        }

        return QApplication::palette().color(QPalette::WindowText);
    }

    if (role == Qt::TextAlignmentRole)
    {
        switch(Column(index.column()))
        {
        case Column::TimeStamp:
            return Qt::AlignRight;
        case Column::FrameId:
        case Column::Direction:
        case Column::Type:
        case Column::Bus:
        case Column::FrameCount:
        case Column::Length:
            return Qt::AlignHCenter;
        default:
            return Qt::AlignLeft;
        }
    }

    if (role == Qt::DisplayRole) {
        switch (Column(index.column()))
        {
        case Column::TimeStamp:
            //Reformatting the output a bit with custom code
            if (overwriteDups)
            {
                if (timeStyle == TS_SECONDS) return QString::number(thisFrame.getTimeDelta() / 1000000.0, 'f', 5);
                return QString::number(thisFrame.getTimeDelta());
            }
            else ts = Utility::formatTimestamp(thisFrame.timeStamp().microSeconds());
            if (ts.typeId() == QMetaType::Double) return QString::number(ts.toDouble(), 'f', 5); //never scientific notation, 5 decimal places
            if (ts.typeId() == QMetaType::LongLong) return QString::number(ts.toLongLong()); //never scientific notion, all digits shown
            if (ts.typeId() == QMetaType::QDateTime) return ts.toDateTime().toString(timeFormat); //custom set format for dates and times
            return Utility::formatTimestamp(thisFrame.timeStamp().microSeconds());
        case Column::FrameId:
            return Utility::formatCANID(thisFrame.frameId(), thisFrame.hasExtendedFrameFormat());
        case Column::Type:
            switch (thisFrame.frameType())
            {
            case CommFrame::FrameType::CANDataFrame:
                tempString = "C";
                break;
            case CommFrame::FrameType::CANFDDataFrame:
                tempString = "D";
                if (thisFrame.hasBitrateSwitch()) tempString.append("B");       //bit rate switch (fast data phase)
                if (thisFrame.hasErrorStateIndicator()) tempString.append("E"); //sender was error passive
                break;
            case CommFrame::FrameType::FLEXRAYDataFrame:
                tempString = "F";
                break;
            case CommFrame::FrameType::LINDataFrame:
                tempString = "L";
                break;
            case CommFrame::FrameType::ErrorFrame:
                tempString = "E";
                break;
            case CommFrame::FrameType::RemoteRequestFrame:
                tempString = "R";
                break;
            case CommFrame::FrameType::InvalidFrame:
                tempString = "I";
                break;
            }
            if (thisFrame.hasExtendedFrameFormat()) tempString.append('X');
            return tempString;
        case Column::FrameCount:
            return QString::number(thisFrame.getFrameCount());
        case Column::Direction:
            if (thisFrame.isReceived()) return QString(tr("Rx"));
            return QString(tr("Tx"));
        case Column::Bus:
            return QString::number(thisFrame.getBus());
        case Column::Length:
            return QString::number(dataLen);
        case Column::ASCII:
            if (thisFrame.frameId() >= 0x7FFFFFF0ull)
            {
                tempString.append("MARK ");
                tempString.append(QString::number(thisFrame.frameId() & 0x7));
                return tempString;
            }
            if (thisFrame.isDataFrame()) { //CAN and CAN FD data frames
                if (dataLen < 0) dataLen = 0;
                //if (dLen > 8) dLen = 8;
                for (int i = 0; i < dataLen; i++)
                {
                    char byt = thisFrame.payload()[i];
                    //0x20 through 0x7E are printable characters. Outside of that range they aren't. So use dots instead
                    if (byt < 0x20) byt = 0x2E; //dot character
                    if (byt > 0x7E) byt = 0x2E;
                    tempString.append(QString::fromUtf8(&byt, 1));
                    if (!((i+1) % bytesPerLine) && (i != (dataLen - 1))) tempString.append("\n");
                }
            }
            if (thisFrame.frameType() == CommFrame::ErrorFrame)
            {
                 tempString = "ERROR";
            }
            return tempString;
        case Column::Data:
            if (dataLen < 0) dataLen = 0;
            //if (useHexMode) tempString.append("0x ");
            if (thisFrame.frameType() == CommFrame::RemoteRequestFrame) {
                return tempString;
            }
            for (int i = 0; i < dataLen; i++)
            {
                if (useHexMode) tempString.append( QString::number(data[i], 16).toUpper().rightJustified(2, '0'));
                else tempString.append(QString::number(data[i], 10));
                if (!((i+1) % bytesPerLine) && (i != (dataLen - 1))) tempString.append("\n");
                else tempString.append(" ");
            }
            if (thisFrame.frameType() == thisFrame.ErrorFrame)
            {
                if (thisFrame.error() & thisFrame.TransmissionTimeoutError) tempString.append("\nTX Timeout");
                if (thisFrame.error() & thisFrame.LostArbitrationError) tempString.append("\nLost Arbitration");
                if (thisFrame.error() & thisFrame.ControllerError) tempString.append("\nController Error");
                if (thisFrame.error() & thisFrame.ProtocolViolationError) tempString.append("\nProtocol Violation");
                if (thisFrame.error() & thisFrame.TransceiverError) tempString.append("\nTransceiver Error");
                if (thisFrame.error() & thisFrame.MissingAcknowledgmentError) tempString.append("\nMissing ACK");
                if (thisFrame.error() & thisFrame.BusOffError) tempString.append("\nBus OFF");
                if (thisFrame.error() & thisFrame.BusError) tempString.append("\nBus ERR");
                if (thisFrame.error() & thisFrame.ControllerRestartError) tempString.append("\nController restart err");
                if (thisFrame.error() & thisFrame.UnknownError) tempString.append("\nUnknown error type");
            }
            //TODO: technically the actual returned bytes for an error frame encode some more info. Not interpreting it yet.

            //now, if we're supposed to interpret the data and the DBC handler is loaded then use it
            if ( (dbcHandler != nullptr) && interpretFrames && thisFrame.isDataFrame() )
            {
                DBC_MESSAGE *msg = dbcHandler->findMessage(thisFrame);
                if (msg != nullptr)
                {
                    tempString.append("   <" + msg->name + ">\n");
                    if (msg->comment.length() > 1) tempString.append(msg->comment + "\n");

                    QList<DBC_SIGNAL *> sigs = msg->sigHandler->getSignalsAsList();

                    for (int j = 0; j < msg->sigHandler->getCount(); j++)
                    {                        
                        QString sigString;
                        DBC_SIGNAL* sig = sigs[j];

                        if ( (sig->multiplexParent == nullptr) && sig->processAsText(thisFrame, sigString))
                        {
                            tempString.append(sigString);
                            tempString.append("\n");
                            if (sig->isMultiplexor)
                            {
                                qDebug() << "Multiplexor. Diving into the tree";
                                tempString.append(sig->processSignalTree(thisFrame));
                            }
                        }
                        else if (sig->isMultiplexed && overwriteDups) //wasn't in this exact frame but is in the message. Use cached value
                        {
                            bool isInteger = false;
                            if (sig->valType == UNSIGNED_INT || sig->valType == SIGNED_INT) isInteger = true;
                            tempString.append(sig->makePrettyOutput(sig->cachedValue.toDouble(), sig->cachedValue.toLongLong(), true, isInteger));
                            tempString.append("\n");
                        }
                    }
                }
            }
            return tempString;

        case Column::DataDec:
            if (!useHexAndDecMode)
                return tempString;

            if (dataLen < 0)
                dataLen = 0;

            if (thisFrame.frameType() == QCanBusFrame::RemoteRequestFrame)
                return tempString;

            for (int i = 0; i < dataLen; i++)
            {
                tempString.append(QString::number(data[i], 10));

                if (!((i+1) % bytesPerLine) && (i != (dataLen - 1)))
                    tempString.append("\n");
                else
                    tempString.append(" ");
            }
            return tempString;

            default:
            return tempString;
        }
    }

    return QVariant();
}

QVariant CommFrameModel::headerData(int section, Qt::Orientation orientation,
                                     int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant();

    if (orientation == Qt::Horizontal)
    {
        switch (Column(section))
        {
        case Column::TimeStamp:
            if (overwriteDups) return QString(tr("Time Delta"));
            return QString(tr("Timestamp"));
        case Column::FrameId:
            return QString(tr("ID"));
        case Column::Type:
            return QString(tr("Type"));
        case Column::FrameCount:
            return QString(tr("Cnt"));
        case Column::Direction:
            return QString(tr("Dir"));
        case Column::Bus:
            return QString(tr("Bus"));
        case Column::Length:
            return QString(tr("Len"));
        case Column::ASCII:
            return QString(tr("ASCII"));
        case Column::Data:
            return QString(tr("Data"));
        case Column::DataDec:
            if(useHexAndDecMode)
                return QString(tr("DataDec"));
            else
                return QString("");
        default:
            return QString("");
        }
    }

    else
        return QString::number(section + 1);

    return QVariant();
}

bool CommFrameModel::any_filters_are_configured(void)
{
    for (auto const &val : std::as_const(filters))
    {
        if (val == true)
            continue;
        else
            return true;
    }
    return false;
}

bool CommFrameModel::any_busfilters_are_configured(void)
{
    for (auto const &val : std::as_const(busFilters))
    {
        if (val == true)
            continue;
        else
            return true;
    }
    return false;
}

bool CommFrameModel::any_dirfilters_are_configured(void)
{
    for (auto const &val : std::as_const(dirFilters))
        if (val == false) return true;
    return false;
}

bool CommFrameModel::any_lengthfilters_are_configured(void)
{
    for (auto const &val : std::as_const(lengthFilters))
        if (val == false) return true;
    return false;
}

// Returns true if the frame passes ALL active filters (ID, bus, type, dir, length, search).
// Uses value(key, true) for type/dir/length so unknown entries default to visible.
bool CommFrameModel::passesFrameFilters(const CommFrame &frame)
{
    if (!filters.value((int)frame.frameId(), false)) return false;
    if (!busFilters.value((int)frame.getBus(), false)) return false;
    int dir = frame.isReceived() ? 1 : 0;
    if (!dirFilters.value(dir, true)) return false;
    if (!lengthFilters.value(frame.payload().length(), true)) return false;
    if (!m_searchFilter.isEmpty() &&
        !QString::number(frame.frameId(), 16).contains(m_searchFilter, Qt::CaseInsensitive))
        return false;
    return true;
}

quint64 CommFrameModel::changingKey(const CommFrame &frame) const
{
    quint64 key  = (quint64)(frame.frameId()         & 0x1FFFFFFF);        // bits  0-28: frame ID
    key         |= (quint64)((int)frame.frameType()  & 0x07)        << 29; // bits 29-31: frame type
    key         |= (quint64)(frame.isReceived() ? 1 : 0)           << 32; // bit  32:    direction
    key         |= (quint64)(frame.getBus()          & 0xFF)        << 33; // bits 33-40: bus
    key         |= (quint64)(frame.payload().length() & 0xFF)       << 41; // bits 41-48: length
    return key;
}

void CommFrameModel::setDirFilterState(int dir, bool state)
{
    if (!dirFilters.contains(dir)) return;
    dirFilters[dir] = state;
    sendRefresh();
}

void CommFrameModel::setLengthFilterState(int length, bool state)
{
    if (!lengthFilters.contains(length)) return;
    lengthFilters[length] = state;
    sendRefresh();
}

void CommFrameModel::setChangingOnly(bool state)
{
    changingOnly = state;
    lastSeenData.clear();
    sendRefresh();
}

const QMap<int, bool> *CommFrameModel::getDirFiltersReference() const
{
    return &dirFilters;
}

const QMap<int, bool> *CommFrameModel::getLengthFiltersReference() const
{
    return &lengthFilters;
}


void CommFrameModel::registerFilters(const CommFrame &tempFrame)
{
    //if this ID isn't found in the filters list then add it and show it enabled by default
    if (!filters.contains(tempFrame.frameId()))
    {
        filters.insert(tempFrame.frameId(), true);
        needFilterRefresh = true;
    }

    //if this BusID isn't found in the busFilters list then add it and show it by default
    if (!busFilters.contains(tempFrame.getBus()))
    {
        // if there are any busFilters already configured, leave the new filter disabled
        if (any_busfilters_are_configured())
            busFilters.insert(tempFrame.getBus(), false);
        else
            busFilters.insert(tempFrame.getBus(), true);
        needFilterRefresh = true;
    }

    // Register direction in dirFilters
    {
        int dir = tempFrame.isReceived() ? 1 : 0;
        if (!dirFilters.contains(dir))
        {
            dirFilters.insert(dir, any_dirfilters_are_configured() ? false : true);
            needFilterRefresh = true;
        }
    }

    // Register payload length in lengthFilters
    {
        int len = tempFrame.payload().length();
        if (!lengthFilters.contains(len))
        {
            lengthFilters.insert(len, any_lengthfilters_are_configured() ? false : true);
            needFilterRefresh = true;
        }
    }
}

//One frame into the store and the trace's rows. The store's write lock is held; no signals here.
void CommFrameModel::ingestLocked(const CommFrame &frame, bool applyOffset)
{
    CommFrame tempFrame = frame;
    if (applyOffset)
        tempFrame.setTimeStamp(CommFrame::TimeStamp(0, tempFrame.timeStamp().microSeconds() - timeOffset));

    lastUpdateNumFrames++;
    registerFilters(tempFrame);

    if (!overwriteDups)
    {
        bool show = false;
        if (passesFrameFilters(tempFrame))
        {
            show = true;
            if (changingOnly)
            {
                quint64 ckey = changingKey(tempFrame);
                auto it = lastSeenData.find(ckey);
                if (it != lastSeenData.end() && it.value() == tempFrame.payload())
                    show = false;
                else
                    lastSeenData.insert(ckey, tempFrame.payload());
            }
        }
        tempFrame.setFrameCount(1);
        const quint64 seq = store.endSeq();
        if (store.append(tempFrame) > 0) pendingPrune = true;
        if (show) filteredSeqs.push_back(seq);
    }
    else //yes, overwrite dups
    {
        const quint64 key = overwriteKey(tempFrame);
        const int row = overwriteRow.value(key, -1);
        if (row >= 0)
        {
            const CommFrame &prev = overwriteFrames.at(row);
            bool dataChanged = true;
            tempFrame.setFrameCount(prev.getFrameCount() + 1);
            tempFrame.setTimeDelta(tempFrame.timeStamp().microSeconds() - prev.timeStamp().microSeconds());
            if (changingOnly)
            {
                quint64 ckey = changingKey(tempFrame);
                auto it = lastSeenData.find(ckey);
                if (it != lastSeenData.end() && it.value() == tempFrame.payload())
                    dataChanged = false;
                else
                    lastSeenData.insert(ckey, tempFrame.payload());
            }
            if (dataChanged) overwriteFrames[row] = tempFrame;
            store.append(tempFrame);
        }
        else
        {
            store.append(tempFrame);
            if (passesFrameFilters(tempFrame))
            {
                if (changingOnly)
                    lastSeenData[changingKey(tempFrame)] = tempFrame.payload();
                tempFrame.setFrameCount(1);
                tempFrame.setTimeDelta(0);
                overwriteRow.insert(key, overwriteFrames.count());
                overwriteFrames.append(tempFrame);
            }
        }
    }
}

//Rows whose frames the ring has dropped: remove them from the index list and tell the views.
void CommFrameModel::pruneTrimmed()
{
    if (!pendingPrune) return;
    pendingPrune = false;
    if (overwriteDups) return; //overwrite rows are copies; they don't point into the store

    if (!filteredSorted)
    {
        //seqs are in arrival order, so the dropped ones are a prefix
        const quint64 first = store.firstSeq();
        qsizetype r = 0;
        const qsizetype n = qsizetype(filteredSeqs.size());
        while (r < n && filteredSeqs[size_t(r)] < first) r++;
        if (r == 0) return;
        const qsizetype rp = std::min(r, publishedRows);
        if (rp > 0) beginRemoveRows(QModelIndex(), 0, int(rp - 1));
        {
            QWriteLocker lk(&store.lock());
            filteredSeqs.erase(filteredSeqs.begin(), filteredSeqs.begin() + r);
            publishedRows -= rp;
        }
        if (rp > 0)
        {
            endRemoveRows();
            emit framesTrimmed(int(rp));
        }
    }
    else
    {
        beginResetModel();
        qsizetype removed;
        {
            QWriteLocker lk(&store.lock());
            const quint64 first = store.firstSeq();
            const qsizetype before = qsizetype(filteredSeqs.size());
            filteredSeqs.erase(std::remove_if(filteredSeqs.begin(), filteredSeqs.end(),
                                              [first](quint64 s) { return s < first; }),
                               filteredSeqs.end());
            removed = before - qsizetype(filteredSeqs.size());
            publishedRows = qsizetype(filteredSeqs.size());
        }
        endResetModel();
        if (removed > 0) emit framesTrimmed(int(removed));
    }
}

//Announce rows added since the last call: one insert per call (and in overwrite mode one dataChanged).
void CommFrameModel::publishRows()
{
    pruneTrimmed();
    const qsizetype total = overwriteDups ? overwriteFrames.count() : qsizetype(filteredSeqs.size());
    if (total > publishedRows)
    {
        beginInsertRows(QModelIndex(), int(publishedRows), int(total - 1));
        publishedRows = total; //plain qsizetype read by the UI thread only; the views' size() uses the lists
        endInsertRows();
    }
    if (overwriteDups && publishedRows > 0)
        emit dataChanged(index(0, 0), index(int(publishedRows - 1), columnCount(QModelIndex()) - 1));
}

void CommFrameModel::addFrame(const CommFrame& frame, bool autoRefresh = false)
{
    {
        QWriteLocker lk(&store.lock());
        ingestLocked(frame, true);
    }
    if (autoRefresh) publishRows();
    else pruneTrimmed();
}

//A batch of frames from a connection (CANConManager's 20 ms drain of its queue, on the GUI thread).
//The frames are stored at once; the views hear about them on the next GUI tick (sendBulkRefresh).
void CommFrameModel::addFrames(const CANConnection*, const QVector<CommFrame>& pFrames)
{
    {
        QWriteLocker lk(&store.lock());
        for (const CommFrame &frame : pFrames)
            ingestLocked(frame, true);
    }
    pruneTrimmed();
}

void CommFrameModel::sendRefresh()
{
    qDebug() << "Sending mass refresh";

    if(overwriteDups)
    {
        recalcOverwrite();
    }
    else
    {
        std::deque<quint64> seqs;
        for (quint64 s = store.firstSeq(); s < store.endSeq(); s++)
        {
            if (passesFrameFilters(store.bySeq(s))) seqs.push_back(s);
        }

        if (changingOnly)
        {
            std::deque<quint64> changingFiltered;
            QHash<quint64, QByteArray> newSeen;
            for (quint64 s : seqs)
            {
                const CommFrame &f = store.bySeq(s);
                quint64 ckey = changingKey(f);
                auto it = newSeen.find(ckey);
                if (it == newSeen.end() || it.value() != f.payload())
                {
                    newSeen.insert(ckey, f.payload());
                    changingFiltered.push_back(s);
                }
            }
            seqs.swap(changingFiltered);
            lastSeenData = newSeen;
        }

        beginResetModel();
        {
            QWriteLocker lk(&store.lock());
            filteredSeqs.swap(seqs);
            filteredSorted = false;
            pendingPrune = false;
            publishedRows = qsizetype(filteredSeqs.size());
        }
        lastUpdateNumFrames = 0;
        endResetModel();
    }
}

void CommFrameModel::sendRefresh(int pos)
{
    Q_UNUSED(pos);
    publishRows();
}

//issue a refresh for the entries added since the last call (GUI tick, every 250 ms).
//Returns the number of frames that came in, as before.
int CommFrameModel::sendBulkRefresh()
{
    publishRows();
    if (lastUpdateNumFrames <= 0) return 0;
    int num = lastUpdateNumFrames;
    lastUpdateNumFrames = 0;
    return num;
}

void CommFrameModel::clearFrames()
{
    this->beginResetModel();
    {
        QWriteLocker lk(&store.lock());
        store.clear();
        store.setCapacity(preallocSize); //a file load may have grown it
        filteredSeqs.clear();
        filteredSorted = false;
        overwriteFrames.clear();
        overwriteRow.clear();
        pendingPrune = false;
        publishedRows = 0;
    }
    if(filtersPersistDuringClear == false)
    {
        filters.clear();
        busFilters.clear();
        dirFilters.clear();
        lengthFilters.clear();
    }
    lastSeenData.clear();
    lastUpdateNumFrames = 0;
    this->endResetModel();

    emit updatedFiltersList();
}

/*
 * Since the getListReference function returns readonly
 * you can't insert frames with it. Instead this function
 * allows for a mass import of frames into the model
 */
void CommFrameModel::insertFrames(const QVector<CommFrame> &newFrames)
{
    {
        QWriteLocker lk(&store.lock());
        //a loaded file is kept whole: grow the ring if the file is bigger than MaximumFrames
        //(clearFrames() puts the capacity back)
        const qsizetype need = store.size() + newFrames.count();
        if (need > store.capacity()) store.setCapacity(need + store.chunkFrames());
        for (const CommFrame &f : newFrames)
            ingestLocked(f, false);
    }
    lastUpdateNumFrames = newFrames.count();
    publishRows();
    if (needFilterRefresh) emit updatedFiltersList();
}

int CommFrameModel::getIndexFromTimeID(unsigned int ID, double timestamp)
{
    //returns a row of the trace (the caller selects it in the view)
    int bestIndex = -1;
    int64_t intTimeStamp = static_cast<int64_t> (timestamp * 1000000l);
    for (int i = 0; i < publishedRows; i++)
    {
        const CommFrame *f = frameForRow(i);
        if (f && f->frameId() == ID)
        {
            if (f->timeStamp().microSeconds() <= intTimeStamp) bestIndex = i;
            else break; //drop out of loop as soon as we pass the proper timestamp
        }
    }
    return bestIndex;
}

void CommFrameModel::loadFilterFile(QString filename)
{
    QFile *inFile = new QFile(filename);
    QByteArray line;
    int ID;

    if (!inFile->open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    filters.clear();
    busFilters.clear();
    dirFilters.clear();
    lengthFilters.clear();

    while (!inFile->atEnd()) {
        line = inFile->readLine().simplified();
        if (line.length() > 2)
        {
            QList<QByteArray> tokens = line.split(',');
            QByteArray prefix = tokens[0].toUpper();
            if (prefix == "BUS" && tokens.size() >= 3) {
                busFilters.insert(tokens[1].toInt(), tokens[2].toUpper() == "T");
            } else if (prefix == "DIR" && tokens.size() >= 3) {
                dirFilters.insert(tokens[1].toInt(), tokens[2].toUpper() == "T");
            } else if (prefix == "LEN" && tokens.size() >= 3) {
                lengthFilters.insert(tokens[1].toInt(), tokens[2].toUpper() == "T");
            } else if (prefix == "CHANGINGONLY") {
                // handled via QSettings now; skip
            } else {
                ID = tokens[0].toInt(nullptr, 16);
                if (tokens[1].toUpper() == "T") filters.insert(ID, true);
                    else filters.insert(ID, false);
            }
        }
    }
    inFile->close();

    sendRefresh();

    emit updatedFiltersList();
}

void CommFrameModel::saveFilterFile(QString filename)
{
    QFile *outFile = new QFile(filename);

    if (!outFile->open(QIODevice::WriteOnly | QIODevice::Text))
        return;

    QMap<int, bool>::const_iterator it;
    for (it = filters.cbegin(); it != filters.cend(); ++it)
    {
        outFile->write(QString::number(it.key(), 16).toUtf8());
        outFile->putChar(',');
        if (it.value()) outFile->putChar('T');
            else outFile->putChar('F');
        outFile->write("\n");
    }
    for (it = busFilters.cbegin(); it != busFilters.cend(); ++it)
        outFile->write(QString("BUS,%1,%2\n").arg(it.key()).arg(it.value() ? "T" : "F").toUtf8());
    for (it = dirFilters.cbegin(); it != dirFilters.cend(); ++it)
        outFile->write(QString("DIR,%1,%2\n").arg(it.key()).arg(it.value() ? "T" : "F").toUtf8());
    for (it = lengthFilters.cbegin(); it != lengthFilters.cend(); ++it)
        outFile->write(QString("LEN,%1,%2\n").arg(it.key()).arg(it.value() ? "T" : "F").toUtf8());
    outFile->close();
}

bool CommFrameModel::getChangingOnly() const
{
    return changingOnly;
}

bool CommFrameModel::needsFilterRefresh()
{
    bool temp = needFilterRefresh;
    needFilterRefresh = false;
    return temp;
}

/*
 * Read-only views for other windows. They lock the store for every access and return copies, so
 * they may be used from another thread (the frame sender). See framestore.h.
 */
const FrameListView* CommFrameModel::getListReference() const
{
    return &allView;
}

const FrameListView* CommFrameModel::getFilteredListReference() const
{
    return &filteredView;
}

quint64 CommFrameModel::framesWrapped() const
{
    return store.totalWrapped();
}

qsizetype CommFrameModel::storeCapacity() const
{
    return store.capacity();
}

const QMap<int, bool>* CommFrameModel::getFiltersReference() const
{
    return &filters;
}

const QMap<int, bool>* CommFrameModel::getBusFiltersReference() const
{
    return &busFilters;
}
