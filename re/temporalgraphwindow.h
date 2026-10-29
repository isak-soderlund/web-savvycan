#ifndef TEMPORALGRAPHWINDOW_H
#define TEMPORALGRAPHWINDOW_H

#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <vector>
#include "framestore.h"
#include "qcustomplot.h"
#include "can_structs.h"

namespace Ui {
class TemporalGraphWindow;
}

/*
 * web-savvycan: the temporal graph's data (fixes G1-G4 of resolve-can docs/research/savvycan-improve.md).
 *
 * Frame counts per (ID, time column), in a fixed number of columns: memory depends on the number of IDs,
 * never on the number of frames or the length of the capture. A column is a power of two microseconds
 * wide; when a frame falls outside the columns, the width doubles (neighbouring columns are added
 * together) until every frame fits, so a 10-second capture and a 10-hour one both have at most
 * Columns columns. Rows are the distinct IDs (11-bit and 29-bit apart), in ID order, not the raw ID
 * range (29-bit IDs would need millions of rows). At most MaxIds IDs get a row; frames of further IDs
 * are counted as hidden.
 */
class TemporalDensity
{
public:
    static constexpr int Columns = 512;
    static constexpr int MaxIds = 2048;

    void clear();
    void add(qint64 timeUs, quint32 id, bool extended);

    bool isEmpty() const { return mFrames == 0; }
    quint64 frames() const { return mFrames; }              // frames counted (hidden ones included)
    quint64 hiddenFrames() const { return mHiddenFrames; }   // frames of IDs past MaxIds
    qint64 firstUs() const { return mFirstUs; }
    qint64 lastUs() const { return mLastUs; }
    qint64 originUs() const { return mOriginUs; }            // start of column 0
    qint64 binUs() const { return mBinUs; }                  // column width
    int usedColumns() const;                                 // columns from column 0 to the last frame's
    int ids() const { return int(mKeys.size()); }
    quint32 maxCount() const;
    std::size_t memoryBytes() const;                         // the counts and the ID index

    // IDs in display order (rows bottom to top): ext << 32 | id. Rebuilt only when an ID was added.
    const std::vector<quint64> &sortedKeys() const;
    // Counts of row `rank` of sortedKeys() (Columns entries).
    const std::vector<quint32> &countsOfRank(int rank) const;

    static quint64 key(quint32 id, bool extended) { return (quint64(extended ? 1 : 0) << 32) | id; }

private:
    void fit(qint64 lo, qint64 hi);
    void sortKeys() const;

    quint64 mFrames = 0, mHiddenFrames = 0;
    qint64 mFirstUs = 0, mLastUs = 0, mOriginUs = 0, mBinUs = 1;
    QHash<quint64, int> mSlot;                   // key -> index into mKeys / mCounts
    std::vector<quint64> mKeys;
    std::vector<std::vector<quint32>> mCounts;
    mutable std::vector<quint64> mSorted;
    mutable std::vector<int> mSortedSlot;
    mutable bool mSortedValid = false;
};

// The ID axis: one tick per row (or per n rows when they don't fit), labelled with the row's ID in hex.
class TemporalIdTicker : public QCPAxisTicker
{
public:
    explicit TemporalIdTicker(const TemporalDensity *density) : mDensity(density) {}
protected:
    double getTickStep(const QCPRange &range) override;
    int getSubTickCount(double tickStep) override;
    QString getTickLabel(double tick, const QLocale &locale, QChar formatChar, int precision) override;
private:
    const TemporalDensity *mDensity;
};

class TemporalGraphWindow : public QDialog
{
    Q_OBJECT

public:
    explicit TemporalGraphWindow(const FrameListView *, QWidget *parent = nullptr);
    ~TemporalGraphWindow();
    void showEvent(QShowEvent*);

    // For the test hook.
    const TemporalDensity &density() const { return mDensity; }
    bool building() const { return mBuilding; }
    QCustomPlot *plot() const;
    std::size_t mapBytes() const;   // the colour map's cells and its image

private slots:
    void updatedFrames(int);
    void mousePress();
    void mouseWheel();
    void resetView();
    void zoomIn();
    void zoomOut();
    void selectionChanged();
    void buildStep();
    void xRangeChanged(const QCPRange &range);

private:
    Ui::TemporalGraphWindow *ui;
    const FrameListView *modelFrames;
    bool useOpenGL;
    TemporalDensity mDensity;
    QCPColorMap *colorMap = nullptr;
    QCPTextElement *summary = nullptr;
    QSharedPointer<TemporalIdTicker> idTicker;
    double keyScale = 1.0;            // display units per µs (the app's time style)

    // Building from the frame list: in slices, so the page stays responsive on a big capture.
    QTimer buildTimer;
    bool mBuilding = false;
    qsizetype buildCursor = 0;
    QElapsedTimer buildReplotClock;

    // The newest frame counted, to tell new frames from counted ones in the list's tail.
    bool haveLast = false;
    CommFrame lastFrame;

    bool autoFit = true;              // the view shows the whole capture until the user zooms or pans
    bool settingRange = false;
    bool plotDirty = false;

    void closeEvent(QCloseEvent *event);
    bool eventFilter(QObject *obj, QEvent *event);
    void readSettings();
    void writeSettings();
    void generateGraph();
    void countFrame(const CommFrame &frame);
    void updatePlot();
    void fitView();
};

#endif // TEMPORALGRAPHWINDOW_H
