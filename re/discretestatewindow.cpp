#include "discretestatewindow.h"
#include "ui_discretestatewindow.h"
#include "mainwindow.h"
#include "helpwindow.h"
#include "theme.h"
#include "connections/canconmanager.h"

#include <QElapsedTimer>
#include <algorithm>

/*
 * web-savvycan (stage 4): RE Tools > Single/Multi State. SavvyCAN's window had the interface and the realtime
 * countdown but no search (calculateResults() was empty in both modes, and its state counter ran one state past
 * "Number of States"). Here, with the same interface:
 * - Realtime: each iteration asks for every state in turn ("Go to state 1", "Go to state 2", ...; a countdown before
 *   each, "Wait...." after), Toggle Frequency seconds each. Frames from the bus (CANConManager, so the trace's ring
 *   does not matter) are recorded per ID and state while a state is held, after its first quarter (time to get
 *   there). Then every field of Min..Max bits (Intel bit numbering, its first and last bits changing) is a match when
 *   in every state at least 90 % of its frames have one value and those values differ from state to state
 *   (ReTools::discreteRealtime). Memory: ReTools::StateSampler (an even spread of at most 2048 frames per ID and
 *   state, all together within 32 MB).
 * - From logged data: per checked ID, fields of Min..Max bits that take exactly Number of States values over the
 *   trace's frames (the comment SavvyCAN left: "record every unique value. IF the # of unique values is the same as
 *   the number of states then we've got a match"), tightest fields only (ReTools::discreteLogged).
 * - CAN FD: fields anywhere in 64 bytes.
 * - Live: new IDs join the list from the trace's newest frames (the trace is a ring).
 * - The status colours lighter in dark; in the browser a framed dialog.
 */

DiscreteStateWindow::DiscreteStateWindow(const FrameListView *frames, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DiscreteStateWindow)
{
    ui->setupUi(this);
#ifdef Q_OS_WASM
    // web-savvycan: a framed dialog in the browser (a Qt::Window is frameless over the main window's menus)
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
#else
    setWindowFlags(Qt::Window);
#endif

#ifdef Q_OS_WASM
    resize(640, 560); // web-savvycan: see readSettings()
#endif
    modelFrames = frames;
    operatingState = DWStates::IDLE;
    ticksUntilStateChange = ticksPerStateChange = 0;
    numToggleStates = currToggleState = numIterations = currIteration = 0;

    timer = new QTimer();
    timer->setInterval(100);

    isRealtime = ui->rbRealtime->isChecked();
    typeChanged();

    ui->treeMatches->setColumnCount(5);
    ui->treeMatches->setHeaderLabels({tr("ID"), tr("Start bit"), tr("Bits"), tr("Values"), tr("Frames")});
    ui->treeMatches->setRootIsDecorated(false);
    setLabelColor(0);

    connect(ui->btnStart, SIGNAL(clicked(bool)), this, SLOT(handleStartButton()));
    connect(timer, SIGNAL(timeout()), this, SLOT(handleTick()));
    connect(MainWindow::getReference(), SIGNAL(framesUpdated(int)), this, SLOT(updatedFrames(int)));
    connect(ui->rbLogged, SIGNAL(clicked(bool)), this, SLOT(typeChanged()));
    connect(ui->rbRealtime, SIGNAL(clicked(bool)), this, SLOT(typeChanged()));
    connect(CANConManager::getInstance(), &CANConManager::framesReceived, this, &DiscreteStateWindow::framesReceived);
    connect(Theme::instance(), &Theme::changed, this, [this]() {
        if (operatingState == DWStates::IDLE && ui->lblStatus->text() == "WAIT") setLabelColor(0);
        else updateStateLabel();
    });

    connect(ui->btnAll, &QAbstractButton::clicked,
            [=]()
            {
                for (int i = 0; i < ui->listID->count(); i++)
                {
                    QListWidgetItem *item = ui->listID->item(i);
                    item->setCheckState(Qt::Checked);
                    idFilters[Utility::ParseStringToNum(item->text())] = true;
                }
            });

    connect(ui->btnNone, &QAbstractButton::clicked,
            [=]()
            {
                for (int i = 0; i < ui->listID->count(); i++)
                {
                    QListWidgetItem *item = ui->listID->item(i);
                    item->setCheckState(Qt::Unchecked);
                    idFilters[Utility::ParseStringToNum(item->text())] = false;
                }
            });

    connect(ui->listID, &QListWidget::itemChanged,
            [=](QListWidgetItem *item)
            {
                bool isChecked = false;
                int id = Utility::ParseStringToNum(item->text());
                if (item->checkState() == Qt::Checked) isChecked = true;
                idFilters[id] = isChecked;
            });

    refreshFilterList();
    installEventFilter(this);
}

