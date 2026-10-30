#include "bisectwindow.h"
#include "ui_bisectwindow.h"

#include "mainwindow.h"
#include "framefileio.h"
#include "helpwindow.h"

#include "re/filetools.h"
#include "connections/canconmanager.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QProgressDialog>
#include <algorithm>

BisectWindow::BisectWindow(const FrameListView *frames, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::BisectWindow)
{
    ui->setupUi(this);
#ifdef Q_OS_WASM
    // web-savvycan: as the connection and graphing windows: a Qt::Window is frameless and covers the main window's
    // menus in the browser; as a dialog it has Qt's title bar and close button and can be moved aside.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
#else
    setWindowFlags(Qt::Window);
#endif

    modelFrames = frames;

    connect(MainWindow::getReference(), SIGNAL(framesUpdated(int)), this, SLOT(updatedFrames(int)));
    connect(ui->btnCalculate, &QAbstractButton::clicked, this, &BisectWindow::handleCalculateButton);
    connect(ui->btnReplaceFrames, &QAbstractButton::clicked, this, &BisectWindow::handleReplaceButton);
    connect(ui->btnSaveFrames, &QAbstractButton::clicked, this, &BisectWindow::handleSaveButton);
    connect(ui->slideFrameNumber, &QSlider::sliderReleased, this, &BisectWindow::updateFrameNumText);
    connect(ui->slidePercentage, &QSlider::sliderReleased, this, &BisectWindow::updatePercentText);
    connect(ui->editFrameNumber, &QLineEdit::editingFinished, this, &BisectWindow::updateFrameNumSlider);
    connect(ui->editPercentage, &QLineEdit::editingFinished, this, &BisectWindow::updatePercentSlider);
    connect(ui->rbFrameNumber, &QRadioButton::toggled, this, &BisectWindow::updateSectionsText);
    connect(ui->rbPercentage, &QRadioButton::toggled, this, &BisectWindow::updateSectionsText);
    connect(ui->rbBusNum, &QRadioButton::toggled, this, &BisectWindow::updateSectionsText);
    connect(ui->rbIDRange, &QRadioButton::toggled, this, &BisectWindow::updateSectionsText);

    installEventFilter(this);

    updateSectionsText();
}

BisectWindow::~BisectWindow()
{
    removeEventFilter(this);
    delete ui;
}

void BisectWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    refreshIDList();
    refreshFrameNumbers();
    updateFrameNumText();
    updatePercentText();
}

bool BisectWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key())
        {
        case Qt::Key_F1:
            HelpWindow::getRef()->showHelp("bisector.md");
            break;
        }
        return true;
    } else {
        // standard event processing
        return QObject::eventFilter(obj, event);
    }
    return false;
}

void BisectWindow::updateSectionsText()
{
    if (ui->rbBusNum->isChecked())
    {
        ui->rbLowerSection->setText("Only this bus");
        ui->rbUpperSection->setText("Not this bus");
    }
    if (ui->rbFrameNumber->isChecked())
    {
        ui->rbLowerSection->setText("Up to this frame number");
        ui->rbUpperSection->setText("After this frame number");
    }
    if (ui->rbIDRange->isChecked())
    {
        ui->rbLowerSection->setText("Inside the ID range");
        ui->rbUpperSection->setText("Outside the ID range");
    }
    if (ui->rbPercentage->isChecked())
    {
        ui->rbLowerSection->setText("Up to this percentage into the file");
        ui->rbUpperSection->setText("After this percentage into the file");
    }

}

// web-savvycan: one pass over the trace into a set (SavvyCAN looked each frame's ID up in a list: frames x IDs), in
// slices taken under one lock each; the IDs picked stay picked.
void BisectWindow::refreshIDList()
{
    foundID.clear();
    constexpr qsizetype kSlice = 65536;
    const qsizetype count = modelFrames->count();
    for (qsizetype i = 0; i < count; i += kSlice)
    {
        for (const CommFrame &f : modelFrames->mid(i, kSlice)) foundID.insert(f.frameId());
        FrameFileIO::keepUiAlive();
    }
    idListStale = false;
    addIDs({});
}

// New IDs into both lists, sorted, the selections kept.
void BisectWindow::addIDs(const QVector<CommFrame> &frames)
{
    bool added = false;
    for (const CommFrame &f : frames)
        if (!foundID.contains(f.frameId())) { foundID.insert(f.frameId()); added = true; }
    if (!added && ui->cbIDLower->count() == foundID.size()) return;
    QList<uint32_t> ids(foundID.begin(), foundID.end());
    std::sort(ids.begin(), ids.end());
    const QString lower = ui->cbIDLower->currentText(), upper = ui->cbIDUpper->currentText();
    ui->cbIDLower->clear();
    ui->cbIDUpper->clear();
    for (uint32_t id : ids) {
        ui->cbIDLower->addItem(Utility::formatCANID(id));
        ui->cbIDUpper->addItem(Utility::formatCANID(id));
    }
    if (int i = ui->cbIDLower->findText(lower); i >= 0) ui->cbIDLower->setCurrentIndex(i);
    if (int i = ui->cbIDUpper->findText(upper); i >= 0) ui->cbIDUpper->setCurrentIndex(i);
}

void BisectWindow::refreshFrameNumbers()
{
    ui->labelMainListNum->setText(QString::number(modelFrames->count()));
    ui->labelSplitNum->setText(QString::number(splitFrames.size()));
    ui->slideFrameNumber->setMaximum(int(qMin<qsizetype>(modelFrames->count(), std::numeric_limits<int>::max())));
}

