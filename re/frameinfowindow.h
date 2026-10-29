#ifndef FRAMEINFOWINDOW_H
#define FRAMEINFOWINDOW_H

#include <QDialog>
#include "framestore.h"
#include <QFile>
#include <QIODevice>
#include <QListWidget>
#include <QSet>
#include <QTreeWidget>
#include <candatagrid.h>
#include "can_structs.h"
#include "bus_protocols/j1939_handler.h"
#include "dbc/dbchandler.h"

#include "qcustomplot.h"

namespace Ui {
class FrameInfoWindow;
}

class FrameInfoWindow : public QDialog
{
    Q_OBJECT

public:
    // web-savvycan: CAN FD: a frame has up to 64 bytes, 512 bits; every per-byte and per-bit table is this size.
    static constexpr int kMaxBytes = 64;
    static constexpr int kMaxBits = kMaxBytes * 8;
    // At most this many points in each byte graph: past it, each point pair is the min and max of a run of frames
    // (a capture of a million frames of one ID stays a few MB and quick to draw).
    static constexpr int kMaxBytePoints = 8000;

    // What the last analysis of an ID found (the heatmap and histograms are drawn from it; the tests read it).
    struct Analysis {
        bool valid = false;
        uint32_t id = 0;
        int frames = 0;
        int minLen = 0, maxLen = 0;
        int flips[kMaxBits] = {};        // changes of each bit from one frame of the ID to the next
        int sets[kMaxBits] = {};         // frames with each bit set (Bitfield Histogram)
        unsigned char heat[kMaxBits] = {}; // the heatmap's level of each bit, 0..255
        int byteGraphs = 0;              // byte graphs shown (one per byte of the longest frame)
        int bytePoints = 0;              // points in each of them
    };
    const Analysis &analysis() const { return lastAnalysis; }

    explicit FrameInfoWindow(const FrameListView *frames, QWidget *parent = 0);
    ~FrameInfoWindow();
    void showEvent(QShowEvent*);

private slots:
    void updateDetailsWindow(QString);
    void updatedFrames(int);
    void saveDetails();
    void mousePress();
    void mouseWheel();
    void mouseDoubleClick();
    void themeChanged();

private:
    Ui::FrameInfoWindow *ui;
    QCustomPlot *graphByte[64];
    QCustomPlot *graphHistogram;
    CANDataGrid *heatmap;

    QSet<uint32_t> foundID;
    const FrameListView *modelFrames;
    bool useOpenGL;
    bool useHexTicker;
    DBCHandler *dbcHandler;
    Analysis lastAnalysis;
    int shownByteGraphs = 8;

    QCPGraph *graphRef[64];

    static QColor byteGraphColor(int byte);
    void addFoundID(uint32_t id);
    void selectFirstID();
    void clearDisplay();
    void writeAllDetails(QIODevice *out);
    void refreshIDList();
    void closeEvent(QCloseEvent *event);
    bool eventFilter(QObject *obj, QEvent *event);
    void setupByteGraph(QCustomPlot *plot, int num);
    void readSettings();
    void writeSettings();
    void dumpNode(QTreeWidgetItem* item, QIODevice *file, int indent);
};

#endif // FRAMEINFOWINDOW_H
