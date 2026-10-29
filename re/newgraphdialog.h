#ifndef NEWGRAPHDIALOG_H
#define NEWGRAPHDIALOG_H

#include <QDialog>
#include "graphingwindow.h"
#include "dbc/dbchandler.h"

class QPushButton;

namespace Ui {
class NewGraphDialog;
}

class NewGraphDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewGraphDialog(DBCHandler *handler, QWidget *parent = 0);
    ~NewGraphDialog();
    void showEvent(QShowEvent*);
    void getParams(GraphParams &);
    void setParams(GraphParams &);
    void clearParams();
    // web-savvycan: the frames of the graphing window, so the bit grid can cover the ID's frame length (CAN FD)
    void setFrames(const FrameListView *frames) { modelFrames = frames; }

private slots:
    void addButtonClicked();
    void colorSwatchClick();
    void fillSwatchClick();
    void loadNodes();
    void loadMessages(int idx);
    void loadSignals(int idx);
    void bitfieldClicked(int);
    void handleStartBitUpdate();
    void handleDataLenUpdate();
    void drawBitfield();
    void copySignalToParamsUI();
    void handleIdUpdate();

private:
    bool eventFilter(QObject *obj, QEvent *event);
    void checkSignalAgreement();
    void fitGrid(int highestByte);
    static void setSwatchColor(QPushButton *swatch, const QColor &color);

    Ui::NewGraphDialog *ui;
    DBCHandler *dbcHandler;
    DBC_SIGNAL *assocSignal;
    int startBit, dataLen;
    bool shownFromPlotEdit = false;
    const FrameListView *modelFrames = nullptr;
    int frameLenHint = 0; // bytes in the newest frame of the ID in the trace (0: none seen)
};

#endif // NEWGRAPHDIALOG_H
