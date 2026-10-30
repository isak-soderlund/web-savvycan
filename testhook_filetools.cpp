// web-savvycan: the browser tests' hook for the file-based RE tools (stage 4: Capture Bisector, File Comparison, DBC
// Comparison, Frame Search), in its own file so it merges apart from the other windows' hooks (testhook.cpp calls
// TestHook::installFileTools()). WebAssembly build only. tests/web/README.md "The app's test hook".
//
// webSavvyCAN.bisect() / bisectSet({...}), fileCompare(), dbcCompare(), search() / searchSet({...}): below.

#include "testhook.h"
#include "wasm/windowtabs.h"
#include "bisectwindow.h"
#include "mainwindow.h"
#include "re/dbccomparatorwindow.h"
#include "re/filecomparatorwindow.h"
#include "re/searchwindow.h"
#include "candatagrid.h"
#include "dbc/dbchandler.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QTableView>
#include <QTimer>
#include <QTreeWidget>
#include <map>

#include <emscripten.h>

namespace {

const char *keep(const QJsonObject &o)
{
    static QByteArray buffer;
    buffer = QJsonDocument(o).toJson(QJsonDocument::Compact);
    return buffer.constData();
}

template <class W> W *window()
{
    for (QWidget *w : WindowTabs::windows())   // tabs of the main window in the browser (wasm/windowtabs.h)
        if (auto *t = qobject_cast<W *>(w); t && WindowTabs::isOpen(t)) return t;
    return nullptr;
}

double timeUs(const CommFrame &f) { return double(f.timeStamp().seconds() * 1000000LL + f.timeStamp().microSeconds()); }

// A tree as nested {t: text, c: [children]} (c left out for a leaf).
QJsonArray treeJson(QTreeWidgetItem *parent)
{
    QJsonArray out;
    for (int i = 0; i < parent->childCount(); i++) {
        QTreeWidgetItem *c = parent->child(i);
        QJsonObject n;
        n.insert("t", c->text(0));
        if (c->childCount()) n.insert("c", treeJson(c));
        out.append(n);
    }
    return out;
}

QJsonObject sideJson(const FileTools::CompareSide &s)
{
    QJsonObject o;
    o.insert("frames", double(s.frames));
    o.insert("ids", int(s.ids.size()));
    o.insert("memoryBytes", double(s.memoryBytes()));
    return o;
}

} // namespace

// The setters' argument: one object, handed over as JSON text in the page (window.__wscArg) and copied into the heap
// here (no malloc export needed from the page).
EM_JS(int, wsc_arg_length, (), { return lengthBytesUTF8(window.__wscArg || '{}') + 1; });
EM_JS(void, wsc_arg_copy, (char *out, int n), { stringToUTF8(window.__wscArg || '{}', out, n); });

namespace {
QJsonObject args()
{
    QByteArray buf(wsc_arg_length(), '\0');
    wsc_arg_copy(buf.data(), int(buf.size()));
    return QJsonDocument::fromJson(QByteArray(buf.constData())).object();
}

template <class W> W *child(QWidget *win, const char *name) { return win ? win->findChild<W *>(name) : nullptr; }

void setText(QWidget *win, const char *name, const QJsonValue &v)
{
    if (v.isUndefined()) return;
    if (auto *e = child<QLineEdit>(win, name)) {
        e->setText(v.isString() ? v.toString() : QString::number(v.toDouble()));
        emit e->editingFinished();
    }
}
void setCheck(QWidget *win, const char *name, const QJsonValue &v)
{
    if (v.isUndefined()) return;
    if (auto *b = child<QAbstractButton>(win, name)) b->setChecked(v.toBool());
}

} // namespace

// ---- Capture Bisector: the main list's frames, the split (frames, their first/last time, frames per ID), the ID lists
static QJsonObject bisectState()
{
    QJsonObject o;
    BisectWindow *win = window<BisectWindow>();
    o.insert("open", win != nullptr);
    if (!win) return o;
    if (auto *l = child<QLabel>(win, "labelMainListNum")) o.insert("mainLabel", l->text());
    if (auto *l = child<QLabel>(win, "labelSplitNum")) o.insert("splitLabel", l->text());
    QJsonArray ids;
    if (auto *cb = child<QComboBox>(win, "cbIDLower"))
        for (int i = 0; i < cb->count(); i++) ids.append(cb->itemText(i));
    o.insert("ids", ids);
    const FrameStore &split = win->split();
    o.insert("splitCount", double(split.size()));
    std::map<uint32_t, double> perId;
    double first = 0, last = 0;
    for (qsizetype i = 0; i < split.size(); i++) {
        const CommFrame f = split.atIndex(i);
        perId[f.frameId()]++;
        if (i == 0) first = timeUs(f);
        last = timeUs(f);
    }
    QJsonObject idCounts;
    for (const auto &[id, n] : perId) idCounts.insert(QString::number(id), n);
    o.insert("splitIds", idCounts);
    o.insert("splitFirstUs", first);
    o.insert("splitLastUs", last);
    o.insert("splitBytes", double(split.memoryBytes()));
    o.insert("calculateMs", win->lastCalculateMs());
    return o;
}

