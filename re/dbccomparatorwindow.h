#ifndef DBCCOMPARATORWINDOW_H
#define DBCCOMPARATORWINDOW_H

#include <QDialog>
#include <QDebug>
#include <QTreeWidget>
#include "framefileio.h"
#include "dbc/dbc_classes.h"
#include "dbc/dbchandler.h"
#include "utility.h"

namespace Ui {
class DBCComparatorWindow;
}

class DBCComparatorWindow : public QDialog
{
    Q_OBJECT

public:
    explicit DBCComparatorWindow(QWidget *parent = 0);
    ~DBCComparatorWindow();

    // for the tests (testhook)
    DBCFile *first() const { return firstDBC; }
    DBCFile *second() const { return secondDBC; }
    double lastCompareMs() const { return compareMs; }

private slots:
    void loadFirstFile();
    void loadSecondFile();
    void saveDetails();

private:
    Ui::DBCComparatorWindow *ui;
    DBCFile *firstDBC;
    DBCFile *secondDBC;
    QString firstDBCFilename;
    QString secondDBCFilename;

    void calculateDetails();
    void showEvent(QShowEvent *);
    void closeEvent(QCloseEvent *event);
    bool eventFilter(QObject *obj, QEvent *event);
    QString loadDBC(DBCFile **file);
#ifdef Q_OS_WASM
    void pickDBC(DBCFile **slot, class QLabel *label);
#endif
    double compareMs = 0;
    void readSettings();
    void writeSettings();
};

#endif //DBCCOMPARATORWINDOW_H
