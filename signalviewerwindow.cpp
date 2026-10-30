#include "signalviewerwindow.h"
#include "ui_signalviewerwindow.h"
#include "helpwindow.h"
#include "mainwindow.h"
#include "utility.h"
#include <QDebug>

#define MSG_COL     1
#define VALUE_COL   2

SignalViewerWindow::SignalViewerWindow(const FrameListView *frames, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SignalViewerWindow)
{
    ui->setupUi(this);
#ifdef Q_OS_WASM
    // web-savvycan: as the graphing window: framed in the browser (a Qt::Window is frameless there)
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
#else
    setWindowFlags(Qt::Window);
#endif

    modelFrames = frames;

    QStringList headers;
    headers << "Node" << "Signal" << "Value";
    ui->tableViewer->setHorizontalHeaderLabels(headers);
    ui->tableViewer->setColumnWidth(0, 100);
    ui->tableViewer->setColumnWidth(1, 150);

    QSettings settings;
    QFont sysFont;
    int fontSize = settings.value("Main/FontSize", 9).toUInt();
    if(settings.value("Main/FontFixedWidth", false).toBool())
        sysFont = QFontDatabase::systemFont(QFontDatabase::FixedFont); //get default fixed width font
    else
        sysFont = QFont();  //get default font
    sysFont.setPointSize(fontSize);
    ui->tableViewer->setFont(sysFont);

    QHeaderView *HorzHdr = ui->tableViewer->horizontalHeader();
    HorzHdr->setStretchLastSection(true); //causes the data column to automatically fill the tableview
    HorzHdr->setFont(QFont());

    QHeaderView *verticalHeader = ui->tableViewer->verticalHeader();
    verticalHeader->setFont(QFont());

    dbcHandler = DBCHandler::getReference();
    currentlySelectedMsg = nullptr;

    connect(ui->cbNodes, SIGNAL(currentIndexChanged(int)), this, SLOT(loadMessages(int)));
    connect(ui->cbMessages, SIGNAL(currentIndexChanged(int)), this, SLOT(loadSignals(int)));
    connect(ui->btnAdd, SIGNAL(clicked(bool)), this, SLOT(addSignal()));
    connect(MainWindow::getReference(), SIGNAL(framesUpdated(int)), this, SLOT(updatedFrames(int)));
    connect(ui->btnRemove, SIGNAL(clicked(bool)), this, SLOT(removeSelectedSignal()));
    connect(ui->btnSave, SIGNAL(clicked(bool)), this, SLOT(saveSignalsFile()));
    connect(ui->btnLoad, SIGNAL(clicked(bool)), this, SLOT(loadSignalsFile()));
    connect(ui->btnAppend, SIGNAL(clicked(bool)), this, SLOT(appendSignalsFile()));
    connect(ui->btnClear, SIGNAL(clicked(bool)), this, SLOT(clearSignalsTable()));
    connect(dbcHandler, &DBCHandler::signalsRemoved, this, &SignalViewerWindow::dropRemovedSignals);
    // web-savvycan: a DBC loaded while the window exists is listed (SavvyCAN listed the files of when it was made)
    connect(dbcHandler, &DBCHandler::filesChanged, this, &SignalViewerWindow::loadNodes);

    loadNodes();
}

SignalViewerWindow::~SignalViewerWindow()
{
    delete ui;
}

// web-savvycan: each signal shows its newest value, so the new frames are gone through from the newest back and the
// search stops once every signal has its value (SavvyCAN went forward over them by index, which skipped every update
// once the trace's ring was full, and showed nothing after a file was loaded (-1)). -1 / -2 (cleared, a file loaded,
// a new set): the values are cleared and the whole trace searched the same way.
void SignalViewerWindow::updatedFrames(int numFrames)
{
    if (signalList.isEmpty()) return;
    qsizetype count = modelFrames->count();
    if (numFrames < 0)
    {
        for (int i = 0; i < ui->tableViewer->rowCount(); i++)
            if (QTableWidgetItem *item = ui->tableViewer->item(i, VALUE_COL)) item->setText(QString());
        count = qMin<qsizetype>(count, 1000000); // a signal whose message isn't in the trace: the newest million frames searched
    }
    else count = qMin<qsizetype>(count, numFrames);

    QList<bool> done(signalList.count(), false);
    int left = signalList.count();
    CommFrame thisFrame;
    const qsizetype total = modelFrames->count();
    for (qsizetype i = total - 1; i >= total - count && left > 0; i--)
    {
        thisFrame = modelFrames->at(i);
        left -= processFrame(thisFrame, done);
    }
}

