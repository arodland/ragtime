#ifndef RAGTIME_GEARSHIFT_DIALOG_H
#define RAGTIME_GEARSHIFT_DIALOG_H

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <wx/frame.h>
#include <wx/timer.h>

#include "ragtime/gearbox/Gearbox.h"

class wxButton;
class wxCheckBox;
class wxChoice;
class wxListCtrl;
class wxListEvent;
class wxStaticText;
class wxToggleButton;
class SubmodeMap;

// The band's noise from the rig's S-meter (RigControl), for the map's QRN
// line and its S-meter axis: whether the rig reports one, the noise in dB
// over S9 once read, and the receive filter's width the S-meter reads
// across, once read.
struct BandNoise
{
    bool reads = false;
    std::optional<double> dbOverS9;
    std::optional<double> filterHz;
};

// The gearshift popup (docs/GEARBOX.md): the Data2G Submode Map with the
// gears on it, Width and Path chips, Auto, and the gear list. It opens from
// the Gear button by Send. Every change goes straight to the Gearshift it
// was given, and onChange runs after each, so the buttons can follow.
// noise, when given, is asked once a second.
//
// It is a window of its own, so the main window stays in use beside it
// (Andrew, 2026-11-08). With onTop it stays above owner; otherwise it
// stacks as any other window. onClosed runs as it closes.
class GearshiftDialog : public wxFrame
{
public:
    GearshiftDialog(wxWindow* owner, bool onTop, ragtime::gearbox::Gearshift& shift,
                    std::function<void()> onChange, std::function<void()> onClosed,
                    std::function<BandNoise()> noise = nullptr);

    // The gearshift changed from outside (S/N, the 500 Hz switch, data2g-
    // host's modes): show it as it is now.
    void follow();

private:
    void refresh();
    void changed();
    void loadFile();
    void onGearSelected(wxListEvent& event);

    ragtime::gearbox::Gearshift& shift_;
    std::function<void()> onChange_;
    std::function<void()> onClosed_;
    std::function<BandNoise()> noise_;
    wxTimer noiseTimer_;
    bool refreshing_ = false;
    bool changing_ = false;   // while onChange_ runs: its own follow() is not needed

    std::array<wxToggleButton*, 4> widthChips_ {};
    wxStaticText* widthNote_ = nullptr;
    std::array<wxToggleButton*, 4> pathChips_ {};
    wxCheckBox* auto_ = nullptr;
    SubmodeMap* map_ = nullptr;
    wxStaticText* inGear_ = nullptr;
    wxStaticText* snr_ = nullptr;
    wxListCtrl* gears_ = nullptr;
    wxChoice* anyMode_ = nullptr;
    std::vector<std::string> anyModes_;  // anyMode_'s modes, by row
    wxStaticText* source_ = nullptr;
    wxButton* load_ = nullptr;  // hidden while data2g-host gives thresholds
};

#endif
