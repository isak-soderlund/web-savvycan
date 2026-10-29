#ifndef SEARCHWINDOW_H
#define SEARCHWINDOW_H

#include <QDialog>
#include "framestore.h"
#include "../can_structs.h"
#include "../dbc/dbchandler.h"

namespace Ui {
class SearchWindow;
}

class SearchWindow : public QDialog
{
    Q_OBJECT

public:
    explicit SearchWindow(const FrameListView *frames, DBCHandler *handler, QWidget *parent = nullptr);
    ~SearchWindow();

    void updatedFrames(int numFrames);

    // for the tests (testhook): the matching rows of the trace, the current one, the last search's time
    const QVector<int> &matches() const { return matchingIndices; }
    int currentMatch() const { return currentMatchIndex; }
    double lastSearchMs() const { return searchMs; }

signals:
    void jumpToFrameIndex(int frameIndex);

private slots:
    void loadNodes();
    void loadMessages(int idx);
    void loadSignals(int idx);
    void copySignalToParamsUI();
    void bitfieldClicked(int bit);
    void handleStartBitUpdate();
    void handleDataLenUpdate();
    void drawBitfield();
    void onSearchClicked();
    void onNextClicked();
    void onPreviousClicked();
    void onValueModeChanged();

private:
    void performSearch();
    void updateNavigationState();
    void fitGrid(int highestByte);
    void handleIdUpdate();

    Ui::SearchWindow *ui;
    const FrameListView *modelFrames;
    DBCHandler *dbcHandler;
    DBC_SIGNAL *assocSignal;
    int startBit;
    int dataLen;

    QVector<int> matchingIndices;
    int currentMatchIndex;
    // web-savvycan: the trace's first frame when the matches were found (new frames keep them valid while it stays)
    CommFrame firstAtSearch;
    int frameLenHint = 0;   // the typed ID's frame length (the bit grid's size)
    double searchMs = 0;
};

#endif // SEARCHWINDOW_H