DiscreteStateWindow::~DiscreteStateWindow()
{
    removeEventFilter(this);
    timer->stop();
    delete timer;
    delete ui;
}

bool DiscreteStateWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key())
        {
        case Qt::Key_F1:
            HelpWindow::getRef()->showHelp("discretestate.md");
            break;
        }
        return true;
    } else {
        // standard event processing
        return QObject::eventFilter(obj, event);
    }
    return false;
}

void DiscreteStateWindow::typeChanged()
{
    if (ui->rbLogged->isChecked())
    {
        ui->spinFreq->setEnabled(false);
        ui->spinIterations->setEnabled(false);
        ui->lblStatus->setEnabled(false);
        ui->spinMaxBits->setEnabled(true);
        ui->spinMinBits->setEnabled(true);
        ui->listID->setEnabled(true);
        ui->btnAll->setEnabled(true);
        ui->btnNone->setEnabled(true);
        isRealtime = false;
    }
    else
    {
        ui->spinFreq->setEnabled(true);
        ui->spinIterations->setEnabled(true);
        ui->lblStatus->setEnabled(true);
        ui->spinMaxBits->setEnabled(false);
        ui->spinMinBits->setEnabled(false);
        ui->listID->setEnabled(false);
        ui->btnAll->setEnabled(false);
        ui->btnNone->setEnabled(false);
        isRealtime = true;
    }
}

void DiscreteStateWindow::addFilterId(const CommFrame &frame)
{
    if (!ReTools::hasData(frame)) return;
    const int id = int(frame.frameId());
    if (idFilters.contains(id)) return;
    idFilters.insert(id, true);
    QListWidgetItem* listItem = new QListWidgetItem(Utility::formatCANID(frame.frameId(), frame.hasExtendedFrameFormat()), ui->listID);
    listItem->setFlags(listItem->flags() | Qt::ItemIsUserCheckable); // set checkable flag
    listItem->setCheckState(Qt::Checked); //default all filters to be set active
}

void DiscreteStateWindow::updatedFrames(int numFrames)
{
    if (numFrames == -1) //all frames deleted (or a file loaded in their place)
    {
        ui->listID->clear();
        idFilters.clear();
        refreshFilterList();
    }
    else if (numFrames == -2) //all new set of frames. Reset
    {
        refreshFilterList();
    }
    else if (numFrames > 0) //just got some new frames. See if they are relevant.
    {
        // web-savvycan: the newest numFrames frames are the trace's tail (fewer if the ring kept fewer)
        const int before = ui->listID->count();
        for (const CommFrame &f : modelFrames->tail(numFrames)) addFilterId(f);
        if (ui->listID->count() != before) ui->listID->sortItems();
    }
}

void DiscreteStateWindow::refreshFilterList()
{
    idFilters.clear();
    ui->listID->clear();

    const qsizetype count = modelFrames->count();
    for (qsizetype i = 0; i < count; i += 4096)
        for (const CommFrame &f : modelFrames->mid(i, 4096)) addFilterId(f);

    ui->listID->sortItems();
}

void DiscreteStateWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    readSettings();

    qDebug() << "DiscreteState show event was processed";
}

void DiscreteStateWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);
    writeSettings();
}

void DiscreteStateWindow::readSettings()
{
    QSettings settings;
    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
#ifdef Q_OS_WASM
        // web-savvycan: first opened large enough to read the matches (400 x 300 showed one and a half rows)
        const QSize defaultSize(640, 560);
#else
        const QSize defaultSize(400, 300);
#endif
        resize(settings.value("DiscreteState/WindowSize", defaultSize).toSize());
        move(Utility::constrainedWindowPos(settings.value("DiscreteState/WindowPos", QPoint(50, 50)).toPoint()));
    }

}