void BisectWindow::updatedFrames(int numFrames)
{
    // web-savvycan: -1 is also a loaded file here (MainWindow), so both refresh the IDs; new frames update the count
    // and add their new IDs (SavvyCAN did neither), only while the window is shown (it recounts when shown again)
    if (numFrames == -1 || numFrames == -2)
    {
        idListStale = true;
        refreshFrameNumbers();
        if (isVisible()) refreshIDList();
    }
    else if (numFrames > 0 && isVisible())
    {
        refreshFrameNumbers();
        if (!idListStale) addIDs(modelFrames->tail(numFrames));
    }
}

// web-savvycan: one pass in slices (each slice taken under one lock), into the compact store; the page keeps running
// meanwhile (FrameFileIO::keepUiAlive). SavvyCAN's rules (FileTools::BisectCriteria).
void BisectWindow::handleCalculateButton()
{
    QElapsedTimer timer;
    timer.start();
    splitFrames.clear();
    FileTools::BisectCriteria c;
    c.lower = ui->rbLowerSection->isChecked();
    const qsizetype count = modelFrames->count();
    if (ui->rbFrameNumber->isChecked()) {
        c.mode = FileTools::BisectCriteria::FrameNumber;
        c.targetFrame = ui->slideFrameNumber->value();
    } else if (ui->rbPercentage->isChecked()) {
        c.mode = FileTools::BisectCriteria::Percentage;
        c.targetFrame = FileTools::BisectCriteria::splitRow(count, ui->slidePercentage->value());
    } else if (ui->rbIDRange->isChecked()) {
        c.mode = FileTools::BisectCriteria::IDRange;
        c.lowerID = Utility::ParseStringToNum2(ui->cbIDLower->currentText());
        c.upperID = Utility::ParseStringToNum2(ui->cbIDUpper->currentText());
    } else if (ui->rbBusNum->isChecked()) {
        c.mode = FileTools::BisectCriteria::Bus;
        c.bus = Utility::ParseStringToNum(ui->editBusNum->text());
    } else {
        refreshFrameNumbers();
        return;
    }
    qsizetype from = 0, to = count;   // the rows that can be kept at all
    if (c.mode == FileTools::BisectCriteria::FrameNumber || c.mode == FileTools::BisectCriteria::Percentage) {
        c.targetFrame = qBound<qsizetype>(0, c.targetFrame, count);
        if (c.lower) to = c.targetFrame; else from = c.targetFrame;
    }
    constexpr qsizetype kSlice = 65536;
    for (qsizetype i = from; i < to; i += kSlice)
    {
        const QVector<CommFrame> slice = modelFrames->mid(i, qMin(kSlice, to - i));
        for (qsizetype j = 0; j < slice.size(); j++)
            if (c.keep(slice[j], i + j)) splitFrames.append(slice[j]);
        FrameFileIO::keepUiAlive();
    }
    calculateMs = timer.nsecsElapsed() / 1e6;
    refreshFrameNumbers();
}

void BisectWindow::handleReplaceButton()
{
    // web-savvycan: the split frames move into the trace (the store is empty after), and every window hears of the new
    // set (SavvyCAN told none: W2), as Crop Log does
    CommFrameModel *model = MainWindow::getReference()->getCommFrameModel();
    model->clearFrames();
    model->insertFrames(splitFrames);
    CANConManager::getInstance()->resetTimeBasis();
    emit MainWindow::getReference()->framesUpdated(-2);
    refreshFrameNumbers();
    refreshIDList();
}

void BisectWindow::handleSaveButton()
{
#ifdef Q_OS_WASM
    // web-savvycan: in the browser the split frames go to the browser as a download, in SavvyCAN's GVRET CSV (what
    // File > Load Log File and SavvyCAN read; Qt's save dialog would only see the page's in-memory file system)
    {
        QByteArray out = FrameFileIO::nativeCSVHeader();
        for (qsizetype i = 0; i < splitFrames.size(); i++) {
            out += FrameFileIO::nativeCSVRow(splitFrames.atIndex(i));
            if ((i & 0xFFFF) == 0) FrameFileIO::keepUiAlive();
        }
        QFileDialog::saveFileContent(out, QStringLiteral("BisectedFrames.csv"));
        return;
    }
#endif
    QMessageBox msg;
    QString filename;
    QVector<CommFrame> frames;
    frames.reserve(splitFrames.size());
    for (qsizetype i = 0; i < splitFrames.size(); i++) frames.append(splitFrames.atIndex(i));
    if (FrameFileIO::saveFrameFile(filename, &frames))
    {
        msg.setText(tr("Successfully saved file"));
    }
    else
    {
        msg.setText(tr("Error while attempting to save."));
    }
    msg.exec();
}

void BisectWindow::updateFrameNumSlider()
{
    ui->slideFrameNumber->setValue(ui->editFrameNumber->text().toInt());
}

void BisectWindow::updatePercentSlider()
{
    ui->slidePercentage->setValue(ui->editPercentage->text().toFloat() * 100);
}

void BisectWindow::updateFrameNumText()
{
    ui->editFrameNumber->setText(QString::number(ui->slideFrameNumber->value()));
}

void BisectWindow::updatePercentText()
{
    ui->editPercentage->setText(QString::number(ui->slidePercentage->value() / 100.0f));
}
