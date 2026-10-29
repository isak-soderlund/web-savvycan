#include "searchwindow.h"
#include "ui_searchwindow.h"
#include "../utility.h"
#include "../framefileio.h"
#include "filetools.h"
#include <QDebug>
#include <QElapsedTimer>
#include <cmath>
#include <cstring>

SearchWindow::SearchWindow(const FrameListView *frames, DBCHandler *handler, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SearchWindow)
    , modelFrames(frames)
    , dbcHandler(handler)
    , assocSignal(nullptr)
    , startBit(0)
    , dataLen(8)
    , currentMatchIndex(-1)
{
    ui->setupUi(this);
#ifdef Q_OS_WASM
    // web-savvycan: as the connection and graphing windows: a Qt::Window is frameless and covers the main window's
    // menus in the browser; as a dialog it has Qt's title bar and close button and can be moved aside.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
#else
    setWindowFlags(Qt::Window);
#endif

    // DBC signal pickers
    connect(ui->cbNodes,    SIGNAL(currentIndexChanged(int)), this, SLOT(loadMessages(int)));
    connect(ui->cbMessages, SIGNAL(currentIndexChanged(int)), this, SLOT(loadSignals(int)));
    connect(ui->btnCopySignal, SIGNAL(clicked(bool)), this, SLOT(copySignalToParamsUI()));

    // Bit-field grid
    connect(ui->gridData,    SIGNAL(gridClicked(int)), this, SLOT(bitfieldClicked(int)));
    connect(ui->txtStartBit, SIGNAL(textChanged(QString)), this, SLOT(handleStartBitUpdate()));
    connect(ui->txtDataLen,  SIGNAL(textChanged(QString)), this, SLOT(handleDataLenUpdate()));
    connect(ui->cbIntel,     SIGNAL(toggled(bool)),  this, SLOT(drawBitfield()));
    // web-savvycan: the grid covers the frames searched (CAN FD: up to 64 bytes)
    connect(ui->txtID, &QLineEdit::editingFinished, this, &SearchWindow::handleIdUpdate);
    connect(ui->spinFrameLen, &QSpinBox::valueChanged, this, &SearchWindow::drawBitfield);
    connect(ui->cbUseLen, &QCheckBox::toggled, this, &SearchWindow::drawBitfield);

    // Value mode toggle
    connect(ui->rbValueNone,     SIGNAL(toggled(bool)), this, SLOT(onValueModeChanged()));
    connect(ui->rbValueAny,      SIGNAL(toggled(bool)), this, SLOT(onValueModeChanged()));
    connect(ui->rbValueSpecific, SIGNAL(toggled(bool)), this, SLOT(onValueModeChanged()));

    // Navigation
    connect(ui->btnSearch, SIGNAL(clicked(bool)), this, SLOT(onSearchClicked()));
    connect(ui->btnNext,   SIGNAL(clicked(bool)), this, SLOT(onNextClicked()));
    connect(ui->btnPrev,   SIGNAL(clicked(bool)), this, SLOT(onPreviousClicked()));

    loadNodes();
    drawBitfield();
}

SearchWindow::~SearchWindow()
{
    delete ui;
}

// Called by MainWindow when the frame list changes
void SearchWindow::updatedFrames(int numFrames)
{
    // web-savvycan: new frames at the end of the trace leave the matches' rows where they were, unless the trace's ring
    // has dropped its oldest frames (rows move up): the first frame tells. Cleared, loaded, refiltered: stale.
    if (numFrames > 0 && !matchingIndices.isEmpty())
    {
        const CommFrame first = modelFrames->first();
        if (first.frameId() == firstAtSearch.frameId() && first.timeStamp().seconds() == firstAtSearch.timeStamp().seconds()
            && first.timeStamp().microSeconds() == firstAtSearch.timeStamp().microSeconds()
            && first.payload() == firstAtSearch.payload())
            return;
    }
    // Clear stale results when frames change
    if (!matchingIndices.isEmpty())
    {
        matchingIndices.clear();
        currentMatchIndex = -1;
        ui->lblMatchInfo->setText("Frames updated – press Search again.");
        updateNavigationState();
    }
}
// ──────────────────────────────────────────────
//  DBC hierarchy loading  (mirrors NewGraphDialog)
// ──────────────────────────────────────────────
void SearchWindow::loadNodes()
{
    ui->cbNodes->clear();
    if (!dbcHandler) return;
    int numFiles = dbcHandler->getFileCount();
    if (numFiles == 0) return;

    for (int f = 0; f < numFiles; f++)
    {
        DBCFile *thisFile = dbcHandler->getFileByIdx(f);
        QList<QString> names;
        QList<DBC_MESSAGE *> msgs = thisFile->messageHandler->getMsgsAsList();

        for (int x = 0; x < thisFile->dbc_nodes.count(); x++)
        {
            bool hasMessages = false;
            for (int m = 0; m < msgs.count(); m++)
            {
                if (msgs[m]->sender->name == thisFile->dbc_nodes[x]->name)
                {
                    hasMessages = true;
                    break;
                }
            }
            if (hasMessages)
            {
                QString fqName = thisFile->getFilenameNoExt()
                               + Utility::fullyQualifiedNameSeperator
                               + thisFile->dbc_nodes[x]->name;
                names.append(fqName);
            }
        }

        if (!names.isEmpty())
        {
            names.sort();
            ui->cbNodes->addItem("----" + thisFile->getFilename());
            Utility::SetComboBoxItemEnabled(ui->cbNodes, ui->cbNodes->count() - 1, false);
            for (const QString &n : names)
                ui->cbNodes->addItem(n);
        }
    }
}

