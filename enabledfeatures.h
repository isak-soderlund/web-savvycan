#ifndef ENABLEDFEATURES_H
#define ENABLEDFEATURES_H

/*
 * web-savvycan: the one list of which parts of SavvyCAN are switched on.
 *
 * The fork is rolled out stage by stage (resolve-can docs/savvycan-fork-plan.md): stage 1 has the
 * connection window, the main window's trace, logging, Preferences and About. Every other menu
 * entry, button, panel and window is hidden (not broken) until its stage turns it on here, on the
 * desktop and in the browser alike.
 *
 * To switch a feature on: set its line in on() below to true. MainWindow::applyFeatures()
 * hides/disables everything tied to a feature that is off (menu actions, so their shortcuts are dead
 * too; panels; Preferences groups; context-menu entries), and the show...() slots refuse to open a
 * window whose feature is off, so nothing is reachable another way either.
 */

namespace Features {

enum class Feature {
    // ---- stage 1 (MVP)
    Connection,       // Connection > Open Connection Window
    Trace,            // main window frame list, its filters, overwrite / changing-only modes
    Logging,          // File > Save Log File, Save Filtered, continuous logging
    Preferences,      // File > Preferences
    About,            // Help > About
    ThemeSwitch,      // View > Dark Theme, Preferences > Theme
    // ---- stage 2
    Graphs,           // RE Tools > New Graphing Window, trace context menu "Add to graph", OpenGL setting
    // ---- stage 3
    DBC,              // File > DBC File Manager, Save Decoded Frames (+CSV), Interpret Frames, DBC settings
    SignalViewer,     // RE Tools > Signal Viewer (DBC signals)
    // ---- stage 4: reverse-engineering tools, one at a time
    Sniffer,
    FrameInfo,        // RE Tools > Frame Data Analysis
    TemporalGraph,
    FlowView,
    RangeState,
    DiscreteState,    // RE Tools > Single/Multi State
    Bisector,         // RE Tools > Capture Bisector
    FileComparator,
    DBCComparator,
    FrameSearch,      // RE Tools > Frame Search
    // ---- stage 5: sending
    FrameSender,      // Send Frames > Custom, the main window's simple send panel
    Playback,
    Fuzzing,
    Scripting,
    CANBridge,
    // ---- stage 6: protocols
    ISOTP,
    UDSScanner,
    UDSFirmwareUpdate,
    // ---- stage 7: files
    LoadLog,          // File > Load Log File (Ctrl+O), dropping a file on the window
    FilterFiles,      // File > Load / Save Filter Definition
    CropLog,          // File > Crop Log to Frame Range
    // ---- excluded (EVTV hardware)
    MotorControlConfig,
};

constexpr bool on(Feature f)
{
    switch (f) {
    case Feature::Connection:         return true;
    case Feature::Trace:              return true;
    case Feature::Logging:            return true;
    case Feature::Preferences:        return true;
    case Feature::About:              return true;
    case Feature::ThemeSwitch:        return true;

    case Feature::Graphs:             return true;  // stage 2 (started 2026-10-29)
    case Feature::DBC:                return false; // stage 3
    case Feature::SignalViewer:       return false; // stage 3
    case Feature::Sniffer:            return false; // stage 4
    case Feature::FrameInfo:          return true;  // stage 4, brought forward: the heatmap tool with CAN FD (owner 2026-10-29)
    case Feature::TemporalGraph:      return true;  // stage 4, brought forward (owner 2026-10-29)
    case Feature::FlowView:           return false; // stage 4
    case Feature::RangeState:         return false; // stage 4
    case Feature::DiscreteState:      return false; // stage 4
    case Feature::Bisector:           return false; // stage 4
    case Feature::FileComparator:     return false; // stage 4
    case Feature::DBCComparator:      return false; // stage 4
    case Feature::FrameSearch:        return false; // stage 4
    case Feature::FrameSender:        return false; // stage 5
    case Feature::Playback:           return false; // stage 5
    case Feature::Fuzzing:            return false; // stage 5
    case Feature::Scripting:          return false; // stage 5
    case Feature::CANBridge:          return false; // stage 5
    case Feature::ISOTP:              return false; // stage 6
    case Feature::UDSScanner:         return false; // stage 6
    case Feature::UDSFirmwareUpdate:  return false; // stage 6
    case Feature::LoadLog:            return true;  // stage 7, brought forward: a log file to graph (owner 2026-10-29)
    case Feature::FilterFiles:        return false; // stage 7
    case Feature::CropLog:            return false; // stage 7
    case Feature::MotorControlConfig: return false; // excluded
    }
    return false;
}

} // namespace Features

#endif // ENABLEDFEATURES_H
