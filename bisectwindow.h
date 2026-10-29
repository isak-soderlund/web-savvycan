#ifndef BISECTWINDOW_H
#define BISECTWINDOW_H

#include <QDialog>
#include "framestore.h"
#include "can_structs.h"
#include <QSet>
#include <limits>

namespace Ui {
class BisectWindow;
}

class BisectWindow : public QDialog
{
    Q_OBJECT

public:
    explicit BisectWindow(const FrameListView *frames, QWidget *parent = 0);
    ~BisectWindow();
    void showEvent(QShowEvent*);

signals:
    void sendCommFrame(const CommFrame *, int);
    void sendFrameBatch(const QList<CommFrame> *);

private slots:
    void updatedFrames(int numFrames);
    void handleSaveButton();
    void handleReplaceButton();
    void handleCalculateButton();
    void updateFrameNumSlider();
    void updatePercentSlider();
    void updateFrameNumText();
    void updatePercentText();
    void updateSectionsText();

private:
    Ui::BisectWindow *ui;
    const FrameListView *modelFrames;
    // web-savvycan: the split frames in a compact store (36 bytes a classic frame, 92 a 64-byte FD one; SavvyCAN's
    // QVector<CommFrame> took ~100-160 in the browser); "Replace main list" moves them into the trace
    FrameStore splitFrames{std::numeric_limits<qsizetype>::max(), FrameStore::DefaultChunkFrames};
    QSet<uint32_t> foundID;
    bool idListStale = true;

public:
    // for the tests (testhook): the split as Calculate Split made it
    const FrameStore &split() const { return splitFrames; }
    double lastCalculateMs() const { return calculateMs; }

private:
    double calculateMs = 0;
    void refreshIDList();
    void addIDs(const QVector<CommFrame> &frames);
    void refreshFrameNumbers();
    bool eventFilter(QObject *obj, QEvent *event);
};

#endif // BISECTWINDOW_H


