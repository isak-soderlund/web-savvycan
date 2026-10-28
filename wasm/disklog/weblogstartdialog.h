#ifndef WEBLOGSTARTDIALOG_H
#define WEBLOGSTARTDIALOG_H

// web-savvycan: "Start Continuous Logging" in the browser. Offers the two ways of naming the log file
// (the owner's decision 2026-10-28): the browser's save dialog, or a file named by the start time in a
// remembered folder; in browsers without the File System Access API, the streaming download, with its
// limitation. Shown with open() (no exec()), so the buttons' clicks are the user gesture the browser's
// pickers need.

#include <QDialog>
#include "webdisklog.h"

class QLabel;
class QPushButton;
class QCheckBox;

class WebLogStartDialog : public QDialog
{
    Q_OBJECT

public:
    explicit WebLogStartDialog(bool filtered, QWidget *parent = nullptr);

signals:
    void chosen(WebDiskLog::Mode mode, qint64 splitBytes);

private:
    void choose(WebDiskLog::Mode mode);
    void showDirectory(const QString &name, const QString &permission);

    QLabel *m_folder = nullptr;
    QPushButton *m_direct = nullptr;
    QPushButton *m_prompt = nullptr;
    QCheckBox *m_split = nullptr;
};

#endif // WEBLOGSTARTDIALOG_H