void DiscreteStateWindow::writeSettings()
{
    QSettings settings;

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("DiscreteState/WindowSize", size());
        settings.setValue("DiscreteState/WindowPos", pos());
    }
}

void DiscreteStateWindow::setLabelColor(int which)
{
    // SavvyCAN's red, blue and green; in dark lighter ones (pure blue can't be read on a dark window)
    static const QColor light[3] = {Qt::red, Qt::blue, Qt::green};
    static const QColor dark[3] = {QColor(0xff, 0x60, 0x60), QColor(0x6c, 0x9c, 0xff), QColor(0x40, 0xe0, 0x40)};
    QPalette pal = ui->lblStatus->palette();
    const QColor c = (Theme::isDark() ? dark : light)[qBound(0, which, 2)];
    pal.setColor(QPalette::Active, QPalette::WindowText, c);
    pal.setColor(QPalette::Inactive, QPalette::WindowText, c);
    ui->lblStatus->setPalette(pal);
}

void DiscreteStateWindow::updateStateLabel()
{
    switch(operatingState)
    {
    case DWStates::IDLE:
        ui->lblStatus->setText("IDLE");
        setLabelColor(0);
        break;
    case DWStates::COUNTDOWN_SIGNAL:
        ui->lblStatus->setText(QString::number(ticksUntilStateChange * 0.1, 'f', 1) + "...");
        setLabelColor(1);
        break;
    case DWStates::COUNTDOWN_WAITING:
        ui->lblStatus->setText("Wait....");
        setLabelColor(0);
        break;
    case DWStates::GETTING_SIGNAL:
        ui->lblStatus->setText("Go to state " + QString::number(currToggleState + 1));
        setLabelColor(2);
        break;
    case DWStates::DONE:
        ui->lblStatus->setText("DONE");
        setLabelColor(2);
        break;
    }
}

void DiscreteStateWindow::handleStartButton()
{
    results.clear();
    ui->treeMatches->clear();
    if (isRealtime)
    {
        operatingState = DWStates::COUNTDOWN_SIGNAL;

        ticksPerStateChange = ticksUntilStateChange = ui->spinFreq->value() * 10;
        numToggleStates = ui->spinStates->value();
        numIterations = ui->spinIterations->value();

        currToggleState = 0;
        currIteration = 0;
        recordingNow = false;
        samples.start(numToggleStates);

        updateStateLabel();
        timer->start();
    }
    else
    {
        calculateResults();
    }
}

void DiscreteStateWindow::handleTick()
{
    switch (operatingState)
    {
    case DWStates::IDLE:
        break;
    case DWStates::DONE:
        break;
    case DWStates::COUNTDOWN_SIGNAL:
        ticksUntilStateChange--;
        if (ticksUntilStateChange <= 0)
        {
            ticksUntilStateChange = ticksPerStateChange;
            operatingState = DWStates::GETTING_SIGNAL;
            recordingNow = false;
        }
        break;
    case DWStates::COUNTDOWN_WAITING:
        ticksUntilStateChange--;
        if (ticksUntilStateChange <= 0)
        {
            ticksUntilStateChange = ticksPerStateChange;
            operatingState = DWStates::COUNTDOWN_SIGNAL;
        }
        break;
    case DWStates::GETTING_SIGNAL:
        ticksUntilStateChange--;
        // the first quarter of the period is the time to get there: not recorded
        recordingNow = ticksUntilStateChange * 4 < ticksPerStateChange * 3;
        if (ticksUntilStateChange <= 0)
        {
            ticksUntilStateChange = ticksPerStateChange;
            recordingNow = false;
            // web-savvycan: states 0..numToggleStates-1 (SavvyCAN's counter went one past), an iteration is every
            // state once, then the results
            currToggleState++;
            if (currToggleState >= numToggleStates)
            {
                currToggleState = 0;
                currIteration++;
            }
            if (currIteration >= numIterations)
            {
                operatingState = DWStates::DONE;
                timer->stop();
                calculateResults();
            }
            else operatingState = DWStates::COUNTDOWN_WAITING;
        }
        break;
    }
    updateStateLabel();
}

