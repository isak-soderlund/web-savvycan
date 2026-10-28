#ifndef ABOUTDIALOG_H
#define ABOUTDIALOG_H

#include <QDialog>

// web-savvycan: Help > About. The licence notice, SavvyCAN's, QCustomPlot's and Qt's credits, and
// this build's git commit with a link to its source (the GPL source offer for every hosted build).
class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = nullptr);

    static QString gitCommit();     // the commit this build was made from ("+dirty" if changed)
    static QString sourceUrl();     // https://github.com/isak-soderlund/web-savvycan[/tree/<commit>]
};

#endif // ABOUTDIALOG_H
