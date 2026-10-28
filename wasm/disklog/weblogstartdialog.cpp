#include "weblogstartdialog.h"

#include <QDateTime>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

WebLogStartDialog::WebLogStartDialog(bool filtered, QWidget *parent) : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(filtered ? tr("Start Continuous Logging (Filtered)") : tr("Start Continuous Logging"));
    auto *layout = new QVBoxLayout(this);
    WebDiskLog *log = WebDiskLog::instance();
    const QString backend = log->backend();
    QSettings settings;

    auto addText = [&](const QString &text) {
        auto *l = new QLabel(text, this);
        l->setWordWrap(true);
        l->setTextFormat(Qt::RichText);
        layout->addWidget(l);
        return l;
    };

    auto *cancel = new QPushButton(tr("Cancel"), this);
    cancel->setAutoDefault(false); //Enter starts the log (the default button), it does not cancel
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    if (backend == QLatin1String("fsa"))
    {
        addText(tr("The log is written into a file on your disk while it runs and saved every few seconds: "
                   "if the tab or the browser crashes, the file keeps everything up to the last save. "
                   "Format: SavvyCAN's GVRET CSV (CAN FD rows included)."));

        const QString example = QStringLiteral("SavvyCAN_%1.csv")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")));
        m_direct = new QPushButton(tr("Name it by the start time"), this);
        m_direct->setToolTip(tr("Creates e.g. %1 in the log folder, without asking").arg(example));
        connect(m_direct, &QPushButton::clicked, this, [this]() { choose(WebDiskLog::Mode::StartTimeName); });
        layout->addWidget(m_direct);

        auto *folderRow = new QHBoxLayout();
        m_folder = new QLabel(tr("Log folder: ..."), this);
        m_folder->setWordWrap(true);
        m_folder->setTextFormat(Qt::RichText);
        folderRow->addWidget(m_folder, 1);
        auto *change = new QPushButton(tr("Change folder..."), this);
        change->setAutoDefault(false);
        connect(change, &QPushButton::clicked, log, &WebDiskLog::chooseDirectory);
        folderRow->addWidget(change);
        layout->addLayout(folderRow);

        // Each save (commit) makes the browser copy the whole file, so a long log in one file saves ever more
        // slowly (measured: ~5 ms per MB). Parts keep every save short; each part is a complete log file.
        const int splitMB = settings.value("WebLog/SplitMB", 128).toInt();
        m_split = new QCheckBox(tr("Start a new file every %1 MB (keeps the saves quick; each file loads on its own)")
                                    .arg(splitMB > 0 ? splitMB : 128), this);
        m_split->setChecked(settings.value("WebLog/Split", true).toBool());
        layout->addWidget(m_split);

        m_prompt = new QPushButton(tr("Choose the file name..."), this);
        m_prompt->setToolTip(tr("The browser asks where to save the log and its name"));
        connect(m_prompt, &QPushButton::clicked, this, [this]() { choose(WebDiskLog::Mode::PromptForName); });
        layout->addWidget(m_prompt);

        connect(log, &WebDiskLog::directoryInfo, this, &WebLogStartDialog::showDirectory);
        log->queryDirectory();

        // the last way used is the default button
        QPushButton *def = settings.value("WebLog/Mode", "direct").toString() == QLatin1String("prompt") ? m_prompt : m_direct;
        (def == m_prompt ? m_direct : m_prompt)->setAutoDefault(false);
        def->setDefault(true);
        QTimer::singleShot(0, def, [def]() { def->setFocus(); }); //after the dialog has placed its focus
    }
    else if (backend == QLatin1String("download"))
    {
        addText(tr("This browser cannot write into files on disk directly (Chrome and Edge can). "
                   "The log is saved as a <b>download</b> that grows while logging runs and is finished "
                   "when you stop logging."));
        addText(tr("<b>Limitation:</b> the log is only safe once logging is stopped. If the browser crashes "
                   "while logging, the download is left unfinished (a partial <i>.part</i> file at best) and "
                   "the log may be lost; if the tab is closed, the download ends with what had arrived. For "
                   "long or important logs, use Chrome or Edge, or the desktop SavvyCAN."));
        auto *start = new QPushButton(tr("Start (download)"), this);
        start->setDefault(true);
        start->setFocus();
        connect(start, &QPushButton::clicked, this, [this]() { choose(WebDiskLog::Mode::Download); });
        layout->addWidget(start);
    }
    else
    {
        addText(tr("This browser cannot save a continuous log. Use Chrome or Edge (or Firefox, over https), "
                   "or the desktop SavvyCAN."));
        cancel->setText(tr("Close"));
    }

    auto *buttons = new QHBoxLayout();
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    setMinimumWidth(560);
    adjustSize(); //word-wrapped labels: adjustSize() takes their height for this width
}

void WebLogStartDialog::showDirectory(const QString &name, const QString &permission)
{
    if (!m_folder) return;
    QTimer::singleShot(0, this, [this]() { adjustSize(); }); //the text's height changes
    if (name.isEmpty())
    {
        m_folder->setText(tr("Log folder: <i>not chosen yet</i>. The first time, the browser asks you to pick "
                             "a folder and to allow this page to save files in it (a web page may only write "
                             "where you allow it). It remembers the folder, so later logs start right away. "
                             "After the browser is restarted it asks once more: choose <i>Allow on every "
                             "visit</i> and it will not ask again."));
    }
    else if (permission == QLatin1String("granted"))
    {
        m_folder->setText(tr("Log folder: <b>%1</b>").arg(name.toHtmlEscaped()));
    }
    else
    {
        m_folder->setText(tr("Log folder: <b>%1</b> (the browser asks once to allow saving there again: "
                             "<i>Allow on every visit</i> stops it asking)")
                              .arg(name.toHtmlEscaped()));
    }
}

void WebLogStartDialog::choose(WebDiskLog::Mode mode)
{
    QSettings settings;
    if (mode == WebDiskLog::Mode::PromptForName) settings.setValue("WebLog/Mode", "prompt");
    else if (mode == WebDiskLog::Mode::StartTimeName) settings.setValue("WebLog/Mode", "direct");
    // emitted inside the click: the browser's picker (if any) opens within this user gesture
    qint64 split = 0;
    if (m_split)
    {
        settings.setValue("WebLog/Split", m_split->isChecked());
        if (m_split->isChecked()) split = qint64(settings.value("WebLog/SplitMB", 128).toInt()) * 1024 * 1024;
    }
    emit chosen(mode, split);
    accept();
}
