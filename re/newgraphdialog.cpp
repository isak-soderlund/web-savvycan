#include "newgraphdialog.h"
#include "ui_newgraphdialog.h"
#include <QApplication>
#include <QColorDialog>
#include <QPushButton>
#include "theme.h"
#include <QRandomGenerator>
#include "utility.h"
#include "helpwindow.h"

NewGraphDialog::NewGraphDialog(DBCHandler *handler, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::NewGraphDialog)
{
    ui->setupUi(this);

    dbcHandler = handler;

    connect(ui->colorSwatch, SIGNAL(clicked(bool)), this, SLOT(colorSwatchClick()));
    connect(ui->fillSwatch, SIGNAL(clicked(bool)), this, SLOT(fillSwatchClick()));
    connect(ui->btnAddGraph, SIGNAL(clicked(bool)), this, SLOT(addButtonClicked()));

    // web-savvycan: a random line colour that shows on the plot: dark ones on light, light ones on dark
    setSwatchColor(ui->colorSwatch, GraphParams::randomLineColor());
    setSwatchColor(ui->fillSwatch, QColor(128,128,128,0)); //light gray, no opacity so it is disabled by default

    ui->coPointStyle->addItem("None");
    ui->coPointStyle->addItem("Dot");
    ui->coPointStyle->addItem("Cross");
    ui->coPointStyle->addItem("Plus");
    ui->coPointStyle->addItem("Circle");
    ui->coPointStyle->addItem("Disc");
    ui->coPointStyle->addItem("Square");
    ui->coPointStyle->addItem("Diamond");
    ui->coPointStyle->addItem("Star");
    ui->coPointStyle->addItem("Triangle");
    ui->coPointStyle->addItem("TriangleInverted");
    ui->coPointStyle->addItem("Cross Inside Square");
    ui->coPointStyle->addItem("Plus Inside Square");
    ui->coPointStyle->addItem("Cross Inside Circle");
    ui->coPointStyle->addItem("Plus Inside Circle");
    ui->coPointStyle->addItem("Peace Sign");

    connect(ui->cbNodes, SIGNAL(currentIndexChanged(int)), this, SLOT(loadMessages(int)));
    connect(ui->cbMessages, SIGNAL(currentIndexChanged(int)), this, SLOT(loadSignals(int)));
    connect(ui->gridData, SIGNAL(gridClicked(int)), this, SLOT(bitfieldClicked(int)));
    connect(ui->txtDataLen, SIGNAL(textChanged(QString)), this, SLOT(handleDataLenUpdate()));
    connect(ui->txtStartBit, SIGNAL(textChanged(QString)), this, SLOT(handleStartBitUpdate()));
    connect(ui->cbIntel, SIGNAL(toggled(bool)), this, SLOT(drawBitfield()));
    connect(ui->btnCopySignal, SIGNAL(clicked(bool)), this, SLOT(copySignalToParamsUI()));
    connect(ui->cbSignals, SIGNAL(currentIndexChanged(int)), this, SLOT(drawBitfield()));
    // web-savvycan: the bit grid follows the frame's length (CAN FD: up to 64 bytes)
    connect(ui->txtID, SIGNAL(textChanged(QString)), this, SLOT(handleIdUpdate()));
    connect(ui->editFrameLen, SIGNAL(textChanged(QString)), this, SLOT(drawBitfield()));

    startBit = 0;
    dataLen = 1;
    assocSignal = nullptr;

    loadNodes();

    installEventFilter(this);
}

NewGraphDialog::~NewGraphDialog()
{
    removeEventFilter(this);
    delete ui;
}

void NewGraphDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if(shownFromPlotEdit == false)
        loadNodes();
    drawBitfield();
    qDebug() << "S" << ui->gridData->geometry();
}

bool NewGraphDialog::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key())
        {
        case Qt::Key_F1:
            HelpWindow::getRef()->showHelp("graphsetup.md");
            break;
        }
        return true;
    } else {
        // standard event processing
        return QObject::eventFilter(obj, event);
    }
    return false;
}

void NewGraphDialog::addButtonClicked()
{
    accept();
}

// web-savvycan: a colour swatch shows its colour, with its "CHANGE" in black or white, whichever reads on it (a light
// line colour in the dark theme had light text on it); a transparent swatch keeps the theme's text colour.
void NewGraphDialog::setSwatchColor(QPushButton *swatch, const QColor &color)
{
    QPalette p = swatch->palette();
    p.setColor(QPalette::Button, color);
    if (color.alpha() >= 128) p.setColor(QPalette::ButtonText, color.lightness() >= 128 ? QColor(Qt::black) : QColor(Qt::white));
    else p.setColor(QPalette::ButtonText, QApplication::palette().color(QPalette::ButtonText));
    swatch->setPalette(p);
}

