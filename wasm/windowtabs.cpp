#include "windowtabs.h"
#include "theme.h"

#include <QAbstractButton>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QColorDialog>
#include <QDialog>
#include <QErrorMessage>
#include <QFileDialog>
#include <QFontDialog>
#include <QHash>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProgressDialog>
#include <QScrollArea>
#include <QStackedLayout>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#include <QWizard>

#include <algorithm>
#include <emscripten.h>

// The row: the menu bar, kRowHeight high (the page's bar takes its height from savvycanTopBar.height). The tabs fill
// it from kTabTop down and join the content below; the menus' text is centred on the tabs' text.
static constexpr int kRowHeight = 34;
static constexpr int kTabTop = 5;
static constexpr int kTabsGap = 17;      // after the last menu (a divider in the middle)
static constexpr int kTabMax = 220;      // a tab's width with room to spare (Chrome's are ~240)
static constexpr int kTabMin = 76;       // the smallest a tool tab gets before the bar scrolls (an elided title)

// The window's own state on the page (window.savvycanTopBar) and its "savvycan-topbar" event, read by the website's
// bar; setReserve(px) is the page's answer: the width its texts take at the row's right end.
EM_JS(void, wsc_topbar_publish, (int height, int tabTop, int left, int natural, int right), {
    const bar = window.savvycanTopBar || (window.savvycanTopBar = {
        setReserve: (px) => _wsc_topbar_set_reserve(Math.max(0, px | 0)),
    });
    Object.assign(bar, { height, tabTop, tabsLeft: left, tabsNatural: natural, tabsRight: right });
    window.dispatchEvent(new CustomEvent('savvycan-topbar', { detail: { height, tabTop, tabsLeft: left, tabsNatural: natural, tabsRight: right } }));
});

namespace {

// A window in a tab, for accessibility: a plain pane named by its title. A QDialog is a "dialog" there, which Qt's
// browser accessibility makes an HTML <dialog> element: shown as a window that is fine, inside the main window it is a
// closed <dialog> and everything in it is out of the accessibility tree (no screen reader, no test by role).
QAccessibleInterface *tabAccessible(const QString &, QObject *object)
{
    QWidget *w = qobject_cast<QWidget *>(object);
    if (w && w->property("webSavvyCanTab").toBool())
        return new QAccessibleWidget(w, QAccessible::Pane, w->windowTitle());
    return nullptr;
}

struct Colors
{
    QColor strip, active, hover, text, textActive, separator, closeHover;
};

// From the theme's palette: the active tab is the window colour (it joins the content), the strip a shade darker.
Colors colors(const QPalette &pal)
{
    const bool dark = Theme::isDark();
    Colors c;
    c.active = pal.color(QPalette::Window);
    c.strip = dark ? c.active.darker(140) : c.active.darker(109);
    c.hover = QColor((c.strip.red() + c.active.red()) / 2, (c.strip.green() + c.active.green()) / 2,
                     (c.strip.blue() + c.active.blue()) / 2);
    c.textActive = pal.color(QPalette::WindowText);
    c.text = c.textActive;
    c.text.setAlpha(dark ? 165 : 175);
    c.separator = dark ? c.strip.lighter(165) : c.strip.darker(118);
    c.closeHover = dark ? QColor(255, 255, 255, 36) : QColor(0, 0, 0, 26);
    return c;
}

// A tab's name: the window's title, or the name of its menu entry where the title says less (short names fit more
// tabs). A window may set its own with the property "webSavvyCanTabTitle".
QString tabName(const QWidget *w)
{
    static const QHash<QString, QString> names = {
        {QStringLiteral("GraphingWindow"), QApplication::translate("WindowTabs", "Graph")},
        {QStringLiteral("FrameInfoWindow"), QApplication::translate("WindowTabs", "Frame Data Analysis")},
        {QStringLiteral("ConnectionWindow"), QApplication::translate("WindowTabs", "Connection")},
        {QStringLiteral("MainSettingsDialog"), QApplication::translate("WindowTabs", "Preferences")},
    };
    const QString own = w->property("webSavvyCanTabTitle").toString();
    if (!own.isEmpty()) return own;
    const QString named = names.value(QString::fromLatin1(w->metaObject()->className()));
    return named.isEmpty() ? w->windowTitle().remove(QStringLiteral("[*]")).trimmed() : named;
}

QString css(const QColor &c)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
}

