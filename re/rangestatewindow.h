#ifndef RANGESTATEWINDOW_H
#define RANGESTATEWINDOW_H

#include <QDialog>
#include "framestore.h"
#include <QMap>
#include "can_structs.h"
#include "re/retools.h"
#include <vector>

class QCustomPlot;

namespace Ui {
class RangeStateWindow;
}

class RangeStateWindow : public QDialog
{
    Q_OBJECT

public:
    explicit RangeStateWindow(const FrameListView *frames, QWidget *parent = 0);
    ~RangeStateWindow();
    void showEvent(QShowEvent*);

    // For the test hook (testhook.cpp).
    const std::vector<ReTools::RangeCandidate> &candidates() const { return foundSignals; }
    bool busy() const { return running; }
    qint64 lastRunMs() const { return runMs; }
    qint64 lastRunFrames() const { return runFrames; }
    QCustomPlot *plot() const;

private slots:
    void updatedFrames(int);
    void recalcButton();
    void clickedSignalList(int idx);

private:
    Ui::RangeStateWindow *ui;
    const FrameListView *modelFrames;
    std::vector<ReTools::RangeCandidate> foundSignals;
    QMap<int, bool> idFilters;
    bool running = false;
    qint64 runMs = 0, runFrames = 0;

    void refreshFilterList();
    void closeEvent(QCloseEvent *event);
    void readSettings();
    void writeSettings();
    void addFilterId(quint32 id);
    ReTools::IdFrames framesOf(quint32 id) const;
    void createGraph(const QVector<double> &values);
    bool eventFilter(QObject *obj, QEvent *event);
};

#endif // RANGESTATEWINDOW_H