// Sets the window's controls as a user would: {mode: 'frame'|'percent'|'id'|'bus', lower: bool, frame, percent (0..100),
// lowId, highId (as the lists show them, e.g. "0x7E8"), bus}.
static int bisectSet(const QJsonObject &a)
{
    BisectWindow *win = window<BisectWindow>();
    if (!win) return 0;
    const QString mode = a.value("mode").toString();
    setCheck(win, mode == "percent" ? "rbPercentage" : mode == "id" ? "rbIDRange" : mode == "bus" ? "rbBusNum" : "rbFrameNumber", true);
    setCheck(win, a.value("lower").toBool(true) ? "rbLowerSection" : "rbUpperSection", true);
    setText(win, "editFrameNumber", a.value("frame"));     // its editingFinished moves the slider
    setText(win, "editPercentage", a.value("percent"));
    setText(win, "editBusNum", a.value("bus"));
    for (const char *key : {"lowId", "highId"}) {
        if (a.value(key).isUndefined()) continue;
        auto *cb = child<QComboBox>(win, key[0] == 'l' ? "cbIDLower" : "cbIDUpper");
        const int i = cb ? cb->findText(a.value(key).toString(), Qt::MatchFixedString) : -1;
        if (i < 0) return 0;
        cb->setCurrentIndex(i);
    }
    return 1;
}

// ---- File Comparison: both sides' summaries, the tree, the load and comparison times
static QJsonObject fileCompareState()
{
    QJsonObject o;
    FileComparatorWindow *win = window<FileComparatorWindow>();
    o.insert("open", win != nullptr);
    if (!win) return o;
    o.insert("interestedName", win->interestedName());
    o.insert("side1", sideJson(win->interestedSide()));
    o.insert("side2", sideJson(win->referenceSide()));
    if (auto *l = child<QLabel>(win, "lblRefFrames")) o.insert("refLabel", l->text());
    o.insert("loadMs", win->lastLoadMs());
    o.insert("compareMs", win->lastCompareMs());
    if (auto *tree = child<QTreeWidget>(win, "treeDetails")) o.insert("tree", treeJson(tree->invisibleRootItem()));
    return o;
}

// ---- DBC Comparison: both DBCs (file, messages, nodes), the tree
static QJsonObject dbcCompareState()
{
    QJsonObject o;
    DBCComparatorWindow *win = window<DBCComparatorWindow>();
    o.insert("open", win != nullptr);
    if (!win) return o;
    auto dbcJson = [](DBCFile *f) {
        QJsonObject d;
        d.insert("loaded", f != nullptr);
        if (f) {
            d.insert("file", f->getFilename());
            d.insert("messages", f->messageHandler->getCount());
            d.insert("nodes", int(f->dbc_nodes.size()));
        }
        return d;
    };
    o.insert("first", dbcJson(win->first()));
    o.insert("second", dbcJson(win->second()));
    if (auto *l = child<QLabel>(win, "lblFirstFile")) o.insert("firstLabel", l->text());
    if (auto *l = child<QLabel>(win, "lblSecondFile")) o.insert("secondLabel", l->text());
    o.insert("compareMs", win->lastCompareMs());
    if (auto *tree = child<QTreeWidget>(win, "treeDetails")) o.insert("tree", treeJson(tree->invisibleRootItem()));
    return o;
}

// ---- Frame Search: the matching rows (the first 5000), how many, the current one, its label, the bit grid's size,
// the row selected in the main window's trace
static QJsonObject searchState()
{
    QJsonObject o;
    SearchWindow *win = window<SearchWindow>();
    o.insert("open", win != nullptr);
    if (!win) return o;
    const QVector<int> &m = win->matches();
    QJsonArray rows;
    for (int i = 0; i < m.size() && i < 5000; i++) rows.append(m[i]);
    o.insert("count", int(m.size()));
    o.insert("rows", rows);
    o.insert("current", win->currentMatch());
    o.insert("searchMs", win->lastSearchMs());
    if (auto *l = child<QLabel>(win, "lblMatchInfo")) o.insert("label", l->text());
    if (auto *g = win->findChild<CANDataGrid *>("gridData")) o.insert("gridBytes", g->getBytesToDraw());
    for (const char *name : {"txtStartBit", "txtDataLen", "txtID", "txtScale", "txtBias"})
        if (auto *e = child<QLineEdit>(win, name)) o.insert(name, e->text());
    for (const char *name : {"cbUseID", "cbIntel", "cbSigned"})
        if (auto *c = child<QCheckBox>(win, name)) o.insert(name, c->isChecked());
    // the DBC signal pickers (node, message, signal boxes): their items, the current ones
    auto items = [](QComboBox *box) {
        QJsonArray a;
        for (int i = 0; box && i < box->count(); i++) a.append(box->itemText(i));
        return a;
    };
    QJsonObject dbc;
    for (auto [key, name] : {std::pair{"nodes", "cbNodes"}, {"messages", "cbMessages"}, {"signals", "cbSignals"}}) {
        auto *box = child<QComboBox>(win, name);
        dbc.insert(key, items(box));
        dbc.insert(QString(key) + "Current", box ? box->currentText() : QString());
    }
    o.insert("dbc", dbc);
    if (auto *view = MainWindow::getReference()->findChild<QTableView *>("canFramesView"))
        o.insert("selectedRow", view->currentIndex().isValid() ? view->currentIndex().row() : -1);
    return o;
}

