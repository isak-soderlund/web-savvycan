#include "aboutdialog.h"
#include "config.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtGlobal>

#ifndef WEB_SAVVYCAN_GIT_COMMIT
#define WEB_SAVVYCAN_GIT_COMMIT "unknown"
#endif

static const char *kRepoUrl = "https://github.com/isak-soderlund/web-savvycan";

QString AboutDialog::gitCommit()
{
    return QStringLiteral(WEB_SAVVYCAN_GIT_COMMIT);
}

QString AboutDialog::sourceUrl()
{
    QString commit = gitCommit();
    commit.remove(QStringLiteral("+dirty"));
    if (commit.isEmpty() || commit == QLatin1String("unknown")) return QString::fromLatin1(kRepoUrl);
    return QString::fromLatin1(kRepoUrl) + QStringLiteral("/tree/") + commit;
}

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("About web-savvycan"));
    resize(620, 600);

    const QString commit = gitCommit();
    const QString repo = QString::fromLatin1(kRepoUrl);
#ifdef Q_OS_WASM
    const QString platform = tr("browser (WebAssembly)");
#else
    const QString platform = tr("desktop");
#endif

    QString html;
    html += QStringLiteral("<h2>web-savvycan by Resolve</h2>");
    html += tr("<p>SavvyCAN in the browser and on the desktop, for the resolve-can CAN FD adapter.<br>"
               "Based on SavvyCAN V%1 (branch QT6WIP).</p>").arg(VERSION);
    html += tr("<p><b>Build:</b> commit <a href=\"%1\"><code>%2</code></a>, %3, Qt %4 (built with %5), %6</p>")
                .arg(sourceUrl().toHtmlEscaped(), commit.toHtmlEscaped(), platform,
                     QString::fromLatin1(qVersion()), QStringLiteral(QT_VERSION_STR),
                     QStringLiteral(__DATE__));
    html += tr("<p><b>Source code:</b> <a href=\"%1\">%1</a></p>").arg(repo);

    html += tr("<h3>Licence</h3>"
               "<p>This program is free software: you can redistribute it and/or modify it under the terms of "
               "the GNU General Public License as published by the Free Software Foundation, either version 3 "
               "of the License, or (at your option) any later version.</p>"
               "<p>This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; "
               "without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. "
               "See the GNU General Public License for more details: "
               "<a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">https://www.gnu.org/licenses/gpl-3.0.html</a>.</p>"
               "<p>The complete source code of this build is at the link above (the commit it names).</p>");

    html += tr("<h3>Credits</h3>"
               "<p><b>SavvyCAN</b> &copy; 2015-2026 Collin Kidder (collin80) and the SavvyCAN contributors "
               "(Andy Huska, canpadawan, BitterAndReal, Alexey Esaulenko, Tom de Bree, Tom-evnut, Bertrand, "
               "Andre Hartmann and many more). MIT License. "
               "<a href=\"https://github.com/collin80/SavvyCAN\">https://github.com/collin80/SavvyCAN</a></p>"
               "<p><b>QCustomPlot</b> 2.1.1 &copy; 2011-2022 Emanuel Eichhammer, GNU GPL version 2 or later. "
               "<a href=\"https://www.qcustomplot.com/\">https://www.qcustomplot.com/</a></p>"
               "<p><b>Qt</b> &copy; The Qt Company Ltd. and other contributors, used under the GNU LGPL "
               "version 3. <a href=\"https://www.qt.io/\">https://www.qt.io/</a></p>"
               "<p>web-savvycan is not affiliated with or endorsed by the SavvyCAN project or EVTV.</p>");

    QTextBrowser *text = new QTextBrowser(this);
    text->setObjectName("aboutText");
    text->setOpenExternalLinks(true);
    text->setHtml(html);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked, this, &QDialog::accept);
    buttons->button(QDialogButtonBox::Close)->setDefault(true);   // Enter closes it
    buttons->button(QDialogButtonBox::Close)->setFocus();

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(text);
    layout->addWidget(buttons);
}