// A tab's ×: drawn only on the current tab and the tab under the mouse (it keeps its room on the others, so titles
// don't move), with a round hover background.
class CloseButton : public QAbstractButton
{
public:
    explicit CloseButton(QWidget *parent) : QAbstractButton(parent)
    {
        setFixedSize(18, 18);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::ArrowCursor);
        setToolTip(QApplication::translate("WindowTabs", "Close tab"));
        setAccessibleName(QApplication::translate("WindowTabs", "Close tab"));
        setAttribute(Qt::WA_Hover);
    }
    void setShown(bool on)
    {
        if (on != shown) { shown = on; update(); }
    }
    void setColors(const QColor &fg, const QColor &hover)
    {
        this->fg = fg;
        this->hoverBg = hover;
        update();
    }
    QSize sizeHint() const override { return {18, 18}; }
    bool shown = false;

protected:
    void paintEvent(QPaintEvent *) override
    {
        const bool hot = underMouse();
        if (!shown && !hot) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (hot) {
            p.setPen(Qt::NoPen);
            p.setBrush(hoverBg);
            p.drawEllipse(rect().adjusted(1, 1, -1, -1));
        }
        p.setPen(QPen(fg, 1.5, Qt::SolidLine, Qt::RoundCap));
        const QRectF r = QRectF(rect()).adjusted(5.5, 5.5, -5.5, -5.5);
        p.drawLine(r.topLeft(), r.bottomRight());
        p.drawLine(r.topRight(), r.bottomLeft());
    }
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    QColor fg, hoverBg;
};

class Host;

// The tab bar: widths between kTabMin and kTabMax, Chrome's thin separators between tabs that aren't current or
// hovered, the hovered tab tracked for the close buttons.
class TabStrip : public QTabBar
{
public:
    TabStrip(Host *host, QWidget *parent) : QTabBar(parent), host(host)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
    }
    int hovered = -1;
    QColor separator;
    int limit = 0;   // crowded: every tool tab this wide (as Chrome's, all equal), 0 = each its own width
    int naturalWidth() const
    {
        int w = 0;
        for (int i = 0; i < count(); i++) w += ownWidth(i);
        return w;
    }
    // a tab's width with room to spare: its title, between 120 and kTabMax (the main screen's tab: its title)
    int ownWidth(int index) const { return qBound(index == 0 ? 72 : 120, QTabBar::tabSizeHint(index).width(), kTabMax); }
    // the tabs in `room`: tool tabs of equal width when they don't fit at their own, at least kTabMin (then it scrolls)
    void fit(int room)
    {
        int l = 0;
        if (count() > 1 && naturalWidth() > room)
            l = qMax(kTabMin, (room - ownWidth(0)) / (count() - 1));
        if (l != limit) {
            limit = l;
            // the tabs' sizes again (QTabBar caches them until a tab changes)
            setIconSize(iconSize() + QSize(1, 0));
            setIconSize(iconSize() - QSize(1, 0));
        }
    }

protected:
    QSize tabSizeHint(int index) const override
    {
        const int w = index > 0 && limit > 0 ? limit : ownWidth(index);
        return {w, kRowHeight - kTabTop};
    }
    QSize minimumTabSizeHint(int index) const override { return tabSizeHint(index); }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        QTabBar::mouseMoveEvent(e);
        setHovered(tabAt(e->position().toPoint()));
    }
    void leaveEvent(QEvent *e) override
    {
        QTabBar::leaveEvent(e);
        setHovered(-1);
    }
    void paintEvent(QPaintEvent *e) override;
    void setHovered(int index);

