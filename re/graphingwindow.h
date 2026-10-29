#ifndef GRAPHINGWINDOW_H
#define GRAPHINGWINDOW_H

#include "qcustomplot.h"
#include "framestore.h"
#include "can_structs.h"
#include "dbc/dbchandler.h"

#include <QAction>
#include <QDialog>
#include <QHBoxLayout>
#include <QMenuBar>

namespace Ui {
class GraphingWindow;
}

class GraphParams
{
public:
    GraphParams();

    uint32_t ID;
    int startBit, numBits;
    bool intelFormat;
    bool isSigned;
    uint64_t mask;
    double bias;
    double scale;
    int stride;
    int strideSoFar;
    int bus;
    int frameLen;
    QColor lineColor;
    QColor fillColor;
    int lineWidth;
    bool drawOnlyPoints;
    int pointType;
    QCPGraph *ref;
    QString graphName;
    DBC_SIGNAL *associatedSignal;

    // web-savvycan: a random line colour that shows on the plot in the theme in effect (dark lines on light, light
    // ones on dark); the Add New Graph dialog and the trace's "Add to graph" use it.
    static QColor randomLineColor();

    //the below stuff is used for internal purposes only - code should be refactored so these can be private
    // web-savvycan: the points live only in the plot's own container (ref->data()); no copy here (G6). Bounded by
    // GraphingWindow::boundGraph(): [envelope of the older samples | the newest samples exactly].
    int envelopePoints;      // points at the start of ref->data() that are the envelope (2 per bucket)
    int bucketSamples;       // samples per envelope bucket (doubles whenever the envelope is halved)
    double samples;          // samples taken in since the graph was (re)made (the tests' check of the bound)
    double xbias;
    int64_t prevValTable;
    QPointF prevValLocation;
    QString prevValStr;
    QCPItemBracket *lastBracket;
    QList<QCPItemBracket *> brackets;
    QList<QCPItemText *> bracketTexts;
};

class GraphingWindow : public QDialog
{
    Q_OBJECT

public:
    explicit GraphingWindow(const FrameListView *, QWidget *parent = 0);
    ~GraphingWindow();
    void showEvent(QShowEvent*);

public slots:
    void createGraph(GraphParams &params, bool createGraphParam = true);

private slots:
    void titleDoubleClick(QMouseEvent *event, QCPTextElement *title);
    void axisDoubleClick(QCPAxis* axis, QCPAxis::SelectablePart part);
    void legendDoubleClick(QCPLegend* legend, QCPAbstractLegendItem* item);
    void legendSingleClick(QCPLegend* legend, QCPAbstractLegendItem* item);
    void plottableDoubleClick(QCPAbstractPlottable* plottable,int dataIdx, QMouseEvent* event);
    void plottableClick(QCPAbstractPlottable* plottable, int dataIdx, QMouseEvent* event);
    void selectionChanged();
    void mousePress();
    void mouseWheel();
    void removeSelectedGraph();
    void removeAllGraphs();
    void contextMenuRequest(QPoint pos);
    void moveLegend();
    void saveGraphs();
    void saveSpreadsheet();
    void saveDefinitions();
    void loadDefinitions();
    void rescaleAxis(QCPAxis* axis);
    void rescaleToData();
    void toggleFollowMode();
    void addNewGraph();    
    void appendToGraph(GraphParams &params, CommFrame &frame, QVector<double> &x, QVector<double> &y);
    void editSelectedGraph();
    void updatedFrames(int);
    void gotCenterTimeID(uint32_t ID, double timestamp);
    void resetView();
    void zoomIn();
    void zoomOut();

signals:
    void sendCenterTimeID(uint32_t ID, double timestamp);

public:
    // web-savvycan: the bound of every graph's points (G6, RESOLVE.md stage 2). The newest kRecentPoints samples are
    // kept exactly; older ones are folded, kFoldPoints at a time, into a min/max envelope of at most
    // kEnvelopePoints points (two per bucket: the bucket's first and last time, carrying its extremes in the order
    // they came), whose buckets double in size whenever it is full, so the whole session stays on the graph.
    static constexpr int kRecentPoints = 200000;
    static constexpr int kFoldPoints = 50000;
    static constexpr int kEnvelopePoints = 150000;
    static constexpr int kFirstBucketSamples = 4;
    static constexpr int kMaxPoints = kRecentPoints + kFoldPoints + kEnvelopePoints; // 400 000 points, 6.4 MB
    static constexpr int kMaxBrackets = 500; // value-table brackets kept per graph (G5), the newest

    const QList<GraphParams> &graphs() const { return graphParams; } // for the test hook

private:
    Ui::GraphingWindow *ui;
    DBCHandler *dbcHandler;
    const FrameListView *modelFrames;
    QList<GraphParams> graphParams;
    QPen selectedPen;
    QCPSelectionDecorator *selDecorator;
    QCPItemText *locationText;
    QCPItemTracer *itemTracer;
    bool needScaleSetup; //do we need to set x,y graphing extents?
    bool useOpenGL;
    bool followGraphEnd;

    QAction *actEditSelected;
    QAction *actRemoveSelected;
    QAction *actRemoveAll;
    QAction *actRescale;
    QAction *actFollowEnd;

    void setupMenuBar();
    void boundGraph(GraphParams &params);
    void trimBrackets(GraphParams &params);
    void showParamsDialog(int idx);
    void closeEvent(QCloseEvent *event);
    void readSettings();
    void writeSettings();
    bool eventFilter(QObject *obj, QEvent *event);
    void changeEvent(QEvent *event);
};

#endif // GRAPHINGWINDOW_H
