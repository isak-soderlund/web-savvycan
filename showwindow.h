#ifndef SHOWWINDOW_H
#define SHOWWINDOW_H

// web-savvycan: shows a window and brings it to the front. In the browser the windows are tabs of the main window
// (wasm/windowtabs.h, owner 2026-10-30): an open one is switched to (a show() alone does nothing to a window that is
// open in a tab behind another). The desktop keeps SavvyCAN's show().

#include <QWidget>
#ifdef Q_OS_WASM
#include "wasm/windowtabs.h"
#endif

inline void showWindow(QWidget *w)
{
#ifdef Q_OS_WASM
    WindowTabs::present(w);
#else
    w->show();
#endif
}

#endif // SHOWWINDOW_H
