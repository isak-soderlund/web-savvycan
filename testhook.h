#ifndef TESTHOOK_H
#define TESTHOOK_H

// web-savvycan: window.webSavvyCAN = { version, stats(), theme(), traceTail(n) } for the browser tests
// (tests/web/README.md). WebAssembly build only.

class CommFrameModel;

namespace TestHook {
void install(CommFrameModel *model);   // once, after the main window exists
void installReTools();                 // the live RE tools' hooks (testhook_retools.cpp), from install()
}

#endif // TESTHOOK_H
