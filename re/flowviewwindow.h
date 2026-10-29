#ifndef FLOWVIEWWINDOW_H
#define FLOWVIEWWINDOW_H

#include <QDialog>
#include "framestore.h"
#include <QLocale>
#include <QSet>
#include <QSlider>
#include "qcustomplot.h"
#include "can_structs.h"
#include "re/retools.h"

namespace Ui {
class FlowViewWindow;
}

enum TABLE_BYTE
{
    BKEY,
    CURR,
    REF,
    TRIGGER,
    GRAPH_EN,
    GRAPH_COLOR,
};

class FlowViewWindow : public QDialog
{
    Q_OBJECT

public:
    explicit FlowViewWindow(const FrameListView *frames, QWidget *parent = 0);
    ~FlowViewWindow();
    void showEvent(QShowEvent*);

    // web-savvycan: the frames of the selected ID, a ring of the newest kMaxFrames (memory bound on a busy bus).
    static constexpr int kMaxFrames = 200000;
    // For the test hook (testhook.cpp).
    const ReTools::IdFrames &cache() const { return frameCache; }
    bool hasId() const { return haveCacheId; }
    quint32 cacheId() const { return cacheIdValue; }
    int position() const { return currentPosition; }
    const unsigned char *currentBytes() const { return currBytes; }
    const unsigned char *referenceBytes() const { return refBytes; }
    bool isPlaying() const { return playbackActive; }
    int graphFrom() const { return graphFirst; }
    int graphTo() const { return graphLast; }
    QCustomPlot *plot() const;
    void selectId(quint32 id);           // as a click on the ID in the list
    void toggleTriggerBit(int bit) { gotCellClick(bit); }
    void setTriggerValue(int byte, int value);
    void playForward() { btnPlayClick(); }
    void playBackward() { btnReverseClick(); }
    void pause() { btnPauseClick(); }
    void stop() { btnStopClick(); }
    void stepForward() { btnFwdOneClick(); }
    void stepBack() { btnBackOneClick(); }
    void goTo(int frame) { gotoFrame(frame); }

private slots:
    void btnBackOneClick();
    void btnPauseClick();
    void btnReverseClick();
    void btnStopClick();
    void btnPlayClick();
    void btnFwdOneClick();
    void changePlaybackSpeed(int newSpeed);
    void changeLooping(bool check);
    void timerTriggered();
    void changeID(QString);
    void updatedFrames(int);
    void contextMenuRequestFlow(QPoint pos);
    void contextMenuRequestGraph(QPoint pos);
    void saveFileFlow();
    void saveFileGraph();
    void plottableDoubleClick(QCPAbstractPlottable* plottable, QMouseEvent* event);
    void gotCenterTimeID(uint32_t ID, double timestamp);
    void gotCellClick(int bitPosition);
    void graphRangeChanged(int range);
    void handleTableCellChange(int row, int col);
    void selectAllGraphs();
    void selectNoGraphs();

signals:
    void sendCenterTimeID(uint32_t ID, double timestamp);

private:
    Ui::FlowViewWindow *ui;
    QSet<quint32> foundID;
    ReTools::IdFrames frameCache{kMaxFrames};
    bool haveCacheId = false;
    quint32 cacheIdValue = 0;
    ReTools::NewFrames newFrames;   // which of the trace's frames are new (it is a ring)
    const FrameListView *modelFrames;
    unsigned char refBytes[64];
    unsigned char currBytes[64];
    int triggerValues[64];
    uint64_t triggerBits[64];
    int currentPosition;
    QTimer *playbackTimer;
    bool playbackActive;
    bool playbackForward;
    bool secondsMode;
    bool openGLMode;
    bool useHexTicker;
    QCPGraph *graphRef[64];
    QCPItemText *centerMark = nullptr;
    bool inhibitChangeCallback;
    // The graphs hold the frames graphFirst..graphLast of the cache (around the current frame, twice the chart range
    // each side), rebuilt when the view leaves them: never a point per frame of the whole cache.
    int graphFirst = 0, graphLast = -1;
    qint64 graphDropped = -1;
    bool graphsDirty = true;

    void refreshIDList();
    void updateFrameLabel();
    void updatePosition(bool forward);
    void processNewPosition();
    void gotoFrame(int frame);
    void updateDataView();
    void removeAllGraphs();
    void createGraph(int);
    void updateGraphLocation();
    void closeEvent(QCloseEvent *event);
    void readSettings();
    void writeSettings();
    bool eventFilter(QObject *obj, QEvent *event);
    void setupByteTable(int bytes);
    void rebuildGraphs(int from, int to);
    void growByteTable(int oldRows, int newRows);
    double graphKey(int i) const;
    void loadCurrentBytes();
    void followDrops(qint64 droppedBefore);
    void themeChanged();
};

#endif // FLOWVIEWWINDOW_H
