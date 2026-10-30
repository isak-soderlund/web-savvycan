#ifndef WINDOWTABS_H
#define WINDOWTABS_H

// web-savvycan, browser build only (owner 2026-10-30, RESOLVE.md "Tabs instead of windows"): every non-modal window
// the app opens (the tools, the connection window, Preferences, Help...) becomes a tab of the main window instead of a
// floating window. A page has no window manager: a floating window got buried behind the main window, which fills the
// page, with no taskbar to get it back.
//
// - One place: an application event filter catches every top-level window when it is shown. Non-modal ones are moved
//   into the main window (a tab, filling the page below the menu bar row); modal dialogs (exec(), open(), message
//   boxes, progress dialogs, file dialogs) stay dialogs over the current tab. Windows added later get tabs without
//   code of their own.
// - The tab bar is in the menu bar row, after the menus. The first tab is the main screen (the frames), not closable.
//   A tool's tab has a close button: it closes the window as SavvyCAN's close does (closeEvent, settings written).
// - A tab in the background is only covered (the tools stay "visible" to Qt, as a window behind another one on the
//   desktop): live graphs keep taking frames, and a tool's showEvent doesn't run again on every switch.
// - Opening a tool that is open (MainWindow's show... functions, through present()) switches to its tab.
// - The page's own text in the row (the website's bar) and the tabs share the row: the app publishes where the tabs
//   start and how wide they would be (window.savvycanTopBar, event "savvycan-topbar"); the page decides which of its
//   texts fit and tells the app how much of the row's right end they take (savvycanTopBar.setReserve(px)). The tabs
//   get the rest, and shrink (elided titles) only past that.

#include <QJsonObject>
#include <QList>

class QMainWindow;
class QWidget;

namespace WindowTabs {
void install(QMainWindow *main);     // once, after the main window's UI is set up
// Shows a window and brings it to the front: its tab (MainWindow's show... functions call it).
void present(QWidget *window);
// For the tests (testhook.cpp): every window but the main one (in a tab, or a top-level), whether one is open (has
// its tab, or is a visible top-level), the tab bar's state, and a tab selected or closed as a click would.
QList<QWidget *> windows();
bool isOpen(QWidget *window);
QJsonObject state();
bool selectTab(int index);
bool closeTab(int index);
}

#endif // WINDOWTABS_H
