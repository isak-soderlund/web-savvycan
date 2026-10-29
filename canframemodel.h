#ifndef COMMFRAMEMODEL_H
#define COMMFRAMEMODEL_H

#include <QAbstractTableModel>
#include <QList>
#include <QVector>
#include <QDebug>
#include <QMutex>
#include "can_structs.h"
#include "dbc/dbchandler.h"
#include "connections/canconnection.h"
#include "utility.h"
#include "framestore.h"
#include <deque>

enum class Column {
    TimeStamp = 0, ///< The timestamp when the frame was transmitted or received
    FrameId   = 1, ///< The frames CAN identifier (Standard: 11 or Extended: 29 bit)
    Type      = 2, ///< Type of packet (CAN, RemoteReq, Err, CANFD, LIN, FlexRay) + X if extended (CAN/FD)
    FrameCount= 3, ///< Number of frames of this ID that have come in (only when in overwrite mode)
    Direction = 4, ///< Whether the frame was transmitted or received
    Bus       = 5, ///< The bus where the frame was transmitted or received
    Length    = 6, ///< The frames payload data length
    ASCII     = 7, ///< The payload interpreted as ASCII characters
    Data      = 8, ///< The frames payload data
    DataDec   = 9,
    NUM_COLUMN
};

class CommFrameModel: public QAbstractTableModel
{
    Q_OBJECT

public:
    CommFrameModel(QObject *parent = 0);
    virtual ~CommFrameModel();

    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role) const;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const;
    int columnCount(const QModelIndex &) const;
    int totalFrameCount();

    void sendRefresh();
    void sendRefresh(int);
    int  sendBulkRefresh();
    void clearFrames();
    void setInterpretMode(bool);
    bool getInterpretMode();
    void setOverwriteMode(bool);
    void setHexMode(bool);
    void setUseColorsByCanId(bool);
    void setHexAndDecMode(bool);
    void setClearMode(bool mode);
    void setTimeStyle(TimeStyle newStyle);
    void setIgnoreDBCColors(bool mode);
    void setFilterState(unsigned int ID, bool state);
    void setFilterStatesBatch(const QList<QPair<int,bool>> &updates);
    void setBusFilterState(unsigned int BusID, bool state);
    void setDirFilterState(int dir, bool state);
    void setLengthFilterState(int length, bool state);
    void setTypeFilterState(int type, bool state);
    void setAllFilters(bool state);
    void setChangingOnly(bool state);
    bool getChangingOnly() const;
    void setTimeFormat(QString);
    void setBytesPerLine(int bpl);
    void loadFilterFile(QString filename);
    void saveFilterFile(QString filename);
    void setSearchFilter(const QString &text);
    void normalizeTiming();
    void recalcOverwrite();
    bool needsFilterRefresh();
    void insertFrames(const QVector<CommFrame> &newFrames);
    void insertFrames(FrameStore &staged); //takes the frames out of staged (it is empty after)
    void sortByColumn(int column);
    int getIndexFromTimeID(unsigned int ID, double timestamp);
    //web-savvycan: read-only, thread-safe views (FrameListView: count(), at(i), mid(), tail(), toVector()).
    //getListReference() = every stored frame (a ring: the oldest go once MaximumFrames is reached),
    //getFilteredListReference() = the rows of the main trace (an index list into the same frames, or the
    //one-row-per-ID list in overwrite mode).
    const FrameListView *getListReference() const; //thou shalt not modify these frames externally!
    const FrameListView *getFilteredListReference() const; //Thus saith the Lord, NO.
    //Frames the ring has dropped from its front (oldest) to make room since the last clear.
    quint64 framesWrapped() const;
    //Frames taken in (live or loaded) since the last clear, including those wrapped out since.
    quint64 framesIngested() const { return mIngested; }
    qsizetype storeCapacity() const;
    qsizetype storeMemoryBytes() const { return store.memoryBytes(); } //the frames' compact records (framestore.h)
    // web-savvycan: frames stored but not yet announced (framesUpdated(n) on the next GUI tick, sendBulkRefresh)
    int unannouncedFrames() const { return lastUpdateNumFrames; }
    const QMap<int, bool> *getFiltersReference() const; //this neither
    const QMap<int, bool> *getBusFiltersReference() const; //this neither
    const QMap<int, bool> *getDirFiltersReference() const;
    const QMap<int, bool> *getLengthFiltersReference() const;
    const QMap<int, bool> *getTypeFiltersReference() const;

    // web-savvycan: the trace's frame type filter (CAN FD versus classic).
    enum TypeFilterKey { TypeClassic = 0, TypeFD = 1, TypeFDBRS = 2, TypeRemote = 3, TypeError = 4, TypeOther = 5 };
    static int typeFilterKey(const CommFrame &frame);
    static QString typeFilterLabel(int key);
    static QString typeFilterToolTip(int key);
    bool passesFrameFilters(const CommFrame &frame);

