#include "filecomparatorwindow.h"
#include "ui_filecomparatorwindow.h"
#include "helpwindow.h"
#include "framestore.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QSettings>
#include <qevent.h>
#include <limits>

FileComparatorWindow::FileComparatorWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::FileComparatorWindow)
{
    ui->setupUi(this);
#ifdef Q_OS_WASM
    // web-savvycan: as the connection and graphing windows: a Qt::Window is frameless and covers the main window's
    // menus in the browser; as a dialog it has Qt's title bar and close button and can be moved aside.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
#else
    setWindowFlags(Qt::Window);
#endif

    connect(ui->btnInterestedFile, SIGNAL(clicked(bool)), this, SLOT(loadInterestedFile()));
    connect(ui->btnLoadRefFile, SIGNAL(clicked(bool)), this, SLOT(loadReferenceFile()));
    connect(ui->btnSaveDetails, SIGNAL(clicked(bool)), this, SLOT(saveDetails()));
    connect(ui->btnClear, SIGNAL(clicked(bool)), this, SLOT(clearReference()));

    ui->lblFirstFile->setText("");
    ui->lblRefFrames->setText("Loaded frames: 0");

    dbcHandler = DBCHandler::getReference();

    installEventFilter(this);
}

FileComparatorWindow::~FileComparatorWindow()
{
    removeEventFilter(this);
    delete ui;
}

void FileComparatorWindow::showEvent(QShowEvent *)
{
    readSettings();
}

void FileComparatorWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event)
    writeSettings();
}

bool FileComparatorWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key())
        {
        case Qt::Key_F1:
            HelpWindow::getRef()->showHelp("filecomparison.md");
            break;
        }
        return true;
    } else {
        // standard event processing
        return QObject::eventFilter(obj, event);
    }
    return false;
}

void FileComparatorWindow::readSettings()
{
    QSettings settings;
    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        resize(settings.value("FileComparator/WindowSize", QSize(720, 631)).toSize());
        move(Utility::constrainedWindowPos(settings.value("FileComparator/WindowPos", QPoint(50, 50)).toPoint()));
    }
}

void FileComparatorWindow::writeSettings()
{
    QSettings settings;

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("FileComparator/WindowSize", size());
        settings.setValue("FileComparator/WindowPos", pos());
    }
}

