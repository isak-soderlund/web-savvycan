#ifndef SIGNALVIEWERWINDOW_H
#define SIGNALVIEWERWINDOW_H

#include <QDialog>
#include "framestore.h"
#include "dbc/dbchandler.h"

namespace Ui {
class SignalViewerWindow;
}

class SignalViewerWindow : public QDialog
{
    Q_OBJECT

public:
    explicit SignalViewerWindow(const FrameListView *frames, QWidget *parent = 0);
    ~SignalViewerWindow();
    const QList<DBC_SIGNAL *> &viewedSignals() const { return signalList; } // web-savvycan: the test hook
private slots:
    void loadNodes();
    void loadMessages(int idx);
    void loadSignals(int idx);
    void addSignal();
    void addSignal(DBC_SIGNAL *sig);
    void removeSelectedSignal();
    void updatedFrames(int);
    void saveSignalsFile();
    void loadSignalsFile();
    void appendSignalsFile();
    void clearSignalsTable();
    void clearSignalsTable(bool);
    void saveDefinitions();
    void loadDefinitions(bool);
    void dropRemovedSignals();

private:
    Ui::SignalViewerWindow *ui;
    DBCHandler *dbcHandler;

    DBC_MESSAGE *currentlySelectedMsg;

    QList<DBC_SIGNAL *> signalList;
    const FrameListView *modelFrames;

    int processFrame(const CommFrame &frame, QList<bool> &done);
    QByteArray definitionsText() const;
    void applyDefinitions(const QByteArray &text, bool append);
};

#endif // SIGNALVIEWERWINDOW_H