public slots:
    void addFrame(const CommFrame&, bool);
    void addFrames(const CANConnection*, const QVector<CommFrame>&);

signals:
    void updatedFiltersList();
    //web-savvycan: rows removed from the top of the trace because the ring wrapped (count of rows).
    void framesTrimmed(int rows);

private:
    class FilteredView : public FrameListView
    {
    public:
        FilteredView(const CommFrameModel *m) : FrameListView(&m->store.lock()), mModel(m) {}
    protected:
        qsizetype sizeLocked() const override;
        bool frameLocked(qsizetype i, CommFrame &out) const override;
    private:
        const CommFrameModel *mModel;
    };
    friend class FilteredView;

    uint64_t getCommFrameVal(const CommFrame &frame, Column col) const;
    static quint64 overwriteKey(const CommFrame &frame);
    bool frameForRow(int row, CommFrame &out) const;
    void ingestLocked(const CommFrame &frame, bool applyOffset); //store's write lock held
    void registerFilters(const CommFrame &frame);
    void pruneTrimmed();   //drop index rows whose frames the ring has dropped (no lock held)
    void publishRows();    //tell the views about rows added since the last call (no lock held)
    quint64 changingKey(const CommFrame &frame) const;
    bool any_filters_are_configured(void);
    bool any_busfilters_are_configured(void);
    bool any_dirfilters_are_configured(void);
    bool any_lengthfilters_are_configured(void);
    bool any_typefilters_are_configured(void);
    void registerFilterKeys(const CommFrame &frame, bool newOnesEnabled);

    FrameStore store;                       //every frame (ring)
    std::deque<quint64> filteredSeqs;        //rows of the trace in normal mode: seqs into store
    bool filteredSorted = false;             //filteredSeqs no longer in arrival order (sortByColumn)
    QVector<CommFrame> overwriteFrames;      //rows of the trace in overwrite mode (one per ID and bus)
    QHash<quint64, int> overwriteRow;        //overwriteKey -> row in overwriteFrames
    qsizetype publishedRows = 0;             //rows the views have been told about (rowCount())
    bool pendingPrune = false;
    quint64 mIngested = 0;                   //frames taken in since the last clear
    quint64 mWrappedBase = 0;                //store.totalWrapped() at the last clear
    FrameStoreView allView;
    FilteredView filteredView;
    QMap<int, bool> filters;
    QMap<int, bool> busFilters;
    QMap<int, bool> dirFilters;
    QMap<int, bool> lengthFilters;
    QMap<int, bool> typeFilters;
    DBCHandler *dbcHandler;
    bool interpretFrames; //should we use the dbcHandler?
    bool overwriteDups; //should we display all frames or only the newest for each ID?
    bool filtersPersistDuringClear;
    QString timeFormat;
    TimeStyle timeStyle;
    bool useHexMode;
    bool useColorsByCanId;
    bool useHexAndDecMode;
    bool changingOnly;
    QHash<quint64, QByteArray> lastSeenData;
    bool needFilterRefresh;
    bool ignoreDBCColors;
    int64_t timeOffset;
    int lastUpdateNumFrames;
    uint32_t preallocSize;
    bool sortDirAsc;
    int bytesPerLine;
    QString m_searchFilter;
};


#endif // CommFrameMODEL_H

