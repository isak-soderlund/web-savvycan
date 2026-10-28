#ifndef MOTORCONTROLLERCONFIGWINDOW_H
#define MOTORCONTROLLERCONFIGWINDOW_H

#include <QDialog>
#include "framestore.h"
#include <QTimer>
#include "can_structs.h"

namespace Ui {
class MotorControllerConfigWindow;
}

//Serial_Number_EEPROM                     , 0x0113, uint, dec, 16, 6, spr, spr, spr

enum PARAM_TYPE
{
    DEC,
    HEX,
    ASCII,
};

enum PARAM_SIGNED
{
    UNSIGNED,
    SIGNED,
    Q15
};

class PARAM
{
public:
    QString paramName;
    uint32_t paramID;
    PARAM_TYPE paramType;
    PARAM_SIGNED signedType;
    uint16_t value;
};


class MotorControllerConfigWindow : public QDialog
{
    Q_OBJECT

public:
    explicit MotorControllerConfigWindow(const FrameListView *frames, QWidget *parent = 0);
    ~MotorControllerConfigWindow();

signals:
    void sendCommFrame(const CommFrame *, int);
    void sendFrameBatch(const QList<CommFrame> *);

private slots:
    void updatedFrames(int numFrames);
    void refreshData();
    void saveData();
    void timerTick();
    void loadFile();

private:
    Ui::MotorControllerConfigWindow *ui;
    const FrameListView *modelFrames;
    QTimer timer;
    CommFrame outFrame;
    bool doingRequest;
    int transmitStep;
    QList<PARAM> params;

};

#endif // MOTORCONTROLLERCONFIGWINDOW_H