void NewGraphDialog::colorSwatchClick()
{
    QColor newColor = QColorDialog::getColor(ui->colorSwatch->palette().button().color());
    if (!newColor.isValid()) return; // cancelled
    setSwatchColor(ui->colorSwatch, newColor);
}

void NewGraphDialog::fillSwatchClick()
{
    QColor newColor = QColorDialog::getColor(ui->fillSwatch->palette().button().color(), nullptr, "Pick A Color", QColorDialog::ShowAlphaChannel);
    if (!newColor.isValid()) return; // cancelled
    setSwatchColor(ui->fillSwatch, newColor);
}

//check whether the current values on the left match the signal selected on the right
void NewGraphDialog::checkSignalAgreement()
{
    GraphParams testingParams;
    bool bAgree = true;
    bool sigSigned = false;
    DBC_SIGNAL *sig = nullptr;
    DBC_MESSAGE *msg = nullptr;

    if (dbcHandler == nullptr) return;
    if (dbcHandler->getFileCount() == 0) return;    

    QString fullyQualifiedNodeName = ui->cbNodes->itemText(ui->cbNodes->currentIndex());
    QString msgName = ui->cbMessages->currentText();
    msg = dbcHandler->findMessage(msgName, fullyQualifiedNodeName);
    if (msg)
    {
        sig = msg->sigHandler->findSignalByName(ui->cbSignals->currentText());
    }
    else
    {
        ui->lblMsgStatus->setText("Msg ID doesn't exist in DBC");
        return;
    }

    if (sig)
    {
        if (sig->valType == SIGNED_INT) sigSigned = true;

        testingParams.ID = Utility::ParseStringToNum(ui->txtID->text());
        testingParams.bias = ui->txtBias->text().toFloat();
        testingParams.isSigned = ui->cbSigned->isChecked();
        testingParams.intelFormat = ui->cbIntel->isChecked();
        testingParams.scale = ui->txtScale->text().toFloat();
        testingParams.startBit = startBit;
        testingParams.numBits = dataLen;

        if (testingParams.ID != msg->ID)
            bAgree = false;
        if (fabs(testingParams.bias - sig->bias) > 0.01)
            bAgree = false;
        if (testingParams.isSigned != sigSigned)
            bAgree = false;
        if (testingParams.intelFormat != sig->intelByteOrder)
            bAgree = false;
        if (fabs(testingParams.scale - sig->factor) > 0.01)
            bAgree = false;
        if (testingParams.startBit != sig->startBit)
            bAgree = false;
        if (testingParams.numBits != sig->signalSize)
            bAgree = false;
    }
    else
    {
        ui->lblMsgStatus->setText("Signal name doesn't exist in DBC");
        return;
    }

    if (bAgree)
    {
        ui->lblMsgStatus->setText("Graph params match this signal");
        //assocSignal = sig;
    }
    else
    {
        ui->lblMsgStatus->setText("Signal and Graph Params do not match");
        //assocSignal = nullptr; //ya done broke it, null the associated signal since the user is changing things
    }
}

void NewGraphDialog::clearParams()
{
    ui->txtID->clear();
    ui->txtBias->clear();
    ui->txtMask->clear();
    ui->txtScale->clear();
    ui->txtStride->clear();
    ui->txtName->clear();
}