private:
    Host *host;
};

class Host : public QObject
{
public:
    explicit Host(QMainWindow *main);

    // A window in the main window: its scroll area (the page in the stack, for a tool larger than the page).
    struct Page
    {
        QWidget *tool = nullptr;
        QScrollArea *page = nullptr;
        CloseButton *close = nullptr;
        int number = 1;        // "Data Graphing 2": the second open window of that title
        bool open = false;     // has a tab
    };

    QMainWindow *main;
    QMenuBar *menu;
    TabStrip *bar;
    QWidget *divider;          // between the menus and the tabs
    QStackedLayout *stack;
    QWidget *mainPage;
    QList<Page *> pages;       // every window moved into the main window, open or closed (kept for a reopen)
    QList<Page *> tabs;        // tab i + 1 (tab 0 is the main screen)
    bool busy = false;         // the host itself shows, hides or moves a window
    QWidget *adopting = nullptr;
    int reserve = 0;           // the page's texts at the row's right end
    int published[5] = {-1, -1, -1, -1, -1};

    Page *pageOf(QObject *o) const
    {
        for (Page *p : pages)
            if (p->tool == o) return p;
        return nullptr;
    }
    int tabOf(const Page *p) const { return p && p->open ? int(tabs.indexOf(const_cast<Page *>(p))) + 1 : -1; }

    bool eventFilter(QObject *o, QEvent *e) override;
    bool isCandidate(QWidget *w) const;
    void adopt(QWidget *w);
    void openTab(Page *p);
    void closedByApp(Page *p);
    void destroyed(QObject *o);
    void select(int index);
    bool closeTab(int index);
    QString title(const Page *p) const;
    void retitle(Page *p);
    void restyle();
    void layoutBar();
    void updateCloseButtons();
};

Host *g_host = nullptr;

void TabStrip::setHovered(int index)
{
    if (index == hovered) return;
    hovered = index;
    host->updateCloseButtons();
    update();
}

void TabStrip::paintEvent(QPaintEvent *e)
{
    QTabBar::paintEvent(e);
    QPainter p(this);
    p.setPen(QPen(separator, 1));
    const int cur = currentIndex();
    for (int i = 0; i + 1 < count(); i++) {
        if (i == cur || i + 1 == cur || i == hovered || i + 1 == hovered) continue;
        const QRect r = tabRect(i);
        if (!r.isValid() || r.right() > width()) continue;
        p.drawLine(r.right(), r.top() + 8, r.right(), r.bottom() - 6);
    }
}

Host::Host(QMainWindow *main) : QObject(main), main(main)
{
    menu = qobject_cast<QMenuBar *>(main->menuWidget());
    mainPage = main->takeCentralWidget();
    QWidget *area = new QWidget(main);
    area->setObjectName(QStringLiteral("webSavvyCanTabArea"));
    stack = new QStackedLayout(area);
    stack->setStackingMode(QStackedLayout::StackAll);   // a background tab is covered, not hidden (windowtabs.h)
    stack->addWidget(mainPage);
    main->setCentralWidget(area);

    divider = new QWidget(menu);
    divider->setAutoFillBackground(true);
    bar = new TabStrip(this, menu);
    bar->setObjectName(QStringLiteral("webSavvyCanTabs"));
    bar->setDrawBase(false);
    bar->setExpanding(false);
    bar->setDocumentMode(true);
    bar->setElideMode(Qt::ElideRight);
    bar->setUsesScrollButtons(true);
    bar->setFocusPolicy(Qt::NoFocus);
    bar->addTab(QApplication::translate("WindowTabs", "Frames"));
    bar->setTabToolTip(0, QApplication::translate("WindowTabs", "SavvyCAN's main screen: the frames"));
    connect(bar, &QTabBar::currentChanged, this, [this](int i) {
        if (!busy) select(i);
    });
    menu->setFixedHeight(kRowHeight);
    menu->installEventFilter(this);
    qApp->installEventFilter(this);
    QAccessible::installFactory(tabAccessible);
    connect(Theme::instance(), &Theme::changed, this, [this] { restyle(); });
    restyle();
    bar->show();
    divider->show();
    QTimer::singleShot(0, this, [this] { layoutBar(); });
}

