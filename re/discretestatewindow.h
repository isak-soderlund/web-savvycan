#ifndef DISCRETESTATEWINDOW_H
#define DISCRETESTATEWINDOW_H

#include <QDialog>
#include "framestore.h"
#include <QHash>
#include <QTimer>
#include "can_structs.h"
#include "re/retools.h"
#include <vector>

class CANConnection;

namespace Ui {
class DiscreteStateWindow;
}

namespace DWStates {
enum DiscreteWindowState
{
    IDLE,
    COUNTDOWN_SIGNAL,
    COUNTDOWN_WAITING,
    GETTING_SIGNAL,
    DONE
};
}

using namespace DWStates;
class DiscreteStateWindow : public QDialog
{
    Q_OBJECT

public:
    explicit DiscreteStateWindow(const FrameListView *frames, QWidget *parent = 0);
    ~DiscreteStateWindow();
    void showEvent(QShowEvent*);

    static constexpr int kMaxMatchesShown = 1000;
    // For the test hook (testhook.cpp).
    DiscreteWindowState state() const { return operatingState; }
    int toggleState() const { return currToggleState; }   // the state asked for (0-based): "Go to state N+1"
    int iteration() const { return currIteration; }
    bool recording() const { return operatingState == DWStates::GETTING_SIGNAL && recordingNow; }
    bool realtime() const { return isRealtime; }
    const std::vector<ReTools::DiscreteMatch> &matches() const { return results; }
    const ReTools::StateSampler &sampler() const { return samples; }
    qint64 lastRunMs() const { return runMs; }
    int runs() const { return runCount; }
    void start() { handleStartButton(); }

private slots:
    void updatedFrames(int);
    void handleStartButton();
    void handleTick();
    void typeChanged();
    void framesReceived(CANConnection *, QVector<CommFrame> &frames);

private:
    Ui::DiscreteStateWindow *ui;
    const FrameListView *modelFrames;
    QTimer *timer;
    DiscreteWindowState operatingState;
    int ticksUntilStateChange;
    int ticksPerStateChange;
    int numToggleStates;
    int currToggleState;
    int numIterations;
    int currIteration;
    bool isRealtime;
    bool recordingNow = false;   // past the first quarter of a "Go to state" period (time to get there)
    QHash<int, bool> idFilters;
    ReTools::StateSampler samples;   // realtime: frames per ID and state, bounded
    std::vector<ReTools::DiscreteMatch> results;
    qint64 runMs = 0;
    int runCount = 0;

    void refreshFilterList();
    void addFilterId(const CommFrame &frame);
    void closeEvent(QCloseEvent *event);
    bool eventFilter(QObject *obj, QEvent *event);
    void readSettings();
    void writeSettings();
    void updateStateLabel();
    void setLabelColor(int which);   // 0 red, 1 blue, 2 green: SavvyCAN's, lighter in dark
    void calculateResults();
    void showResults();
};

#endif // DISCRETESTATEWINDOW_H