void NewGraphDialog::setParams(GraphParams &params)
{
    shownFromPlotEdit = true;

    ui->txtBias->setText(QString::number(params.bias));
    ui->txtMask->setText(Utility::formatNumber(params.mask));
    ui->txtScale->setText(QString::number(params.scale));
    ui->txtStride->setText(QString::number(params.stride));
    ui->cbSigned->setChecked(params.isSigned);
    ui->cbIntel->setChecked(params.intelFormat);

    startBit = params.startBit;
    ui->txtStartBit->blockSignals(true);
    ui->txtStartBit->setText(QString::number(startBit));
    ui->txtStartBit->blockSignals(false);
    dataLen = params.numBits;
    ui->txtDataLen->setText(QString::number(dataLen));
    ui->txtID->setText(Utility::formatCANID(params.ID));
    ui->txtName->setText(params.graphName);
    ui->editBus->setText(QString::number(params.bus));
    ui->editFrameLen->setText(QString::number(params.frameLen));
    setSwatchColor(ui->colorSwatch, params.lineColor);
    setSwatchColor(ui->fillSwatch, params.fillColor);
    ui->cbOnlyPoints->setChecked(params.drawOnlyPoints);
    ui->spinLineWidth->setValue(params.lineWidth);
    ui->coPointStyle->setCurrentIndex(params.pointType);

    assocSignal = params.associatedSignal;

    loadNodes();

    if (assocSignal)
    {
        auto msg = assocSignal->parentMessage;
        auto node = msg->sender;

        bool nodeFound = false;
        for(int i=0; i<ui->cbNodes->count(); i++)
        {
            if(ui->cbNodes->itemText(i) == node->sourceFileName + Utility::fullyQualifiedNameSeperator + node->name)
            {
                ui->cbNodes->setCurrentIndex(i);
                nodeFound = true;
                break;
            }
        }

        qDebug() << "Matching plot params to Node: " << nodeFound;

        if(nodeFound)
        {
            bool msgFound = false;
            for(int i=0; i<ui->cbMessages->count(); i++)
            {
                if(ui->cbMessages->itemText(i) == msg->name)
                {
                    ui->cbMessages->setCurrentIndex(i);
                    msgFound = true;
                    break;
                }
            }

            qDebug() << "Matching plot params to Msg: " << msgFound;

            if(msgFound)
            {
                bool sigFound = false;
                for(int i=0; i<ui->cbSignals->count(); i++)
                {
                    if(ui->cbSignals->itemText(i) == assocSignal->name)
                    {
                        ui->cbSignals->setCurrentIndex(i);
                        sigFound = true;
                        break;
                    }
                }

                qDebug() << "Matching plot params to Signal: " << sigFound;
            }
        }
    }
    //ui->cbNodes->model()->


    //loadSignals(0);
    drawBitfield();
    checkSignalAgreement();
}

void NewGraphDialog::getParams(GraphParams &params)
{
    params.lineColor = ui->colorSwatch->palette().button().color();
    params.fillColor = ui->fillSwatch->palette().button().color();
    params.graphName = ui->txtName->text();

    params.lineWidth = ui->spinLineWidth->value();
    params.drawOnlyPoints = ui->cbOnlyPoints->isChecked();
    params.pointType = ui->coPointStyle->currentIndex();

    params.ID = Utility::ParseStringToNum(ui->txtID->text());
    params.bias = ui->txtBias->text().toFloat();
    params.isSigned = ui->cbSigned->isChecked();
    params.intelFormat = ui->cbIntel->isChecked();
    params.mask = Utility::ParseStringToNum(ui->txtMask->text());
    params.scale = ui->txtScale->text().toFloat();
    params.stride = Utility::ParseStringToNum(ui->txtStride->text());
    params.bus = Utility::ParseStringToNum(ui->editBus->text());
    params.frameLen = ui->editFrameLen->text().toInt();

    params.startBit = startBit;
    params.numBits = dataLen;

    params.associatedSignal = assocSignal;

    //now catch stupidity and bring it to defaults
    if (params.mask == 0) params.mask = 0xFFFFFFFF;
    if (fabs(params.scale) < 0.00000001) params.scale = 1.0f;
    if (params.stride < 1) params.stride = 1;
}

void NewGraphDialog::loadNodes()
{
    int numFiles;
    ui->cbNodes->clear();
    if (dbcHandler == nullptr) return;
    if ((numFiles = dbcHandler->getFileCount()) == 0) return;
    qDebug() << numFiles;
    for (int f = 0; f < numFiles; f++)
    {
        DBCFile* thisFile = dbcHandler->getFileByIdx(f);
        qDebug() << thisFile->messageHandler->getCount();

        QList<QString> names;
        QList<DBC_MESSAGE*> msgs = thisFile->messageHandler->getMsgsAsList();

        for (int x = 0; x < thisFile->dbc_nodes.count(); x++)
        {
            bool messagesInNode = false;
            for (int m = 0; m < thisFile->messageHandler->getCount(); m++)
            {
                if(msgs[m]->sender->name == thisFile->dbc_nodes[x]->name)
                {
                    messagesInNode = true;
                    break;
                }
            }
            if(messagesInNode)
            {
                QString fullyQualifiedNodeName = thisFile->getFilenameNoExt() + Utility::fullyQualifiedNameSeperator + thisFile->dbc_nodes[x]->name;
                names.append(fullyQualifiedNodeName);
            }
        }

        if(names.count() > 0)
        {
            names.sort();
            ui->cbNodes->addItem("----" + thisFile->getFilename());
            Utility::SetComboBoxItemEnabled(ui->cbNodes, ui->cbNodes->count() -1, false);
            for(int i=0; i<names.count(); i++)
                ui->cbNodes->addItem(names[i]);
        }
    }
}