// A window that gets a tab: a non-modal top-level window or dialog. Modal dialogs, message boxes, progress dialogs and
// Qt's standard dialogs are short questions: they stay dialogs over the current tab.
bool Host::isCandidate(QWidget *w) const
{
    if (w == main || !w->isWindow() || pageOf(w)) return false;
    const Qt::WindowType type = w->windowType();
    if (type != Qt::Window && type != Qt::Dialog) return false;
    if (w->isModal() || w->testAttribute(Qt::WA_DontShowOnScreen)) return false;
    if (qobject_cast<QMessageBox *>(w) || qobject_cast<QProgressDialog *>(w) || qobject_cast<QFileDialog *>(w)
        || qobject_cast<QInputDialog *>(w) || qobject_cast<QColorDialog *>(w) || qobject_cast<QFontDialog *>(w)
        || qobject_cast<QErrorMessage *>(w) || qobject_cast<QWizard *>(w))
        return false;
    return !w->property("webSavvyCanFloating").toBool();   // a window may opt out
}

bool Host::eventFilter(QObject *o, QEvent *e)
{
    switch (e->type()) {
    case QEvent::Show:
        if (o == adopting) return true;   // its showEvent ran when it was shown; not again while it moves in
        if (o->isWidgetType()) {
            QWidget *w = static_cast<QWidget *>(o);
            if (w->isWindow() && isCandidate(w)) {
                // Moved in from the event loop (not inside its own show()); unseen until then.
                if (QWindow *h = w->windowHandle()) h->setOpacity(0);
                QPointer<QWidget> p(w);
                QTimer::singleShot(0, this, [this, p] { if (p) adopt(p); });
            }
        }
        break;
    case QEvent::Hide:
        if (o == adopting) return true;
        break;
    case QEvent::ShowToParent:   // the app shows a window whose tab was closed: a tab again
        if (!busy)
            if (Page *p = pageOf(o); p && !p->open) {
                QPointer<QWidget> tool(p->tool);
                QTimer::singleShot(0, this, [this, tool] {
                    if (Page *q = tool ? pageOf(tool) : nullptr; q && !q->open && !tool->isHidden()) openTab(q);
                });
            }
        break;
    case QEvent::HideToParent:   // the app closed or hid it (its own close button, Escape in SavvyCAN, done())
        if (!busy)
            if (Page *p = pageOf(o); p && p->open) closedByApp(p);
        break;
    case QEvent::WindowTitleChange:
        if (Page *p = pageOf(o)) retitle(p);
        break;
    case QEvent::KeyPress:
        // Escape would close a dialog-based tool (QDialog::reject): a stray key shouldn't close a tab.
        if (static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape && pageOf(o)) return true;
        break;
    case QEvent::Resize:
    case QEvent::LayoutRequest:
        if (o == menu) layoutBar();
        break;
    default:
        break;
    }
    return QObject::eventFilter(o, e);
}