// web-savvycan: a file's frames summarised into a side (FileTools::CompareSide) as they are read, then dropped. In the
// browser the bytes come from the browser's file picker (Qt's dialog would only see the page's empty in-memory file
// system), with File > Load Log File's type detection and limits.
bool FileComparatorWindow::summarise(const QString &name, const QByteArray &content, FileTools::CompareSide *side)
{
    constexpr qint64 kMaxFileBytes = 1000LL * 1000 * 1000;     // SavvyCAN's GVRET CSV (MainWindow::loadBrowserFile)
    constexpr qint64 kMaxOtherFileBytes = 200LL * 1000 * 1000; // the other formats
    const QString shown = QFileInfo(name).fileName();
    const bool csv = !content.startsWith("LOGG") && FrameFileIO::isNativeCSVData(content);
    if (content.size() > (csv ? kMaxFileBytes : kMaxOtherFileBytes)) {
        QMessageBox::warning(this, tr("File too large"),
                             tr("%1 is %2 MB. The browser version loads SavvyCAN's GVRET CSV logs up to %3 MB and other "
                                "formats up to %4 MB.").arg(shown).arg(content.size() / 1000000)
                                 .arg(kMaxFileBytes / 1000000).arg(kMaxOtherFileBytes / 1000000));
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    QProgressDialog progress(this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setLabelText(tr("Loading %1...").arg(shown));
    progress.setCancelButton(nullptr);
    progress.setRange(0, 1000);
    progress.setMinimumDuration(1000);
    progress.setValue(0);
    FrameFileIO::progressHook = [&progress](qint64 done, qint64 total) {
        progress.setValue(int(qMin<qint64>(900, done * 900 / total)));
    };
    FrameStore frames(std::numeric_limits<qsizetype>::max(), FrameStore::DefaultChunkFrames);
    const bool ok = FrameFileIO::loadLogData(name, content, &frames);
    FrameFileIO::progressHook = nullptr;
    if (!ok) {
        progress.cancel();
        QMessageBox::warning(this, tr("Error Loading"), tr("%1 could not be read.").arg(shown));
        return false;
    }
    // summarised a chunk at a time, each chunk freed as it is done
    const qsizetype total = frames.size();
    qsizetype done = 0;
    while (!frames.isEmpty()) {
        const quint64 end = frames.frontChunkEndSeq();
        for (quint64 seq = frames.firstSeq(); seq < end; seq++) side->add(frames.bySeq(seq), dbcHandler);
        done += qsizetype(end - frames.firstSeq());
        frames.dropFront();
        progress.setValue(900 + int(done * 99 / qMax<qsizetype>(1, total)));
        FrameFileIO::keepUiAlive();
    }
    progress.cancel();
    loadMs = timer.nsecsElapsed() / 1e6;
    return true;
}

void FileComparatorWindow::loadInterestedFile()
{
#ifdef Q_OS_WASM
    QPointer<FileComparatorWindow> self(this);
    QFileDialog::getOpenFileContent(QString(), [self](const QString &name, const QByteArray &content) {
        if (!self || name.isEmpty()) return; // empty: the picker was cancelled
        self->interested.clear();
        if (self->summarise(name, content, &self->interested)) self->interestedLoaded(QFileInfo(name).fileName());
    });
#else
    QString resultingFileName;
    QVector<CommFrame> frames;
    qApp->processEvents();
    if (FrameFileIO::loadFrameFile(resultingFileName, &frames))
    {
        interested.clear();
        for (const CommFrame &f : std::as_const(frames)) interested.add(f, dbcHandler);
        frames = QVector<CommFrame>();
        interestedLoaded(resultingFileName);
    }
#endif
}

void FileComparatorWindow::interestedLoaded(const QString &name)
{
    ui->lblFirstFile->setText(name);
    interestedFilename = name;
    if (interested.frames > 0 && reference.frames > 0) calculateDetails();
}

void FileComparatorWindow::loadReferenceFile()
{
    // Side 2 takes one or more files (each one adds to it, until Clear Frames)
#ifdef Q_OS_WASM
    QPointer<FileComparatorWindow> self(this);
    QFileDialog::getOpenFileContent(QString(), [self](const QString &name, const QByteArray &content) {
        if (!self || name.isEmpty()) return;
        if (self->summarise(name, content, &self->reference)) self->referenceLoaded();
    });
#else
    QString resultingFileName;
    QVector<CommFrame> frames;
    qApp->processEvents();
    if (FrameFileIO::loadFrameFile(resultingFileName, &frames))
    {
        for (const CommFrame &f : std::as_const(frames)) reference.add(f, dbcHandler);
        referenceLoaded();
    }
#endif
}

void FileComparatorWindow::referenceLoaded()
{
    ui->lblRefFrames->setText("Loaded frames: " + QString::number(reference.frames));
    if (interested.frames > 0 && reference.frames > 0) calculateDetails();
}

void FileComparatorWindow::clearReference()
{
    reference.clear();
    ui->treeDetails->clear();
    ui->lblRefFrames->setText("Loaded frames: " + QString::number(reference.frames));
}

// web-savvycan: the tree from the two summaries (FileTools::buildCompareTree; CAN FD bytes 8..63 included, no tree
// items made that are not shown)
void FileComparatorWindow::calculateDetails()
{
    QElapsedTimer timer;
    timer.start();
    ui->treeDetails->clear();
    ui->treeDetails->addTopLevelItems(FileTools::buildCompareTree(interested, reference, interestedFilename,
                                                                  ui->ckUniqueToInterested->isChecked(), dbcHandler));
    QSettings settings;
    if (settings.value("InfoCompare/AutoExpand", false).toBool())
    {
        ui->treeDetails->expandAll();
    }
    compareMs = timer.nsecsElapsed() / 1e6;
}

void FileComparatorWindow::saveDetails()
{
#ifdef Q_OS_WASM
    // web-savvycan: in the browser the text goes to the browser as a download (FileComparison.txt)
    {
        QByteArray out;
        for (QTreeWidgetItemIterator it(ui->treeDetails); *it; ++it) {
            QTreeWidgetItem *item = *it;
            const QString itemText = item->text(0);
            for (; item->parent(); item = item->parent()) out += "   ";
            out += itemText.toUtf8() + "\n";
        }
        QFileDialog::saveFileContent(out, QStringLiteral("FileComparison.txt"));
        return;
    }
#endif
    QString filename;
    QFileDialog dialog(this);
    QSettings settings;

    QStringList filters;
    filters.append(QString(tr("Text File (*.txt)")));

    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDirectory(settings.value("FileComparator/LoadSaveDirectory", dialog.directory().path()).toString());

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];
        settings.setValue("FileComparator/LoadSaveDirectory", dialog.directory().path());
        if (!filename.contains('.')) filename += ".txt";
        QFile *outFile = new QFile(filename);

        if (!outFile->open(QIODevice::WriteOnly | QIODevice::Text))
            return;

        QTreeWidget *tree = ui->treeDetails;


        QTreeWidgetItemIterator it(tree);
        while (*it) {
          QTreeWidgetItem *item = *it;
          QString itemText = item->text(0);
          while (item->parent())
          {
              outFile->write("   ");
              item = item->parent();
          }
          outFile->write(itemText.toUtf8() + "\n");
          ++it;
        }

        outFile->close();
        delete outFile;

    }
}