void DiscreteStateWindow::framesReceived(CANConnection *, QVector<CommFrame> &frames)
{
    if (!isRealtime || operatingState != DWStates::GETTING_SIGNAL || !recordingNow) return;
    for (const CommFrame &f : frames) samples.add(f, currToggleState);
}

void DiscreteStateWindow::calculateResults()
{
    QElapsedTimer clock;
    clock.start();
    results.clear();
    const int states = ui->spinStates->value();
    const int minBits = ui->spinMinBits->value();
    const int maxBits = ui->spinMaxBits->value();
    if (isRealtime)
    {
        // every ID heard in every state, by ID
        QList<quint64> keys = samples.entries().keys();
        std::sort(keys.begin(), keys.end(), [](quint64 a, quint64 b) {
            return ((a >> 32) != (b >> 32)) ? (a >> 32) < (b >> 32) : quint32(a) < quint32(b);
        });
        for (quint64 k : keys)
        {
            const ReTools::StateSampler::Entry &e = samples.entries()[k];
            ReTools::discreteRealtime(e.id, e.ext, e.perState, minBits, maxBits, results);
        }
    }
    else //use already loaded frames from main cache
    {
        //basic overview: run through all ID filters and see if it is enabled.
        //If so add it to a giant list of messages by ID where each ID has its own
        //list of messages
        //Then, for each ID start a loop that runs from largest bits to smallest bits for scan
        //For each value run through the algorithm for all messages. At the end list any matches
        //the simplest approach seems to be to grab that number of bits and then record every
        //unique value. IF the # of unique values is the same as the number of states then
        //we've got a match.It should be noted that the # of states must be at least 2 - the idle
        //state is 1 and then a second state at the minimum. Turn signals might be 3 states then
        // web-savvycan: one pass over the trace into compact rows per checked ID (no event loop meanwhile: the ring
        // keeps its rows)
        QMap<quint64, ReTools::IdFrames> perId;
        const qsizetype count = modelFrames->count();
        for (qsizetype i = 0; i < count; i += 4096)
        {
            for (const CommFrame &f : modelFrames->mid(i, 4096))
            {
                if (!ReTools::hasData(f) || !idFilters.value(int(f.frameId()), false)) continue;
                const quint64 key = (f.hasExtendedFrameFormat() ? (quint64(1) << 32) : 0) | f.frameId();
                perId[key].append(f);
            }
        }
        for (auto it = perId.cbegin(); it != perId.cend(); ++it)
            ReTools::discreteLogged(quint32(it.key()), (it.key() >> 32) != 0, it.value(), states, minBits, maxBits, results);
    }
    runMs = clock.elapsed();
    runCount++;
    showResults();
}

void DiscreteStateWindow::showResults()
{
    ui->treeMatches->clear();
    QList<QTreeWidgetItem *> items;
    for (const ReTools::DiscreteMatch &m : results)
    {
        if (items.size() >= kMaxMatchesShown) break;
        QStringList values;
        for (size_t s = 0; s < m.values.size(); s++)
            values << (isRealtime ? QString("state %1: %2").arg(int(s) + 1).arg(m.values[s]) : QString::number(m.values[s]));
        QString frames = QString::number(m.frames);
        if (isRealtime) frames += QString(" (%1 %)").arg(qRound(m.share * 100));
        const int byte = m.startBit / 8, bit = m.startBit % 8;
        items << new QTreeWidgetItem(QStringList{Utility::formatCANID(m.id, m.ext),
                                                 QString("%1 (byte %2 bit %3)").arg(m.startBit).arg(byte).arg(bit),
                                                 QString::number(m.bits), values.join(", "), frames});
    }
    if (int(results.size()) > kMaxMatchesShown)
        items << new QTreeWidgetItem(QStringList{tr("%1 more not shown").arg(int(results.size()) - kMaxMatchesShown)});
    if (results.empty()) items << new QTreeWidgetItem(QStringList{tr("No matches")});
    ui->treeMatches->addTopLevelItems(items);
    for (int c = 0; c < ui->treeMatches->columnCount(); c++) ui->treeMatches->resizeColumnToContents(c);
}