void SearchWindow::loadMessages(int idx)
{
    ui->cbMessages->clear();
    if (!dbcHandler) return;
    if (dbcHandler->getFileCount() == 0) return;

    QString displayedNode = ui->cbNodes->itemText(idx);

    for (int f = 0; f < dbcHandler->getFileCount(); f++)
    {
        QList<DBC_MESSAGE *> msgs = dbcHandler->getFileByIdx(f)->messageHandler->getMsgsAsList();
        for (int x = 0; x < msgs.count(); x++)
        {
            QString fqName = dbcHandler->getFileByIdx(f)->getFilenameNoExt()
                           + Utility::fullyQualifiedNameSeperator
                           + msgs[x]->sender->name;
            if (fqName == displayedNode)
                ui->cbMessages->addItem(msgs[x]->name);
        }
    }
}

void SearchWindow::loadSignals(int /*idx*/)
{
    ui->cbSignals->clear();
    if (!dbcHandler) return;
    if (dbcHandler->getFileCount() == 0) return;

    QString fqNode = ui->cbNodes->itemText(ui->cbNodes->currentIndex());
    DBC_MESSAGE *msg = dbcHandler->findMessage(ui->cbMessages->currentText(), fqNode);
    if (!msg) return;

    QList<DBC_SIGNAL *> sigs = msg->sigHandler->getSignalsAsList();
    for (DBC_SIGNAL *sig : sigs)
    {
        if (sig)
            ui->cbSignals->addItem(sig->name);
    }
    ui->cbSignals->model()->sort(0);
}

void SearchWindow::copySignalToParamsUI()
{
    assocSignal = nullptr;
    if (!dbcHandler) return;

    QString fqNode  = ui->cbNodes->itemText(ui->cbNodes->currentIndex());
    QString msgName = ui->cbMessages->itemText(ui->cbMessages->currentIndex());
    DBC_MESSAGE *msg = dbcHandler->findMessage(msgName, fqNode);
    if (!msg) return;

    DBC_SIGNAL *sig = msg->sigHandler->findSignalByName(ui->cbSignals->currentText());
    if (!sig) return;

    // Populate manual fields from DBC signal
    startBit = sig->startBit;
    dataLen  = sig->signalSize;

    ui->txtStartBit->blockSignals(true);
    ui->txtStartBit->setText(QString::number(startBit));
    ui->txtStartBit->blockSignals(false);

    ui->txtDataLen->setText(QString::number(dataLen));
    ui->txtID->setText(Utility::formatCANID(msg->ID));
    ui->cbUseID->setChecked(true);
    ui->txtScale->setText(QString::number(sig->factor));
    ui->txtBias->setText(QString::number(sig->bias));
    ui->cbIntel->setChecked(sig->intelByteOrder);
    ui->cbSigned->setChecked(sig->valType == SIGNED_INT);

    assocSignal = sig;
    ui->lblSignalStatus->setText("Params loaded from: " + sig->name);

    drawBitfield();
}

// ──────────────────────────────────────────────
//  Bit-field visualisation (mirrors NewGraphDialog)
// ──────────────────────────────────────────────
void SearchWindow::bitfieldClicked(int bit)
{
    startBit = bit;
    ui->txtStartBit->blockSignals(true);
    ui->txtStartBit->setText(QString::number(startBit));
    ui->txtStartBit->blockSignals(false);
    drawBitfield();
}

