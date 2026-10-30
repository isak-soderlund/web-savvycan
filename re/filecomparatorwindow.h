#ifndef FILECOMPARATORWINDOW_H
#define FILECOMPARATORWINDOW_H

#include <QDialog>
#include <QDebug>
#include <QTreeWidget>
#include "framefileio.h"
#include "can_structs.h"
#include "utility.h"
#include "dbc/dbchandler.h"
#include "re/filetools.h"

namespace Ui {
class FileComparatorWindow;
}

class FileComparatorWindow : public QDialog
{
    Q_OBJECT

public:
    explicit FileComparatorWindow(QWidget *parent = 0);
    ~FileComparatorWindow();

    // for the tests (testhook): what each side holds, the time of the last load and comparison
    const FileTools::CompareSide &interestedSide() const { return interested; }
    const FileTools::CompareSide &referenceSide() const { return reference; }
    QString interestedName() const { return interestedFilename; }
    double lastLoadMs() const { return loadMs; }
    double lastCompareMs() const { return compareMs; }

private slots:
    void loadInterestedFile();
    void loadReferenceFile();
    void clearReference();
    void saveDetails();

private:
    Ui::FileComparatorWindow *ui;
    // web-savvycan: each side summarised as its file is loaded (FileTools::CompareSide: per ID the values per byte and
    // the bits set, CAN FD's 64 bytes), its frames dropped; SavvyCAN kept every frame of both files
    FileTools::CompareSide interested;
    FileTools::CompareSide reference;
    QString interestedFilename;
    DBCHandler *dbcHandler;
    double loadMs = 0, compareMs = 0;

    bool summarise(const QString &name, const QByteArray &content, FileTools::CompareSide *side);
    void interestedLoaded(const QString &name);
    void referenceLoaded();
    void calculateDetails();
    void showEvent(QShowEvent *);
    void closeEvent(QCloseEvent *event);
    bool eventFilter(QObject *obj, QEvent *event);
    void readSettings();
    void writeSettings();
};

#endif // FILECOMPARATORWINDOW_H
