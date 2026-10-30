#ifndef SNIFFERMODEL_H
#define SNIFFERMODEL_H

#include <QAbstractItemModel>
#include <QModelIndex>
#include <QVariant>
#include <QTimer>

#include "can_structs.h"
#include "connections/canconnection.h"
#include "snifferitem.h"


enum fltType
{
    ALL,
    ADD,
    REMOVE,
    NONE
};

class SnifferModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit SnifferModel(QObject *parent = 0);
    virtual ~SnifferModel();

    /* from QAbstractItemModel */
    QVariant data(const QModelIndex &index, int role) const Q_DECL_OVERRIDE;
    Qt::ItemFlags flags(const QModelIndex &index) const Q_DECL_OVERRIDE;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const Q_DECL_OVERRIDE;
    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const Q_DECL_OVERRIDE;
    QModelIndex parent(const QModelIndex &index) const Q_DECL_OVERRIDE;
    int rowCount(const QModelIndex &parent = QModelIndex()) const Q_DECL_OVERRIDE;
    int columnCount(const QModelIndex &parent = QModelIndex()) const Q_DECL_OVERRIDE;
    void refresh();
    void clear();
    void filter(fltType pType, int pId=0);
    bool getNeverExpire();
    bool getFadeInactive();
    bool getMuteNotched();
    void setNeverExpire(bool val);
    void setFadeInactive(bool val);
    void setMuteNotched(bool val);
    void setExpireInterval(int newVal);
    void updateNotchPoint();

    // web-savvycan: at most kMaxIds IDs (a bus of random 29-bit IDs would grow the list without end with
    // Never Expire); frames of further IDs are counted, not shown.
    static constexpr int kMaxIds = 4096;
    // For the test hook (testhook.cpp).
    qint64 framesSeen() const { return mFramesSeen; }
    qint64 framesOfHiddenIds() const { return mHiddenFrames; }
    const QMap<quint32, SnifferItem*> &items() const { return mFilter ? mFilters : mMap; }
    const QMap<quint32, SnifferItem*> &allItems() const { return mMap; }
    bool filtering() const { return mFilter; }


public slots:
    void update(CANConnection*, QVector<CommFrame>&);
    void notch();
    void unNotch();

signals:
    void idChange(int, bool);

private:
    void rebuildRows();

    QMap<quint32, SnifferItem*> mMap;
    QMap<quint32, SnifferItem*> mFilters;
    QVector<SnifferItem*>       mRows;      // web-savvycan: the shown map's items in row order (index() was O(rows))
    bool                        mFilter;
    bool                        mNeverExpire;
    bool                        mFadeInactive;
    bool                        mMuteNotched;
    quint32                     mTimeSequence;
    quint32                     mExpireInterval;
    qint64                      mFramesSeen = 0;
    qint64                      mHiddenFrames = 0;
};

#endif // SNIFFERMODEL_H