// The values of the signals not yet done that this frame carries; returns how many it set.
int SignalViewerWindow::processFrame(const CommFrame &frame, QList<bool> &done)
{
    QString sigString;
    int set = 0;
    for (int i = 0; i < signalList.count(); i++)
    {
        if (done[i]) continue;
        DBC_SIGNAL *sig = signalList.at(i);
        if (!sig) continue;
        if (sig->parentMessage->ID == frame.frameId())
        {
            if (sig->isSignalInMessage(frame)) //filter out multiplexed signals that aren't in this message.
            {
                if (sig->processAsText(frame, sigString, false)) //if true we could interpret the signal so update it in the list
                {
                    QTableWidgetItem *item = ui->tableViewer->item(i, VALUE_COL);
                    if (!item)
                    {
                        item = new QTableWidgetItem(sigString);
                        ui->tableViewer->setItem(i, VALUE_COL, item);
                    }
                    else item->setText(sigString);
                    done[i] = true;
                    set++;
                }
            }
        }
    }
    return set;
}

// A DBC signal, message or file was removed: its rows go (their pointers would point at freed memory).
void SignalViewerWindow::dropRemovedSignals()
{
    for (int i = signalList.count() - 1; i >= 0; i--)
    {
        if (dbcHandler->isLiveSignal(signalList.at(i))) continue;
        signalList.removeAt(i);
        ui->tableViewer->removeRow(i);
    }
    loadNodes();
}

void SignalViewerWindow::removeSelectedSignal()
{
    int selRow = ui->tableViewer->currentRow();
    if (selRow < 0) return; //no selected row
    signalList.removeAt(selRow);
    ui->tableViewer->removeRow(selRow);
}

void SignalViewerWindow::loadNodes()
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
        QList<DBC_MESSAGE *> msgs = thisFile->messageHandler->getMsgsAsList();

        QList<QString> names;

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

void SignalViewerWindow::loadMessages(int idx)
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

        QList<DBC_MESSAGE *> msgs = dbcHandler->getFileByIdx(f)->messageHandler->getMsgsAsList();
        for (int x = 0; x < msgs.length(); x++)
        {
            QString fullyQualifiedNodeName = dbcHandler->getFileByIdx(f)->getFilenameNoExt() + Utility::fullyQualifiedNameSeperator + msgs[x]->sender->name;
            if(fullyQualifiedNodeName == displayedNodeName)
                ui->cbMessages->addItem(msgs[x]->name);
        }
    }
}

void SignalViewerWindow::loadSignals(int idx)
{
    Q_UNUSED(idx);
    //messages were placed into the list in the same order as they exist
    //in the data structure so it should have been possible to just
    //look it up based on index but by name is probably safer and this operation
    //is not time critical at all.

    QString fullyQualifiedNodeName = ui->cbNodes->itemText(ui->cbNodes->currentIndex());
    QString msgName = ui->cbMessages->itemText(ui->cbMessages->currentIndex());

    DBC_MESSAGE *msg = dbcHandler->findMessage(msgName, fullyQualifiedNodeName);

    if (msg == nullptr) return;
    ui->cbSignals->clear();

    QList<DBC_SIGNAL *> sigs = msg->sigHandler->getSignalsAsList();
    for (int x = 0; x < msg->sigHandler->getCount(); x++)
    {
        ui->cbSignals->addItem(sigs[x]->name);
    }
}

void SignalViewerWindow::addSignal()
{
    DBC_MESSAGE *msg = nullptr;

    QString fullyQualifiedNodeName = ui->cbNodes->itemText(ui->cbNodes->currentIndex());
    QString msgName = ui->cbMessages->itemText(ui->cbMessages->currentIndex());

    msg = dbcHandler->findMessage(msgName, fullyQualifiedNodeName);

    if (!msg) return;
    DBC_SIGNAL *sig = msg->sigHandler->findSignalByName(ui->cbSignals->currentText());
    if (!sig) return;

    addSignal(sig);
}

void SignalViewerWindow::addSignal(DBC_SIGNAL *sig)
{
    signalList.append(sig);

    int rowIdx = ui->tableViewer->rowCount();
    ui->tableViewer->insertRow(rowIdx);
    QTableWidgetItem *nodeitem = new QTableWidgetItem(sig->parentMessage->sender->name);
    ui->tableViewer->setItem(rowIdx, 0, nodeitem);
    QTableWidgetItem *msgitem = new QTableWidgetItem(sig->name);
    ui->tableViewer->setItem(rowIdx, 1, msgitem);
    updatedFrames(-2); // web-savvycan: its newest value from the trace at once (SavvyCAN waited for the next frame)
}

void SignalViewerWindow::saveSignalsFile()
{
    saveDefinitions();
}

