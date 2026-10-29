#include "dbccomparatorwindow.h"
#include "ui_dbccomparatorwindow.h"
#include "helpwindow.h"
#include "re/filetools.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QSettings>
#include <qevent.h>

DBCComparatorWindow::DBCComparatorWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DBCComparatorWindow)
{
    ui->setupUi(this);
#ifdef Q_OS_WASM
    // web-savvycan: as the connection and graphing windows: a Qt::Window is frameless and covers the main window's
    // menus in the browser; as a dialog it has Qt's title bar and close button and can be moved aside.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
#else
    setWindowFlags(Qt::Window);
#endif

    connect(ui->btnDBCFile1, SIGNAL(clicked(bool)), this, SLOT(loadFirstFile()));
    connect(ui->btnDBCFile2, SIGNAL(clicked(bool)), this, SLOT(loadSecondFile()));
    connect(ui->btnSaveDetails, SIGNAL(clicked(bool)), this, SLOT(saveDetails()));

    ui->lblFirstFile->setText("");
    ui->lblSecondFile->setText("");

    firstDBC = nullptr;
    secondDBC = nullptr;

    installEventFilter(this);
}

DBCComparatorWindow::~DBCComparatorWindow()
{
    removeEventFilter(this);
    delete firstDBC;
    delete secondDBC;
    delete ui;
}

void DBCComparatorWindow::showEvent(QShowEvent *)
{
    readSettings();
}

void DBCComparatorWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event)
    writeSettings();
}

bool DBCComparatorWindow::eventFilter(QObject *obj, QEvent *event)
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

void DBCComparatorWindow::readSettings()
{
    QSettings settings;
    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        resize(settings.value("DBCComparator/WindowSize", QSize(720, 631)).toSize());
        move(Utility::constrainedWindowPos(settings.value("DBCComparator/WindowPos", QPoint(50, 50)).toPoint()));
    }
}

void DBCComparatorWindow::writeSettings()
{
    QSettings settings;

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("DBCComparator/WindowSize", size());
        settings.setValue("DBCComparator/WindowPos", pos());
    }
}

#ifdef Q_OS_WASM
// web-savvycan: in the browser the DBC file comes from the browser's file picker (Qt's dialog would only see the page's
// empty in-memory file system). SavvyCAN's DBC loader reads by file name: the bytes go to a temporary file in the page's
// file system, are loaded into the comparator's own DBCFile (not the DBC File Manager's) and the file is deleted.
void DBCComparatorWindow::pickDBC(DBCFile **slot, QLabel *label)
{
    QPointer<DBCComparatorWindow> self(this);
    QFileDialog::getOpenFileContent(QStringLiteral("DBC Files (*.dbc *.DBC)"),
                                    [self, slot, label](const QString &name, const QByteArray &content) {
        if (!self || name.isEmpty()) return; // empty: the picker was cancelled
        const QString shown = QFileInfo(name).fileName();
        const QString dir = QDir::tempPath() + QStringLiteral("/web-savvycan-dbc");
        QDir().mkpath(dir);
        const QString path = dir + QLatin1Char('/') + shown;
        {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) {
                QFile::remove(path);
                QMessageBox::warning(self, tr("Error Loading"), tr("%1 could not be read.").arg(shown));
                return;
            }
        }
        QProgressDialog progress(self);
        progress.setWindowModality(Qt::WindowModal);
        progress.setLabelText("Loading file...");
        progress.setCancelButton(nullptr);
        progress.setRange(0,0);
        progress.setMinimumDuration(1000);
        delete *slot;
        *slot = new DBCFile;
        const bool ok = (*slot)->loadFile(path);
        QFile::remove(path);
        progress.cancel();
        if (!ok) {
            delete *slot;
            *slot = nullptr;
            QMessageBox::warning(self, tr("Error Loading"), tr("%1 could not be read.").arg(shown));
            label->setText(QString());
            return;
        }
        label->setText(shown);
        if (self->firstDBC && self->secondDBC) self->calculateDetails();
    });
}
#endif

QString DBCComparatorWindow::loadDBC(DBCFile **file)
{
    QString filename;
    QFileDialog dialog;
    QSettings settings;

    QStringList filters;
    filters.append(QString(tr("DBC Files (*.dbc *.DBC)")));

    dialog.setDirectory(settings.value("FileIO/LoadSaveDirectory", dialog.directory().path()).toString());
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];

        QProgressDialog progress(qApp->activeWindow());
        progress.setWindowModality(Qt::WindowModal);
        progress.setLabelText("Loading file...");
        progress.setCancelButton(nullptr);
        progress.setRange(0,0);
        progress.setMinimumDuration(0);
        progress.show();

        qApp->processEvents();

        if (dialog.selectedNameFilter() == filters[0])
        {
            if (file[0]) delete file[0];
            file[0] = new DBCFile;
            file[0]->loadFile(filename);
            qDebug() << "Loaded the DBC file into first slot";
        }

        progress.cancel();
        return filename;
    }
    return QString();
}

void DBCComparatorWindow::loadFirstFile()
{
#ifdef Q_OS_WASM
    pickDBC(&firstDBC, ui->lblFirstFile);
    return;
#endif
    QString filename;
    filename = loadDBC(&firstDBC);
    ui->lblFirstFile->setText(filename);
    if (firstDBC && secondDBC) calculateDetails();
}

void DBCComparatorWindow::loadSecondFile()
{
#ifdef Q_OS_WASM
    pickDBC(&secondDBC, ui->lblSecondFile);
    return;
#endif
    QString filename;
    filename = loadDBC(&secondDBC);
    ui->lblSecondFile->setText(filename);
    if (firstDBC && secondDBC) calculateDetails();
}

// web-savvycan: SavvyCAN's tree, built by FileTools::buildDbcDiff (its loop over the second DBC's signals ran on the
// message index and read past the message list; tree items no longer leaked for unchanged signals)
void DBCComparatorWindow::calculateDetails()
{
    QElapsedTimer timer;
    timer.start();
    ui->treeDetails->clear();
    ui->treeDetails->addTopLevelItems(FileTools::buildDbcDiff(firstDBC, secondDBC));

    QSettings settings;
    if (settings.value("InfoCompare/AutoExpand", false).toBool())
    {
        ui->treeDetails->expandAll();
    }
    compareMs = timer.nsecsElapsed() / 1e6;
}

void DBCComparatorWindow::saveDetails()
{
#ifdef Q_OS_WASM
    // web-savvycan: in the browser the text goes to the browser as a download (DBCComparison.txt)
    {
        QByteArray out;
        for (QTreeWidgetItemIterator it(ui->treeDetails); *it; ++it) {
            QTreeWidgetItem *item = *it;
            const QString itemText = item->text(0);
            for (; item->parent(); item = item->parent()) out += "   ";
            out += itemText.toUtf8() + "\n";
        }
        QFileDialog::saveFileContent(out, QStringLiteral("DBCComparison.txt"));
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