void SearchWindow::handleStartBitUpdate()
{
    startBit = ui->txtStartBit->text().toInt();
    if (startBit < 0)   startBit = 0;
    if (startBit > 511) startBit = 511;
    // Manual edit overrides any loaded DBC signal
    if (assocSignal) {
        assocSignal = nullptr;
        ui->lblSignalStatus->setText("(manual – DBC signal cleared)");
    }
    drawBitfield();
}

void SearchWindow::handleDataLenUpdate()
{
    dataLen = ui->txtDataLen->text().toInt();
    if (dataLen < 1)  dataLen = 1;
    if (dataLen > 64) dataLen = 64; // web-savvycan: a value is at most 64 bits (Utility::processIntegerSignal)
    // Manual edit overrides any loaded DBC signal
    if (assocSignal) {
        assocSignal = nullptr;
        ui->lblSignalStatus->setText("(manual – DBC signal cleared)");
    }
    drawBitfield();
}

void SearchWindow::drawBitfield()
{
    uint8_t bitField[64];
    memset(bitField, 0, sizeof(bitField));

    // Mark the start bit distinctly
    bitField[Utility::getByteFromBitPosition(startBit)] |=
        static_cast<uint8_t>(1 << Utility::getBitFromBitPosition(startBit));

    ui->gridData->setReference(reinterpret_cast<unsigned char *>(bitField), false);

    if (ui->cbIntel->isChecked())
    {
        int endBit = startBit + dataLen - 1;
        if (endBit > 511) endBit = 511;
        for (int y = startBit; y <= endBit; y++)
            bitField[Utility::getByteFromBitPosition(y)] |=
                static_cast<uint8_t>(1 << Utility::getBitFromBitPosition(y));
    }
    else
    {
        int size = dataLen;
        int sBit = startBit;
        while (size > 0)
        {
            bitField[Utility::getByteFromBitPosition(sBit)] |=
                static_cast<uint8_t>(1 << Utility::getBitFromBitPosition(sBit));
            size--;
            if ((sBit % 8) == 0) sBit += 15;
            else sBit--;
            if (sBit > 511) sBit = 511;
        }
    }

    int highestByte = 0;
    for (int b = 0; b < 64; b++) if (bitField[b]) highestByte = b;
    fitGrid(highestByte);
    ui->gridData->updateData(reinterpret_cast<unsigned char *>(bitField), true);
}

// web-savvycan: as Add New Graph's grid: the frame length searched for, the typed ID's frames and the bits picked,
// whichever is longest; 8 bytes at least (SavvyCAN's grid), up to CAN FD's 64 (CANDataGrid's 8/16/32/64 layouts).
void SearchWindow::fitGrid(int highestByte)
{
    int bytes = qMax(8, qMax(frameLenHint, highestByte + 1));
    if (ui->cbUseLen->isChecked()) bytes = qMax(bytes, ui->spinFrameLen->value());
    bytes = bytes <= 8 ? 8 : bytes <= 16 ? 16 : bytes <= 32 ? 32 : 64;
    if (ui->gridData->getBytesToDraw() == bytes) return;
    ui->gridData->setBytesToDraw(bytes);
    const int cols = bytes > 32 ? 32 : bytes > 8 ? 16 : 8;
    ui->gridData->setMinimumWidth(qMax(ui->gridData->minimumWidth(), cols * 20 + 24));
    ui->gridData->update();
}

// The frame length of the ID typed in: its longest frame among the newest 100 000 in the trace.
void SearchWindow::handleIdUpdate()
{
    frameLenHint = 0;
    const QString text = ui->txtID->text().trimmed();
    if (!text.isEmpty() && modelFrames)
    {
        const uint32_t id = uint32_t(Utility::ParseStringToNum(text));
        for (const CommFrame &f : modelFrames->tail(100000))
            if (f.frameId() == id) frameLenHint = qMax(frameLenHint, int(qMin<qsizetype>(64, f.payload().size())));
    }
    drawBitfield();
}

// ──────────────────────────────────────────────
//  Value mode
// ──────────────────────────────────────────────
void SearchWindow::onValueModeChanged()
{
    bool hasValueFilter = !ui->rbValueNone->isChecked();
    ui->txtStartBit->setEnabled(hasValueFilter);
    ui->txtDataLen->setEnabled(hasValueFilter);
    ui->cbIntel->setEnabled(hasValueFilter);
    ui->cbSigned->setEnabled(hasValueFilter);
    ui->txtScale->setEnabled(hasValueFilter);
    ui->txtBias->setEnabled(hasValueFilter);
    ui->gridData->setEnabled(hasValueFilter);
    ui->txtValue->setEnabled(ui->rbValueSpecific->isChecked());
}