void Host::adopt(QWidget *w)
{
    if (!w->isWindow() || !w->isVisible() || w->isModal() || pageOf(w)) {
        if (QWindow *h = w->windowHandle()) h->setOpacity(1);
        return;
    }
    Page *p = new Page;
    p->tool = w;
    p->page = new QScrollArea;
    p->page->setFrameShape(QFrame::NoFrame);
    p->page->setWidgetResizable(true);
    p->page->setBackgroundRole(QPalette::Window);
    p->page->viewport()->setBackgroundRole(QPalette::Window);
    p->page->viewport()->setAutoFillBackground(true);
    pages.append(p);

    // its accessible interface again, as a pane (tabAccessible): the old one (a dialog window) goes first
    w->setProperty("webSavvyCanTab", true);
    if (QAccessible::isActive()) {
        if (QAccessibleInterface *old = QAccessible::queryAccessibleInterface(w)) {
            QAccessibleEvent gone(w, QAccessible::ObjectDestroyed);
            QAccessible::updateAccessibility(&gone);
            QAccessible::deleteAccessibleInterface(QAccessible::uniqueId(old));
        }
    }

    busy = true;
    adopting = w;
    w->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);   // a window of a fixed size fills its tab
    w->setAutoFillBackground(true);
    w->setBackgroundRole(QPalette::Window);
    if (auto *d = qobject_cast<QDialog *>(w)) d->setSizeGripEnabled(false);   // no window corner to drag
    p->page->setWidget(w);   // a child now: no frame or title bar of its own (its platform window is gone)
    stack->addWidget(p->page);
    p->page->show();
    w->show();
    adopting = nullptr;
    busy = false;
    connect(w, &QObject::destroyed, this, [this](QObject *o) { destroyed(o); });
    openTab(p);
}

void Host::openTab(Page *p)
{
    // its number among the open windows of the same title
    const QString base = tabName(p->tool);
    QList<int> used;
    for (Page *t : tabs)
        if (tabName(t->tool) == base) used.append(t->number);
    p->number = 1;
    while (used.contains(p->number)) p->number++;

    busy = true;
    p->page->show();
    p->open = true;
    tabs.append(p);
    const int i = bar->addTab(title(p));
    bar->setTabToolTip(i, p->tool->windowTitle().remove(QStringLiteral("[*]")).trimmed());
    p->close = new CloseButton(bar);
    QObject::connect(p->close, &QAbstractButton::clicked, this, [this, p] { closeTab(tabOf(p)); });
    bar->setTabButton(i, QTabBar::RightSide, p->close);
    busy = false;
    restyle();
    select(i);
    layoutBar();
}

// The app closed the window itself: its tab goes (the window stays here, hidden, for a reopen).
void Host::closedByApp(Page *p)
{
    const int i = tabOf(p);
    const bool wasCurrent = i == bar->currentIndex();
    busy = true;
    p->open = false;
    tabs.removeOne(p);
    bar->removeTab(i);   // the close button goes with it
    p->close = nullptr;
    p->page->hide();
    busy = false;
    // Chrome's choice: the tab on the right, else the one on the left
    if (wasCurrent) select(qMin(i, bar->count() - 1));
    else updateCloseButtons();
    layoutBar();
}

void Host::destroyed(QObject *o)
{
    for (int k = 0; k < pages.size(); k++) {
        Page *p = pages[k];
        if (p->tool != o) continue;
        if (p->open) {
            const int i = tabOf(p);
            const bool wasCurrent = i == bar->currentIndex();
            busy = true;
            tabs.removeOne(p);
            bar->removeTab(i);
            busy = false;
            if (wasCurrent) select(qMin(i, bar->count() - 1));
        }
        p->page->takeWidget();   // being deleted already
        p->page->deleteLater();
        pages.removeAt(k);
        delete p;
        layoutBar();
        return;
    }
}

void Host::select(int index)
{
    if (index < 0 || index >= bar->count()) return;
    busy = true;
    bar->setCurrentIndex(index);
    QWidget *target = index == 0 ? mainPage : tabs[index - 1]->page;
    stack->setCurrentWidget(target);   // raised above the other pages
    busy = false;
    QWidget *focusOn = index == 0 ? mainPage : tabs[index - 1]->tool;
    if (QWidget *f = focusOn->focusWidget(); f && f->isVisible()) f->setFocus(Qt::TabFocusReason);
    else focusOn->setFocus(Qt::TabFocusReason);
    if (index > 0 && !focusOn->hasFocus()) focusOn->nextInFocusChain()->setFocus(Qt::TabFocusReason);
    updateCloseButtons();
    bar->update();
}