void SignalViewerWindow::loadSignalsFile()
{
    loadDefinitions(false);
}

void SignalViewerWindow::appendSignalsFile()
{
    loadDefinitions(true);
}

void SignalViewerWindow::clearSignalsTable()
{
    clearSignalsTable(true);
}

void SignalViewerWindow::clearSignalsTable(bool askForConfirmation)
{
    if(askForConfirmation)
    {
        QMessageBox::StandardButton confirmDialog;
        confirmDialog = QMessageBox::question(this, "Danger Will Robinson", "Are you sure you want to clear all of your signals?",
                                      QMessageBox::Yes|QMessageBox::No);
        if (confirmDialog == QMessageBox::No)
        {
            return;
        }
    }

    signalList.clear();
    ui->tableViewer->setRowCount(0);
}

// The view as SavvyCAN's signal viewer definition (SV1 lines)
QByteArray SignalViewerWindow::definitionsText() const
{
    QByteArray out;
    for (DBC_SIGNAL *sig : signalList)
    {
        out += "SV1," + QByteArray::number(sig->parentMessage->ID, 16) + ',' + sig->parentMessage->name.toUtf8() + ','
               + sig->name.toUtf8() + '\n';
    }
    return out;
}

void SignalViewerWindow::saveDefinitions()
{
#ifdef Q_OS_WASM
    // web-savvycan: in the browser a download (or Chrome's save picker)
    QFileDialog::saveFileContent(definitionsText(), QStringLiteral("SignalView.sdf"));
    return;
#endif
    QString filename;
    QFileDialog dialog(this);
    QSettings settings;

    QStringList filters;
    filters.append(QString(tr("SignalViewer definition (*.sdf)")));

    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDirectory(settings.value("SignalViewer/LoadSaveDirectory", dialog.directory().path()).toString());

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];
        settings.setValue("SignalViewer/LoadSaveDirectory", dialog.directory().path());

        if (!filename.contains('.')) filename += ".sdf";

        QFile *outFile = new QFile(filename);

        if (!outFile->open(QIODevice::WriteOnly | QIODevice::Text))
            return;

        outFile->write(definitionsText());
        outFile->close();
        delete outFile;
    }
}

void SignalViewerWindow::loadDefinitions(bool append)
{
    QString filename;
    QFileDialog dialog;
    QSettings settings;

    QStringList filters;
    filters.append(QString(tr("SignalViewer definition (*.sdf)")));

    if (dbcHandler == nullptr) return;
    if (dbcHandler->getFileCount() == 0) dbcHandler->createBlankFile();
#ifdef Q_OS_WASM
    // web-savvycan: in the browser the file comes through the browser's file picker
    QFileDialog::getOpenFileContent(tr("SignalViewer definition (*.sdf)"), [this, append](const QString &name, const QByteArray &content) {
        if (!name.isEmpty()) applyDefinitions(content, append);
    });
    return;
#endif

    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);
    dialog.setDirectory(settings.value("SignalViewer/LoadSaveDirectory", dialog.directory().path()).toString());

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];
        settings.setValue("SignalViewer/LoadSaveDirectory", dialog.directory().path());

        QFile inFile(filename);
        if (!inFile.open(QIODevice::ReadOnly | QIODevice::Text))
            return;
        applyDefinitions(inFile.readAll(), append);
    }
}

// SV1 lines: each signal found by its message ID (else its message name) and name joins the view (or replaces it)
void SignalViewerWindow::applyDefinitions(const QByteArray &text, bool append)
{
    QList<DBC_SIGNAL *> loadedSignals;
    for (const QByteArray &raw : text.split('\n'))
    {
        const QByteArray line = raw.simplified();
        if (line.length() <= 2) continue;
        QList<QByteArray> tokens = line.split(',');
        if (tokens[0] != "SV1" || tokens.count() < 4) continue; //signal viewer save format v1
        uint32_t msgId = tokens[1].toUInt(nullptr, 16);
        QString msgName = QString(tokens[2]);
        QString sigName = QString(tokens[3]);
        DBC_MESSAGE *msg = dbcHandler->findMessage(msgId);
        if (!msg) msg = dbcHandler->findMessage(msgName); //this is not a very safe way to match since messages names can be duplicated
        DBC_SIGNAL *sig = msg ? msg->sigHandler->findSignalByName(sigName) : nullptr;
        if (sig) loadedSignals.append(sig);
        else qDebug() << "Couldn't find the message by ID or name! " << msgName << "  " << sigName;
    }
    if (loadedSignals.count() > 0)
    {
        if (append == false) clearSignalsTable(false);
        for (DBC_SIGNAL *sig : std::as_const(loadedSignals)) addSignal(sig);
    }
}
