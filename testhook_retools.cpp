// web-savvycan: the browser tests' hook (tests/web/README.md) for the live reverse-engineering tools of stage 4:
// RE Tools > Sniffer, Flow View, Range State and Single/Multi State. Their own file (installed from
// TestHook::install), so it merges apart from the other windows' hooks. WebAssembly build only.
//
// webSavvyCAN.sniffer(), snifferDo(action, id, on)
// webSavvyCAN.flowView(), flowViewFrames(from, n), flowViewDo(action, a, b)
// webSavvyCAN.rangeState(), rangeStateRun({...}), rangeStateSelect(i)
// webSavvyCAN.discreteState(), discreteStateRun({...})
// Actions go through the windows' own widgets (buttons clicked, boxes set, list items checked) or the slots those
// call, so what the tests do is what a click does.

#include "testhook.h"
#include "re/sniffer/snifferwindow.h"
#include "re/sniffer/snifferitem.h"
#include "re/flowviewwindow.h"
#include "re/rangestatewindow.h"
#include "re/discretestatewindow.h"
#include "candatagrid.h"
#include "filterutility.h"
#include "qcustomplot.h"
#include "utility.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTreeWidget>

#include <emscripten.h>
#include <emscripten/heap.h>

namespace {

const char *keep(const QByteArray &utf8)
{
    static QByteArray buffer;
    buffer = utf8;
    return buffer.constData();
}
const char *json(const QJsonObject &o) { return keep(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
const char *json(const QJsonArray &a) { return keep(QJsonDocument(a).toJson(QJsonDocument::Compact)); }

template <class W> W *openWindow()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *t = qobject_cast<W *>(w); t && t->isVisible()) return t;
    return nullptr;
}

QJsonArray bytes(const unsigned char *d, int n)
{
    QJsonArray a;
    for (int i = 0; i < n; i++) a.append(int(d[i]));
    return a;
}

void click(QWidget *win, const char *name)
{
    if (auto *b = win->findChild<QAbstractButton *>(name)) b->click();
}

void setChecked(QWidget *win, const char *name, bool on)
{
    if (auto *b = win->findChild<QAbstractButton *>(name)) b->setChecked(on);
}

void setSpin(QWidget *win, const char *name, int v)
{
    if (auto *s = win->findChild<QSpinBox *>(name)) s->setValue(v);
}

// A checkable ID list's items: [{id, on}]
QJsonArray idItems(QListWidget *list)
{
    QJsonArray a;
    if (!list) return a;
    for (int i = 0; i < list->count(); i++)
    {
        QJsonObject o;
        o.insert("id", double(FilterUtility::getIdAsInt(list->item(i))));
        o.insert("on", list->item(i)->checkState() == Qt::Checked);
        a.append(o);
    }
    return a;
}

void checkId(QListWidget *list, quint32 id, bool on)
{
    if (!list) return;
    for (int i = 0; i < list->count(); i++)
        if (FilterUtility::getIdAsInt(list->item(i)) == id) list->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
}

QJsonObject plotState(QCustomPlot *plot)
{
    QJsonObject o;
    double points = 0, minV = 0, maxV = 0;
    bool any = false;
    for (int i = 0; plot && i < plot->graphCount(); i++)
    {
        auto data = plot->graph(i)->data();
        points += data->size();
        for (auto it = data->constBegin(); it != data->constEnd(); ++it)
        {
            if (!any || it->value < minV) minV = it->value;
            if (!any || it->value > maxV) maxV = it->value;
            any = true;
        }
    }
    o.insert("graphs", plot ? plot->graphCount() : 0);
    o.insert("points", points);
    o.insert("minValue", minV);
    o.insert("maxValue", maxV);
    if (plot)
    {
        o.insert("xLower", plot->xAxis->range().lower);
        o.insert("xUpper", plot->xAxis->range().upper);
        if (plot->graphCount() > 0)
            o.insert("penColor", plot->graph(0)->pen().color().name());
        o.insert("background", plot->background().color().name());
    }
    return o;
}

QJsonObject heap()
{
    QJsonObject o;
    o.insert("heapSize", double(emscripten_get_heap_size()));
    return o;
}

// ---------------------------------------------------------------- Sniffer

QJsonObject snifferState()
{
    QJsonObject o;
    SnifferWindow *win = openWindow<SnifferWindow>();
    o.insert("open", win != nullptr);
    if (!win) return o;
    const SnifferModel &m = win->model();
    QJsonArray rows;
    for (auto it = m.items().cbegin(); it != m.items().cend(); ++it)
    {
        const SnifferItem *item = it.value();
        if (!item) continue;
        QJsonObject r;
        int len = 0;
        while (len < MAX_BYTES && item->getData(uchar(len)) >= 0) len++;
        QJsonArray data, changes, notch, lastData, fade;
        for (int i = 0; i < len; i++)
        {
            data.append(item->getData(uchar(i)));
            changes.append(int(item->dataChange(uchar(i))));   // 0 none, 1 up (green), 2 down (red)
            notch.append(int(item->getNotchPattern(uchar(i))));
            lastData.append(int(item->getLastData(uchar(i))));
            fade.append(int(item->getSeqInterval(uchar(i))));
        }
        r.insert("id", double(item->getId()));
        r.insert("len", len);
        r.insert("data", data);
        r.insert("last", lastData);
        r.insert("changes", changes);
        r.insert("notch", notch);
        r.insert("seqInterval", fade);
        r.insert("delta", item->getDelta());
        r.insert("elapsedMs", item->elapsed());
        rows.append(r);
    }
    o.insert("rows", rows);
    o.insert("ids", idItems(win->idList()));
    o.insert("framesSeen", double(m.framesSeen()));
    o.insert("hiddenFrames", double(m.framesOfHiddenIds()));
    o.insert("allIds", int(m.allItems().size()));
    o.insert("filtering", m.filtering());
    o.insert("maxIds", SnifferModel::kMaxIds);
    o.insert("memoryBytes", double(m.allItems().size()) * double(sizeof(SnifferItem)));
    for (const char *name : {"cbFadeInactive", "cbMuteNotched", "cbNoExpire", "cbViewBits"})
        if (auto *b = win->findChild<QCheckBox *>(name)) o.insert(name, b->isChecked());
    o.insert("width", win->width());
    o.insert("height", win->height());
    return o;
}

// ---------------------------------------------------------------- Flow View

QJsonObject flowState()
{
    QJsonObject o;
    FlowViewWindow *win = openWindow<FlowViewWindow>();
    o.insert("open", win != nullptr);
    if (!win) return o;
    const ReTools::IdFrames &c = win->cache();
    QJsonArray ids;
    if (auto *list = win->findChild<QListWidget *>("listFrameID"))
        for (int i = 0; i < list->count(); i++) ids.append(double(FilterUtility::getIdAsInt(list->item(i))));
    o.insert("ids", ids);
    o.insert("hasId", win->hasId());
    o.insert("id", double(win->cacheId()));
    o.insert("frames", c.size());
    o.insert("maxFrames", FlowViewWindow::kMaxFrames);
    o.insert("dropped", double(c.dropped()));
    o.insert("maxLength", c.maxLength());
    o.insert("cacheBytes", double(c.memoryBytes()));
    o.insert("position", win->position());
    o.insert("playing", win->isPlaying());
    const int n = qMax(8, c.maxLength());
    o.insert("curr", bytes(win->currentBytes(), qMin(64, n)));
    o.insert("ref", bytes(win->referenceBytes(), qMin(64, n)));
    if (!c.isEmpty())
    {
        o.insert("firstTime", double(c.timeUs(0)));
        o.insert("lastTime", double(c.timeUs(c.size() - 1)));
        o.insert("firstData", bytes(c.data(0), c.length(0)));
        o.insert("lastData", bytes(c.data(c.size() - 1), c.length(c.size() - 1)));
        if (win->position() >= 0 && win->position() < c.size())
        {
            o.insert("positionTime", double(c.timeUs(win->position())));
            o.insert("positionData", bytes(c.data(win->position()), c.length(win->position())));
        }
    }
    if (auto *t = win->findChild<QTableWidget *>("tableBytes")) o.insert("tableRows", t->rowCount());
    if (auto *g = win->findChild<CANDataGrid *>("flowView")) o.insert("gridBytes", g->getBytesToDraw());
    if (auto *l = win->findChild<QLabel *>("lblNumFrames")) o.insert("label", l->text());
    if (auto *s = win->findChild<QSlider *>("graphRangeSlider")) o.insert("chartRange", s->value());
    if (auto *s = win->findChild<QSlider *>("timelineSlider")) { o.insert("timelineMax", s->maximum()); o.insert("timeline", s->value()); }
    for (const char *name : {"cbLiveMode", "cbAutoRef", "cbTimeGraph", "cbLoopPlayback", "cbSync"})
        if (auto *b = win->findChild<QCheckBox *>(name)) o.insert(name, b->isChecked());
    o.insert("graphFrom", win->graphFrom());
    o.insert("graphTo", win->graphTo());
    o.insert("plot", plotState(win->plot()));
    return o;
}

QJsonArray flowFrames(int from, int n)
{
    QJsonArray a;
    FlowViewWindow *win = openWindow<FlowViewWindow>();
    if (!win) return a;
    const ReTools::IdFrames &c = win->cache();
    for (int i = qMax(0, from); i < qMin(c.size(), from + n); i++)
    {
        QJsonObject f;
        f.insert("time", double(c.timeUs(i)));
        f.insert("data", bytes(c.data(i), c.length(i)));
        a.append(f);
    }
    return a;
}

// ---------------------------------------------------------------- Range State

QJsonObject rangeStateJson()
{
    QJsonObject o;
    RangeStateWindow *win = openWindow<RangeStateWindow>();
    o.insert("open", win != nullptr);
    if (!win) return o;
    o.insert("busy", win->busy());
    o.insert("ids", idItems(win->findChild<QListWidget *>("listFilter")));
    QJsonArray cands;
    for (const ReTools::RangeCandidate &c : win->candidates())
    {
        QJsonObject x;
        x.insert("id", double(c.id));
        x.insert("startBit", c.startBit);
        x.insert("bits", c.bits);
        x.insert("signed", c.isSigned);
        x.insert("bigEndian", c.bigEndian);
        cands.append(x);
    }
    o.insert("candidates", cands);
    if (auto *list = win->findChild<QListWidget *>("listCandidates"))
    {
        o.insert("listed", list->count());
        o.insert("current", list->currentRow());
        if (list->count() > 0) o.insert("firstText", list->item(0)->text());
    }
    o.insert("lastRunMs", double(win->lastRunMs()));
    o.insert("lastRunFrames", double(win->lastRunFrames()));
    o.insert("plot", plotState(win->plot()));
    return o;
}

// ---------------------------------------------------------------- Single/Multi State

QJsonObject discreteJson()
{
    QJsonObject o;
    DiscreteStateWindow *win = openWindow<DiscreteStateWindow>();
    o.insert("open", win != nullptr);
    if (!win) return o;
    static const char *const names[] = {"IDLE", "COUNTDOWN_SIGNAL", "COUNTDOWN_WAITING", "GETTING_SIGNAL", "DONE"};
    o.insert("state", names[int(win->state())]);
    o.insert("toggleState", win->toggleState());
    o.insert("iteration", win->iteration());
    o.insert("recording", win->recording());
    o.insert("realtime", win->realtime());
    if (auto *l = win->findChild<QLabel *>("lblStatus"))
    {
        o.insert("label", l->text());
        o.insert("labelColor", l->palette().color(QPalette::WindowText).name());
    }
    o.insert("ids", idItems(win->findChild<QListWidget *>("listID")));
    QJsonArray matches;
    for (const ReTools::DiscreteMatch &m : win->matches())
    {
        QJsonObject x;
        x.insert("id", double(m.id));
        x.insert("ext", m.ext);
        x.insert("startBit", m.startBit);
        x.insert("bits", m.bits);
        QJsonArray v;
        for (int val : m.values) v.append(val);
        x.insert("values", v);
        x.insert("share", m.share);
        x.insert("frames", m.frames);
        matches.append(x);
    }
    o.insert("matches", matches);
    if (auto *t = win->findChild<QTreeWidget *>("treeMatches"))
    {
        o.insert("treeRows", t->topLevelItemCount());
        if (t->topLevelItemCount() > 0)
        {
            QJsonArray first;
            for (int c = 0; c < t->columnCount(); c++) first.append(t->topLevelItem(0)->text(c));
            o.insert("firstRow", first);
        }
    }
    const ReTools::StateSampler &s = win->sampler();
    QJsonObject sampler;
    sampler.insert("framesAdded", double(s.framesAdded()));
    sampler.insert("framesKept", double(s.framesKept()));
    sampler.insert("memoryBytes", double(s.memoryBytes()));
    sampler.insert("cap", s.cap());
    sampler.insert("ids", int(s.entries().size()));
    o.insert("sampler", sampler);
    o.insert("lastRunMs", double(win->lastRunMs()));
    return o;
}

} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE const char *wsc_sniffer_state() { return json(snifferState()); }
// 0 notch, 1 unnotch, 2 all, 3 none, 4 fade, 5 mute, 6 never expire, 7 view bits (on), 8 an ID's filter box (id, on),
// 9 expire interval (ms in id)
EMSCRIPTEN_KEEPALIVE int wsc_sniffer_do(int action, int id, int on)
{
    SnifferWindow *win = openWindow<SnifferWindow>();
    if (!win) return 0;
    switch (action)
    {
    case 0: click(win, "btNotch"); break;
    case 1: click(win, "btUnNotch"); break;
    case 2: click(win, "btAll"); break;
    case 3: click(win, "btNone"); break;
    case 4: setChecked(win, "cbFadeInactive", on); break;
    case 5: setChecked(win, "cbMuteNotched", on); break;
    case 6: setChecked(win, "cbNoExpire", on); break;
    case 7: setChecked(win, "cbViewBits", on); break;
    case 8: checkId(win->idList(), quint32(id), on); break;
    case 9: setSpin(win, "spinExpireInterval", id); break;
    default: return 0;
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE const char *wsc_flow_state() { return json(flowState()); }
EMSCRIPTEN_KEEPALIVE const char *wsc_flow_frames(int from, int n) { return json(flowFrames(from, n)); }
// 0 select ID a, 1 play, 2 reverse, 3 pause, 4 stop, 5 forward one, 6 back one, 7 go to frame a, 8 live mode (a),
// 9 seek value (byte a, value b; -1 none), 10 trigger bit a, 11 playback speed a ms, 12 loop (a), 13 auto reference (a),
// 14 chart range a, 15 graph by timestamp (a)
EMSCRIPTEN_KEEPALIVE int wsc_flow_do(int action, int a, int b)
{
    FlowViewWindow *win = openWindow<FlowViewWindow>();
    if (!win) return 0;
    switch (action)
    {
    case 0: win->selectId(quint32(a)); break;
    case 1: win->playForward(); break;
    case 2: win->playBackward(); break;
    case 3: win->pause(); break;
    case 4: win->stop(); break;
    case 5: win->stepForward(); break;
    case 6: win->stepBack(); break;
    case 7: if (auto *s = win->findChild<QSlider *>("timelineSlider")) s->setValue(a); break;  // as dragging it
    case 8: setChecked(win, "cbLiveMode", a); break;
    case 9: win->setTriggerValue(a, b); break;
    case 10: win->toggleTriggerBit(a); break;
    case 11: setSpin(win, "spinPlayback", a); break;
    case 12: setChecked(win, "cbLoopPlayback", a); break;
    case 13: setChecked(win, "cbAutoRef", a); break;
    case 14: if (auto *s = win->findChild<QSlider *>("graphRangeSlider")) s->setValue(a); break;
    case 15: setChecked(win, "cbTimeGraph", a); break;
    default: return 0;
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE const char *wsc_range_state() { return json(rangeStateJson()); }
// Sets the window's boxes, then presses Recalculate from the event loop (it lets the event loop run while it works).
// mode: Signal Mode index (0 big, 1 little, 2 both); sign: Signed Mode index (0 signed, 1 unsigned, 2 both).
EMSCRIPTEN_KEEPALIVE int wsc_range_run(int minBits, int maxBits, int granularity, int mode, int sign, int sensitivity)
{
    RangeStateWindow *win = openWindow<RangeStateWindow>();
    if (!win) return 0;
    setSpin(win, "spinMaxSigSize", 32);
    setSpin(win, "spinMinSigSize", minBits);
    setSpin(win, "spinMaxSigSize", maxBits);
    setSpin(win, "spinGranularity", granularity);
    if (auto *c = win->findChild<QComboBox *>("cbSignalMode")) c->setCurrentIndex(mode);
    if (auto *c = win->findChild<QComboBox *>("cbSignedMode")) c->setCurrentIndex(sign);
    if (auto *s = win->findChild<QSlider *>("slideSensitivity")) s->setValue(sensitivity);
    auto *b = win->findChild<QAbstractButton *>("btnRecalc");
    return b && QMetaObject::invokeMethod(b, "click", Qt::QueuedConnection) ? 1 : 0;
}
EMSCRIPTEN_KEEPALIVE int wsc_range_id(int id, int on)
{
    RangeStateWindow *win = openWindow<RangeStateWindow>();
    if (!win) return 0;
    if (id < 0) click(win, on ? "btnAllFilter" : "btnNoneFilter");
    else checkId(win->findChild<QListWidget *>("listFilter"), quint32(id), on);
    return 1;
}
EMSCRIPTEN_KEEPALIVE int wsc_range_select(int row)
{
    RangeStateWindow *win = openWindow<RangeStateWindow>();
    auto *list = win ? win->findChild<QListWidget *>("listCandidates") : nullptr;
    if (!list || row < 0 || row >= list->count()) return 0;
    list->setCurrentRow(row);
    return 1;
}

EMSCRIPTEN_KEEPALIVE const char *wsc_discrete_state() { return json(discreteJson()); }
EMSCRIPTEN_KEEPALIVE int wsc_discrete_id(int id, int on)
{
    DiscreteStateWindow *win = openWindow<DiscreteStateWindow>();
    if (!win) return 0;
    if (id < 0) click(win, on ? "btnAll" : "btnNone");
    else checkId(win->findChild<QListWidget *>("listID"), quint32(id), on);
    return 1;
}
// Sets the boxes and presses "Go for it" from the event loop.
EMSCRIPTEN_KEEPALIVE int wsc_discrete_run(int states, int realtime, int freq, int iterations, int minBits, int maxBits)
{
    DiscreteStateWindow *win = openWindow<DiscreteStateWindow>();
    if (!win) return 0;
    setSpin(win, "spinStates", states);
    click(win, realtime ? "rbRealtime" : "rbLogged");
    setSpin(win, "spinFreq", freq);
    setSpin(win, "spinIterations", iterations);
    setSpin(win, "spinMaxBits", 8);
    setSpin(win, "spinMinBits", minBits);
    setSpin(win, "spinMaxBits", maxBits);
    auto *b = win->findChild<QAbstractButton *>("btnStart");
    return b && QMetaObject::invokeMethod(b, "click", Qt::QueuedConnection) ? 1 : 0;
}
}

EM_JS(void, wsc_install_retools_hook, (), {
    const J = (p) => JSON.parse(UTF8ToString(p));
    Object.assign(window.webSavvyCAN, {
        sniffer: () => J(_wsc_sniffer_state()),
        snifferDo: (action, id = 0, on = true) => _wsc_sniffer_do(
            { notch: 0, unnotch: 1, all: 2, none: 3, fade: 4, mute: 5, noExpire: 6, viewBits: 7, filter: 8, expire: 9 }[action] ?? -1,
            id | 0, on ? 1 : 0) === 1,
        flowView: () => J(_wsc_flow_state()),
        flowViewFrames: (from, n) => J(_wsc_flow_frames(from | 0, n | 0)),
        flowViewDo: (action, a = 0, b = 0) => _wsc_flow_do(
            { select: 0, play: 1, reverse: 2, pause: 3, stop: 4, forward: 5, back: 6, goto: 7, live: 8, seek: 9, triggerBit: 10,
              speed: 11, loop: 12, autoRef: 13, range: 14, byTime: 15 }[action] ?? -1,
            typeof a === 'boolean' ? (a ? 1 : 0) : a | 0, b | 0) === 1,
        rangeState: () => J(_wsc_range_state()),
        rangeStateIds: (ids, on = true) => ids === 'all' || ids === 'none' ? _wsc_range_id(-1, ids === 'all' ? 1 : 0) === 1
            : ids.every((id) => _wsc_range_id(id | 0, on ? 1 : 0) === 1),
        rangeStateRun: ({ minBits = 8, maxBits = 16, granularity = 1, mode = 2, sign = 2, sensitivity = 50 } = {}) =>
            _wsc_range_run(minBits, maxBits, granularity, mode, sign, sensitivity) === 1,
        rangeStateSelect: (row) => _wsc_range_select(row | 0) === 1,
        discreteState: () => J(_wsc_discrete_state()),
        discreteStateIds: (ids, on = true) => ids === 'all' || ids === 'none' ? _wsc_discrete_id(-1, ids === 'all' ? 1 : 0) === 1
            : ids.every((id) => _wsc_discrete_id(id | 0, on ? 1 : 0) === 1),
        discreteStateRun: ({ states = 2, realtime = true, freq = 2, iterations = 1, minBits = 1, maxBits = 8 } = {}) =>
            _wsc_discrete_run(states, realtime ? 1 : 0, freq, iterations, minBits, maxBits) === 1,
    });
});

void TestHook::installReTools()
{
    wsc_install_retools_hook();
}