// As SavvyCAN's close: the window's closeEvent decides (it may ask first); closedByApp follows from its hide.
bool Host::closeTab(int index)
{
    if (index < 1 || index >= bar->count()) return false;
    Page *p = tabs[index - 1];
    QPointer<QWidget> tool(p->tool);
    const bool closed = tool->close();
    if (closed && tool && p->open) closedByApp(p);   // closed without a hide (a window that was hidden already)
    return closed;
}

QString Host::title(const Page *p) const
{
    QString t = tabName(p->tool);
    if (t.isEmpty()) t = QApplication::translate("WindowTabs", "Window");
    return p->number > 1 ? QStringLiteral("%1 %2").arg(t).arg(p->number) : t;
}

void Host::retitle(Page *p)
{
    if (const int i = tabOf(p); i > 0) {
        bar->setTabText(i, title(p));
        bar->setTabToolTip(i, p->tool->windowTitle().remove(QStringLiteral("[*]")).trimmed());
        layoutBar();
    }
}

void Host::restyle()
{
    const Colors c = colors(main->palette());
    // the menus as high as the tabs (an item taller than the row's inside would go into the ">>" extension)
    const int itemPadding = qMax(1, (kRowHeight - kTabTop - menu->fontMetrics().height()) / 2);
    menu->setStyleSheet(QStringLiteral(
        "QMenuBar { background: %1; padding: %2px 0 0 4px; border: none; }"
        "QMenuBar::item { background: transparent; color: %3; padding: %5px 9px; border-radius: 6px; }"
        "QMenuBar::item:selected, QMenuBar::item:pressed { background: %4; }")
                            .arg(css(c.strip)).arg(kTabTop).arg(css(c.textActive), css(c.hover))
                            .arg(itemPadding));
    bar->setStyleSheet(QStringLiteral(
        "QTabBar { background: transparent; border: none; }"
        "QTabBar::tab { background: transparent; color: %1; border: none; margin: 0 1px 0 0; padding: 0 6px 0 12px;"
        " border-top-left-radius: 9px; border-top-right-radius: 9px; }"
        "QTabBar::tab:first { padding: 0 14px; }"
        "QTabBar::tab:hover { background: %2; color: %3; }"
        "QTabBar::tab:selected { background: %4; color: %3; }"
        "QTabBar QToolButton { background: %5; border: none; border-radius: 6px; margin: 4px 1px; }"
        "QTabBar QToolButton:hover { background: %2; }"
        "QTabBar::scroller { width: 26px; }"
        "QTabBar::tear { image: none; width: 0; border: none; }")
                           .arg(css(c.text), css(c.hover), css(c.textActive), css(c.active), css(c.strip)));
    bar->separator = c.separator;
    QPalette dp = divider->palette();
    dp.setColor(QPalette::Window, c.separator);
    divider->setPalette(dp);
    for (Page *p : tabs)
        if (p->close) p->close->setColors(c.textActive, c.closeHover);
    bar->update();
}

void Host::updateCloseButtons()
{
    for (int i = 1; i < bar->count(); i++)
        if (auto *b = static_cast<CloseButton *>(bar->tabButton(i, QTabBar::RightSide)))
            b->setShown(i == bar->currentIndex() || i == bar->hovered);
}

// The bar after the menus, as wide as its tabs want, at most up to the page's texts (reserve).
void Host::layoutBar()
{
    int left = 4;
    for (QAction *a : menu->actions()) {
        if (!a->isVisible()) continue;
        const QRect r = menu->actionGeometry(a);
        if (r.isValid() && !r.isEmpty()) left = qMax(left, r.right() + 1);
    }
    left += kTabsGap;
    const int natural = bar->naturalWidth();
    const int room = qMax(0, menu->width() - left - reserve - 8);
    bar->fit(room);
    const int width = qMin(bar->limit > 0 ? bar->ownWidth(0) + (bar->count() - 1) * bar->limit : natural, room);
    bar->setGeometry(left, kTabTop, width, kRowHeight - kTabTop);
    divider->setGeometry(left - kTabsGap / 2 - 1, kTabTop + 7, 1, kRowHeight - kTabTop - 12);
    const int now[5] = {menu->height(), kTabTop, left, natural, left + width};
    if (!std::equal(now, now + 5, published)) {
        std::copy(now, now + 5, published);
        wsc_topbar_publish(now[0], now[1], now[2], now[3], now[4]);
    }
}

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void wsc_topbar_set_reserve(int px)
{
    if (!g_host || g_host->reserve == px) return;
    g_host->reserve = px;
    g_host->layoutBar();
}