void NewGraphDialog::loadMessages(int idx)
{
    int numFiles = 0;
    ui->cbMessages->clear();
    if (dbcHandler == nullptr) return;
    if ((numFiles = dbcHandler->getFileCount()) == 0) return;
    qDebug() << numFiles;

    QString displayedNodeName = ui->cbNodes->itemText(idx);

    for (int f = 0; f < numFiles; f++)
    {
        qDebug() << dbcHandler->getFileByIdx(f)->messageHandler->getCount();

        QList<DBC_MESSAGE*> msgs = dbcHandler->getFileByIdx(f)->messageHandler->getMsgsAsList();

        for (int x = 0; x < msgs.count(); x++)
        {
            QString fullyQualifiedNodeName = dbcHandler->getFileByIdx(f)->getFilenameNoExt() + Utility::fullyQualifiedNameSeperator + msgs[x]->sender->name;
            if(fullyQualifiedNodeName == displayedNodeName)
                ui->cbMessages->addItem(msgs[x]->name);
        }
    }
}

void NewGraphDialog::loadSignals(int idx)
{
    Q_UNUSED(idx);

    ui->cbSignals->clear();

    //search through all DBC files in order to try to find a message with the given name
    QString fullyQualifiedNodeName = ui->cbNodes->itemText(ui->cbNodes->currentIndex());
    DBC_MESSAGE *msg = dbcHandler->findMessage(ui->cbMessages->currentText(), fullyQualifiedNodeName);
    if (msg == nullptr) return;

    DBC_SIGNAL *sig;
    QList<DBC_SIGNAL *> sigs = msg->sigHandler->getSignalsAsList();

    for (int x = 0; x < msg->sigHandler->getCount(); x++)
    {
        sig = sigs[x];
        if (sig)
        {
            ui->cbSignals->addItem(sig->name);
            if (assocSignal && sig->name == assocSignal->name)
            {
                ui->cbSignals->setCurrentIndex(ui->cbSignals->count() - 1);
                //qDebug() << "Found me";
            }
        }
    }
    ui->cbSignals->model()->sort(0);
    checkSignalAgreement();
}

void NewGraphDialog::bitfieldClicked(int bit)
{
    qDebug() << "Clicked bit: " << bit;
    startBit = bit;
    ui->txtStartBit->blockSignals(true);
    ui->txtStartBit->setText(QString::number(startBit));
    ui->txtStartBit->blockSignals(false);
    drawBitfield();    
}

void NewGraphDialog::handleStartBitUpdate()
{
    startBit = ui->txtStartBit->text().toInt();
    if (startBit < 0) startBit = 0;
    if (startBit > 511) startBit = 511;
    drawBitfield();
}

void NewGraphDialog::drawBitfield()
{
    qDebug() << "Draw Bitfield";
    uint8_t bitField[64];  // 512 bits: a CAN FD frame
    uint8_t refField[64];  // the start bit only, so the grid sets it apart
    int endBit, sBit;

    memset(bitField, 0, 64);
    memset(refField, 0, 64);
    startBit = qBound(0, startBit, 511); // web-savvycan: a loaded definition can say anything

    //make the start bit a different color to set it apart
    refField[Utility::getByteFromBitPosition(startBit)] |= 1 << Utility::getBitFromBitPosition(startBit);
    bitField[Utility::getByteFromBitPosition(startBit)] |= 1 << Utility::getBitFromBitPosition(startBit);

    if (ui->cbIntel->isChecked())
    {
        endBit = startBit + dataLen - 1;
        if (startBit < 0) startBit = 0;
        if (endBit > 511) endBit = 511;
        for (int y = startBit; y <= endBit; y++)
        {
            //bitField |= 1ull << y;
            bitField[Utility::getByteFromBitPosition(y)] |= 1 << Utility::getBitFromBitPosition(y);
        }
    }
    else //big endian / motorola format
    {
        //much more irritating than the intel version...
        int size = dataLen;
        sBit = startBit;
        while (size > 0)
        {
            //bitField |= 1ull << sBit;
            bitField[Utility::getByteFromBitPosition(sBit)] |= 1 << Utility::getBitFromBitPosition(sBit);
            size--;
            if ((sBit % 8) == 0) sBit += 15;
            else sBit--;
            if (sBit > 511) sBit = 511;
        }
    }

    int highestByte = 0;
    for (int b = 0; b < 64; b++) if (bitField[b]) highestByte = b;
    fitGrid(highestByte);

    ui->gridData->setReference((unsigned char *)&refField, false);
    ui->gridData->updateData((unsigned char *)&bitField, true);

    checkSignalAgreement();
}

