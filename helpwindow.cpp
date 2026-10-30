#include <QtDebug>
#include "utility.h"
#include "showwindow.h"   // web-savvycan: to the front (its tab in the browser)
#include "helpwindow.h"
#include "ui_helpwindow.h"

HelpWindow* HelpWindow::self = nullptr;

HelpWindow::HelpWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::HelpWindow)
{
    ui->setupUi(this);
    setWindowFlags(Qt::Window);

    readSettings();
}

HelpWindow::~HelpWindow()
{
    writeSettings();
    delete ui;
}

void HelpWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);
    writeSettings();
}

void HelpWindow::readSettings()
{
    QSettings settings;
    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        resize(settings.value("HelpViewer/WindowSize", QSize(600, 700)).toSize());
        move(Utility::constrainedWindowPos(settings.value("HelpViewer/WindowPos", QPoint(50, 50)).toPoint()));
    }
}

void HelpWindow::writeSettings()
{
    QSettings settings;

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("HelpViewer/WindowSize", size());
        settings.setValue("HelpViewer/WindowPos", pos());
    }
}

HelpWindow* HelpWindow::getRef()
{
    if (!self)
    {
        self = new HelpWindow();
    }

    return self;
}

void HelpWindow::showHelp(QString help)
{
#ifdef Q_OS_WASM
    // web-savvycan: the help pages aren't part of the browser build; F1 does nothing there (owner 2026-10-29).
    Q_UNUSED(help);
    return;
#endif
    QString helpfile = QCoreApplication::applicationDirPath() + "/help/" + help;
    QUrl url = QUrl::fromLocalFile(helpfile);
    qDebug() << "Searching for " << url;
    ui->textHelp->setSource(url);

    readSettings();
    showWindow(self);
}