void WindowTabs::install(QMainWindow *main)
{
    if (!g_host && main && qobject_cast<QMenuBar *>(main->menuWidget())) g_host = new Host(main);
}

void WindowTabs::present(QWidget *window)
{
    if (!window) return;
    window->show();   // a closed tab comes back (ShowToParent); a new window gets its tab (Show)
    if (g_host)
        if (Host::Page *p = g_host->pageOf(window); p && p->open) g_host->select(g_host->tabOf(p));
    if (window->isWindow()) {
        window->raise();
        window->activateWindow();
    }
}

QList<QWidget *> WindowTabs::windows()
{
    QList<QWidget *> out;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (!g_host || w != g_host->main) out.append(w);
    if (g_host)
        for (Host::Page *p : g_host->pages) out.append(p->tool);
    return out;
}

bool WindowTabs::isOpen(QWidget *window)
{
    if (!window) return false;
    if (g_host)
        if (Host::Page *p = g_host->pageOf(window)) return p->open;
    return window->isVisible();
}

QJsonObject WindowTabs::state()
{
    QJsonObject o;
    if (!g_host) return o;
    Host *h = g_host;
    QJsonArray tabs;
    for (int i = 0; i < h->bar->count(); i++) {
        const QRect r = h->bar->tabRect(i).translated(h->bar->geometry().topLeft());
        QJsonObject t;
        t.insert("title", h->bar->tabText(i));
        t.insert("current", i == h->bar->currentIndex());
        t.insert("class", i == 0 ? QStringLiteral("MainWindow") : QString::fromLatin1(h->tabs[i - 1]->tool->metaObject()->className()));
        t.insert("closable", i > 0);
        t.insert("x", r.x());
        t.insert("y", r.y());
        t.insert("width", r.width());
        t.insert("height", r.height());
        t.insert("visible", r.right() <= h->bar->geometry().right() && r.left() >= h->bar->geometry().left());
        if (i > 0)
            if (auto *b = static_cast<CloseButton *>(h->bar->tabButton(i, QTabBar::RightSide)))
                t.insert("closeShown", b->shown);
        tabs.append(t);
    }
    o.insert("tabs", tabs);
    o.insert("current", h->bar->currentIndex());
    o.insert("rowHeight", h->menu->height());
    o.insert("barLeft", h->bar->x());
    o.insert("barWidth", h->bar->width());
    o.insert("natural", h->bar->naturalWidth());
    o.insert("reserve", h->reserve);
    o.insert("hosted", int(h->pages.size()));
    // the page shown: the current tab's page is on top of the stack and fills the area
    QWidget *top = h->bar->currentIndex() == 0 ? h->mainPage : h->tabs[h->bar->currentIndex() - 1]->page;
    o.insert("pageWidth", top->width());
    o.insert("pageHeight", top->height());
    const QWidgetList tops = QApplication::topLevelWidgets();
    o.insert("topLevels", int(std::count_if(tops.begin(), tops.end(), [](QWidget *w) { return w->isVisible(); })));
    return o;
}

bool WindowTabs::selectTab(int index)
{
    if (!g_host || index < 0 || index >= g_host->bar->count()) return false;
    g_host->select(index);
    return true;
}

bool WindowTabs::closeTab(int index)
{
    return g_host && g_host->closeTab(index);
}