// ──────────────────────────────────────────────
//  Search
// ──────────────────────────────────────────────
void SearchWindow::onSearchClicked()
{
    performSearch();
}

// web-savvycan: the matching in FileTools::FrameSearch (signals anywhere in a 64-byte CAN FD frame, only frames that
// hold the whole signal, values as decimal, 0x hex or 0b binary), over the trace in slices taken under one lock each;
// the page keeps running on a big capture (FrameFileIO::keepUiAlive).
void SearchWindow::performSearch()
{
    QElapsedTimer timer;
    timer.start();
    matchingIndices.clear();
    currentMatchIndex = -1;

    if (!modelFrames || modelFrames->isEmpty())
    {
        ui->lblMatchInfo->setText("No frames loaded.");
        updateNavigationState();
        return;
    }

    FileTools::SearchCriteria c;
    c.useID = ui->cbUseID->isChecked();
    c.id = c.useID ? uint32_t(Utility::ParseStringToNum(ui->txtID->text())) : 0;
    c.useLen = ui->cbUseLen->isChecked();
    c.len = ui->spinFrameLen->value();
    if (ui->rbDirRX->isChecked()) c.direction = 1;
    if (ui->rbDirTX->isChecked()) c.direction = 2;
    c.valueMode = ui->rbValueNone->isChecked() ? FileTools::SearchCriteria::NoFilter
                : ui->rbValueSpecific->isChecked() ? FileTools::SearchCriteria::Specific
                : FileTools::SearchCriteria::AnyChange;
    c.startBit = startBit;
    c.numBits = dataLen;
    c.intel = ui->cbIntel->isChecked();
    c.isSigned = ui->cbSigned->isChecked();
    c.scale = ui->txtScale->text().toDouble();
    c.bias = ui->txtBias->text().toDouble();
    c.valueText = ui->txtValue->text();
    c.signal = assocSignal;
    FileTools::FrameSearch search(c);
    if (!search.valid())
    {
        ui->lblMatchInfo->setText("The value is not a number (decimal, 0x hex or 0b binary).");
        updateNavigationState();
        return;
    }

    constexpr qsizetype kSlice = 65536;
    const qsizetype count = modelFrames->size();
    firstAtSearch = modelFrames->first();
    for (qsizetype i = 0; i < count; i += kSlice)
    {
        const QVector<CommFrame> slice = modelFrames->mid(i, kSlice);
        for (qsizetype j = 0; j < slice.size(); j++)
            if (search.matches(slice[j])) matchingIndices.append(int(i + j));
        FrameFileIO::keepUiAlive();
    }
    searchMs = timer.nsecsElapsed() / 1e6;

    if (matchingIndices.isEmpty())
    {
        ui->lblMatchInfo->setText("No matches found.");
    }
    else
    {
        currentMatchIndex = 0;
        ui->lblMatchInfo->setText(
            QString("Match 1 of %1").arg(matchingIndices.size()));
        emit jumpToFrameIndex(matchingIndices.at(0));
    }

    updateNavigationState();
}

// ──────────────────────────────────────────────
//  Navigation
// ──────────────────────────────────────────────
void SearchWindow::onNextClicked()
{
    if (matchingIndices.isEmpty()) return;
    currentMatchIndex = (currentMatchIndex + 1) % matchingIndices.size();
    ui->lblMatchInfo->setText(
        QString("Match %1 of %2").arg(currentMatchIndex + 1).arg(matchingIndices.size()));
    emit jumpToFrameIndex(matchingIndices.at(currentMatchIndex));
}

void SearchWindow::onPreviousClicked()
{
    if (matchingIndices.isEmpty()) return;
    currentMatchIndex = (currentMatchIndex - 1 + matchingIndices.size()) % matchingIndices.size();
    ui->lblMatchInfo->setText(
        QString("Match %1 of %2").arg(currentMatchIndex + 1).arg(matchingIndices.size()));
    emit jumpToFrameIndex(matchingIndices.at(currentMatchIndex));
}

void SearchWindow::updateNavigationState()
{
    bool hasResults = !matchingIndices.isEmpty();
    ui->btnNext->setEnabled(hasResults);
    ui->btnPrev->setEnabled(hasResults);
}