// web-savvycan: the bit grid covers the frame: the newest frame of this ID in the trace, the Frame Length field, the
// DBC message and the bits picked, whichever is longest; 8 bytes at least (SavvyCAN's grid), up to CAN FD's 64.
void NewGraphDialog::fitGrid(int highestByte)
{
    int bytes = qMax(8, qMax(frameLenHint, highestByte + 1));
    bytes = qMax(bytes, qMin(64, ui->editFrameLen->text().toInt()));
    bytes = bytes <= 8 ? 8 : bytes <= 16 ? 16 : bytes <= 32 ? 32 : 64; // the grid's layouts (CANDataGrid)
    if (ui->gridData->getBytesToDraw() == bytes) return;
    ui->gridData->setBytesToDraw(bytes);
    const int cols = bytes > 32 ? 32 : bytes > 8 ? 16 : 8, rows = bytes > 16 ? 16 : 8;
    ui->gridData->setMinimumSize(qMax(280, cols * 20 + 24), qMax(280, rows * 20 + 24));
}

// The frame length of the ID typed in: its newest frame among the last 100 000 in the trace, or its DBC message.
void NewGraphDialog::handleIdUpdate()
{
    frameLenHint = 0;
    const QString text = ui->txtID->text().trimmed();
    if (!text.isEmpty())
    {
        const uint32_t id = uint32_t(Utility::ParseStringToNum(text));
        if (dbcHandler)
            if (DBC_MESSAGE *msg = dbcHandler->findMessage(id)) frameLenHint = int(qMin(64u, msg->len));
        const qsizetype count = modelFrames ? modelFrames->count() : 0;
        for (qsizetype i = count - 1; i >= 0 && i >= count - 100000; i--)
        {
            const CommFrame frame = modelFrames->at(i);
            if (frame.frameId() == id && frame.isDataFrame())
            {
                frameLenHint = qMax(frameLenHint, int(frame.payload().length()));
                break;
            }
        }
    }
    drawBitfield();
}

void NewGraphDialog::handleDataLenUpdate()
{
    dataLen = ui->txtDataLen->text().toInt();
    if (dataLen < 1) dataLen = 1;
    if (dataLen > 64) dataLen = 64;
    drawBitfield();
    checkSignalAgreement();
}

void NewGraphDialog::copySignalToParamsUI()
{
    assocSignal = nullptr;
    DBC_MESSAGE *msg = nullptr;

    QString fullyQualifiedNodeName = ui->cbNodes->itemText(ui->cbNodes->currentIndex());
    QString msgName = ui->cbMessages->itemText(ui->cbMessages->currentIndex());

    msg = dbcHandler->findMessage(msgName, fullyQualifiedNodeName);

    if (!msg) return;
    DBC_SIGNAL *sig = msg->sigHandler->findSignalByName(ui->cbSignals->currentText());
    if (!sig) return;

    startBit = sig->startBit;
    ui->txtStartBit->blockSignals(true);
    ui->txtStartBit->setText(QString::number(startBit));
    ui->txtStartBit->blockSignals(false);
    ui->txtBias->setText(QString::number(sig->bias));
    ui->txtDataLen->setText(QString::number(sig->signalSize));
    ui->txtID->setText(Utility::formatCANID(msg->ID));
    ui->txtMask->setText("0xFFFFFFFF");
    ui->txtName->setText(sig->name);
    ui->txtScale->setText(QString::number(sig->factor));
    ui->txtStride->setText("1");
    ui->cbIntel->setChecked(sig->intelByteOrder);
    if (sig->valType == SIGNED_INT) ui->cbSigned->setChecked(true);
        else ui->cbSigned->setChecked(false);
    drawBitfield();
    assocSignal = sig;
    checkSignalAgreement();
}