// {id: '0x7E8' | null (any ID), len: n | null (any length), dir: 'both'|'rx'|'tx', startBit, bits, intel, signed, scale,
// bias, mode: 'none'|'any'|'specific', value: '0x5A' | '12.5'}, as typed into the window.
static int searchSet(const QJsonObject &a)
{
    SearchWindow *win = window<SearchWindow>();
    if (!win) return 0;
    if (a.contains("id")) {
        setCheck(win, "cbUseID", !a.value("id").isNull());
        if (!a.value("id").isNull()) setText(win, "txtID", a.value("id"));
    }
    if (a.contains("len")) {
        setCheck(win, "cbUseLen", !a.value("len").isNull());
        if (auto *s = child<QSpinBox>(win, "spinFrameLen"); s && !a.value("len").isNull()) s->setValue(a.value("len").toInt());
    }
    if (a.contains("dir")) {
        const QString d = a.value("dir").toString();
        setCheck(win, d == "rx" ? "rbDirRX" : d == "tx" ? "rbDirTX" : "rbDirBoth", true);
    }
    if (a.contains("mode")) {
        const QString m = a.value("mode").toString();
        setCheck(win, m == "any" ? "rbValueAny" : m == "specific" ? "rbValueSpecific" : "rbValueNone", true);
    }
    // {dbc: {node, message, signal}}: picked in the DBC boxes as clicked, then Copy Signal Parameters
    if (a.contains("dbc")) {
        const QJsonObject d = a.value("dbc").toObject();
        auto pick = [&](const char *name, const QString &text) {
            auto *box = child<QComboBox>(win, name);
            const int i = box ? box->findText(text) : -1;
            if (i < 0) return false;
            box->setCurrentIndex(i);
            return true;
        };
        if (!pick("cbNodes", d.value("node").toString()) || !pick("cbMessages", d.value("message").toString())
            || !pick("cbSignals", d.value("signal").toString()))
            return 0;
        if (auto *b = child<QAbstractButton>(win, "btnCopySignal")) b->click(); else return 0;
    }
    setText(win, "txtStartBit", a.value("startBit"));
    setText(win, "txtDataLen", a.value("bits"));
    setCheck(win, "cbIntel", a.value("intel"));
    setCheck(win, "cbSigned", a.value("signed"));
    setText(win, "txtScale", a.value("scale"));
    setText(win, "txtBias", a.value("bias"));
    setText(win, "txtValue", a.value("value"));
    // {run: true}: then Search, as its button (a click needs the window on top, which the browser tests can't always
    // get: see RESOLVE.md, raise() and the accessibility layer)
    if (a.value("run").toBool())
        if (auto *b = child<QAbstractButton>(win, "btnSearch"))
            QTimer::singleShot(0, b, &QAbstractButton::click); // from the event loop: the search may yield to the page
    return 1;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE const char *wsc_bisect_state() { return keep(bisectState()); }
EMSCRIPTEN_KEEPALIVE int wsc_bisect_set() { return bisectSet(args()); }
EMSCRIPTEN_KEEPALIVE const char *wsc_filecompare_state() { return keep(fileCompareState()); }
EMSCRIPTEN_KEEPALIVE const char *wsc_dbccompare_state() { return keep(dbcCompareState()); }
EMSCRIPTEN_KEEPALIVE const char *wsc_search_state() { return keep(searchState()); }
EMSCRIPTEN_KEEPALIVE int wsc_search_set() { return searchSet(args()); }
}

EM_JS(void, wsc_install_filetools_hook, (), {
    const call = (fn, o) => {
        window.__wscArg = JSON.stringify(o || {});
        try { return fn() === 1; } finally { delete window.__wscArg; }
    };
    Object.assign(window.webSavvyCAN, {
        bisect: () => JSON.parse(UTF8ToString(_wsc_bisect_state())),
        bisectSet: (o) => call(_wsc_bisect_set, o),
        fileCompare: () => JSON.parse(UTF8ToString(_wsc_filecompare_state())),
        dbcCompare: () => JSON.parse(UTF8ToString(_wsc_dbccompare_state())),
        search: () => JSON.parse(UTF8ToString(_wsc_search_state())),
        searchSet: (o) => call(_wsc_search_set, o),
    });
});

void TestHook::installFileTools()
{
    wsc_install_filetools_hook();
}
