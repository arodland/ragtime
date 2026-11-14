#include "GearshiftDialog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <wx/arrstr.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/graphics.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/tglbtn.h>

#include "AppIcon.h"
#include "DrawnIcon.h"
#include "Html.h"
#include "Thresholds.h"
#include "ragtime/radio/Noise.h"

using namespace ragtime::gearbox;

namespace
{

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

wxColour fromRgb(std::uint32_t rgb)
{
    return wxColour((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// a toward b by t, so greys follow the system's light or dark theme.
wxColour blend(const wxColour& a, const wxColour& b, double t)
{
    auto mix = [t](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(std::lround(x + (y - x) * t));
    };
    return wxColour(mix(a.Red(), b.Red()), mix(a.Green(), b.Green()), mix(a.Blue(), b.Blue()));
}

const wxColour ACCENT(0x1f, 0x5f, 0xbf);      // the current gear, as the mock-ups

constexpr double PI = 3.14159265358979323846;
constexpr double MIN_BPS = 20.0;
constexpr double MAX_BPS = 10000.0;
constexpr double DOT_RADIUS = 4.5;
constexpr double KNOB_RADIUS = 9.5;       // half a gear knob's height
constexpr double KNOB_HALF_WIDTH = 19.0;  // wide enough for a short name, "E2C"
constexpr double S_AXIS_WIDTH = 64.0;     // the S-meter axis, right of the plot

// The modulations in the map's legend, in order.
const std::array<Modulation, 6> LEGEND_KINDS = {Modulation::Fsk,   Modulation::Qpsk,   Modulation::Qam16,
                                               Modulation::Qam64, Modulation::Qam256, Modulation::Polar};

// The gear list's columns: their headings, the widths they were drawn at,
// and their widest usual text. A column is as wide as its text needs at
// the system's font size, and never narrower than drawn (bigger desktop
// text cut "4 min 19 s" to "4 min 1...", Andrew, 2026-11-08).
struct GearColumn
{
    const char* heading;
    int format;
    int width;
    const char* widest;
};
const GearColumn GEAR_COLUMNS[] = {
    {"Gear", wxLIST_FORMAT_LEFT, 56, "Z8k"},
    {"Data2G mode", wxLIST_FORMAT_LEFT, 132, "w48-16qam-r1/2"},
    {"Speed", wxLIST_FORMAT_RIGHT, 84, "8222 bit/s"},
    {"S/N", wxLIST_FORMAT_RIGHT, 76, "\xE2\x88\x92" "12.3 dB"},
    {"1 KB", wxLIST_FORMAT_RIGHT, 80, "4 min 19 s"},
};

// A list column wide enough for its text, with the cell's padding.
int columnWidth(const wxWindow* list, int drawn, const wxString& text)
{
    int w = 0, h = 0;
    list->GetTextExtent(text, &w, &h);
    return std::max(drawn, w + list->FromDIP(16));
}

// Why there are no gears to draw, or empty when there are: no thresholds
// found at all, or thresholds with no widths and speeds yet (data2g-host's
// own file has none; its MODES list gives them). A host since Data2G PR
// #186 gives the thresholds too, so no file is asked for.
wxString whyNoGears(const Gearshift& shift)
{
    if (shift.table().empty() && !shift.hostThresholds())
    {
        return "No thresholds found. Ragtime looks for Data2G's own\n"
               "data2g/codes_data/mode_thresholds.json in a Data2G checkout\n"
               "($DATA2G_HOME, ~/Data2G, ~/src/Data2G and the like).\n"
               "Load it with Load thresholds... below.";
    }
    for (const Submode& mode : shift.usable().modes())
    {
        if (mode.hasFacts())
            return wxString();
    }
    return wxString::Format("Thresholds loaded for %d modes. Their widths and speeds\n"
                            "come from data2g-host: the gears appear once it is connected.",
                            static_cast<int>(shift.table().modes().size()));
}

void setPen(wxGraphicsContext& gc, const wxColour& colour, double width,
            wxPenStyle style = wxPENSTYLE_SOLID)
{
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour).Width(width).Style(style)));
}

} // namespace

// The Submode Map: speed across on a log scale, the S/N each mode needs down
// the side (harder further down), one dot per mode coloured by modulation,
// the best modes on a thin dashed line and the gears as knobs bearing their
// short names on a heavy one. Clicking a knob picks that gear, and clicking
// any other dot within the width picks its mode.
class SubmodeMap : public wxPanel
{
public:
    SubmodeMap(wxWindow* parent, const Gearshift& shift, std::function<void(int)> onPickGear,
               std::function<void(const std::string&)> onPickMode)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(640, 430))
        , shift_(shift)
        , onPickGear_(std::move(onPickGear))
        , onPickMode_(std::move(onPickMode))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(560, 380));
        Bind(wxEVT_PAINT, &SubmodeMap::onPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
            Refresh();
            e.Skip();
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
            mouse_ = e.GetPosition();
            hovering_ = true;
            Refresh();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            hovering_ = false;
            Refresh();
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
            if (qrnBox_.Contains(e.GetPosition().x, e.GetPosition().y)
                || needleBox_.Contains(e.GetPosition().x, e.GetPosition().y))
                return;
            const int gear = gearAt(e.GetPosition());
            if (gear > 0)
            {
                onPickGear_(gear);
                return;
            }
            const Submode* mode = dotAt(dots(plot()), e.GetPosition());
            if (mode && mode->widthHz <= widthCapHz(shift_.effectiveWidth()))
                onPickMode_(mode->name);
        });
    }

private:
    struct Plot
    {
        double left, top, right, bottom;
        double snrTop, snrBottom;  // dB at the top and bottom edges
    };

    // The plot's margins, from the small font's size: what they were drawn
    // at, more for bigger text. The S/N title stands on its side at the
    // left with the scale beside it, the legend sits above, the speeds and
    // "bit/s" under, and the S-meter's units and title at the right.
    struct Margins
    {
        double left, top, bottom, sAxis;
        double across;      // "bit/s" this far under the plot
    };

    // The legend over the plot: a dot and words for each modulation, then
    // the hollow dot for modes wider than the width, left to right from the
    // plot's left edge, onto another row where the next would run past the
    // panel's (bigger text, narrower map).
    struct LegendItem
    {
        wxString label;
        double x, y;
    };

    std::vector<LegendItem> legendItems(double left) const
    {
        const wxFont small = GetFont().Smaller();
        std::vector<wxString> labels;
        for (Modulation kind : LEGEND_KINDS)
            labels.push_back(modulationLabel(kind));
        labels.push_back(wxString::FromUTF8("wider than ") + widthLabel(shift_.effectiveWidth()));
        std::vector<LegendItem> items;
        double x = left, y = 10;
        for (const wxString& label : labels)
        {
            int w = 0, h = 0;
            GetTextExtent(label, &w, &h, nullptr, nullptr, &small);
            if (x > left && x + 13 + w > GetClientSize().x - 4)
            {
                x = left;
                y += std::max(13, h) + 4;
            }
            items.push_back({label, x, y});
            x += 13 + w + 14;
        }
        return items;
    }

    Margins margins() const
    {
        const wxFont small = GetFont().Smaller();
        int w = 0, h = 0;
        GetTextExtent(wxString::FromUTF8("\xE2\x88\x92") + "30", &w, &h, nullptr, nullptr, &small);
        Margins m;
        m.left = std::max(58.0, 4.0 + h + 4.0 + w + 6.0);
        const std::vector<LegendItem> legend = legendItems(m.left);
        m.top = std::max(34.0, legend.back().y + std::max(13, h) + 10.0);
        m.across = std::max(20.0, 4.0 + h + 2.0);
        m.bottom = std::max(42.0, m.across + h + 4.0);
        int sw = 0;
        GetTextExtent("S9+60", &sw, &h, nullptr, nullptr, &small);
        m.sAxis = std::max(S_AXIS_WIDTH, 7.0 + sw + 4.0 + h + 2.0);
        return m;
    }

    Plot plot() const
    {
        const wxSize size = GetClientSize();
        const Margins m = margins();
        Plot p {m.left, m.top, size.x - (noise_.reads ? m.sAxis : 16.0), size.y - m.bottom, -15.0, 30.0};
        // One S/N range for every path, so the dots move when the path
        // changes but the scale doesn't.
        double lo = 1e9, hi = -1e9;
        for (const Submode& mode : shift_.usable().modes())
        {
            for (Path path : ALL_PATHS)
            {
                if (std::optional<double> dB = mode.thresholdOn(path))
                {
                    lo = std::min(lo, *dB);
                    hi = std::max(hi, *dB);
                }
            }
        }
        if (lo <= hi)
        {
            p.snrTop = std::floor((lo - 1.0) / 5.0) * 5.0;
            p.snrBottom = std::ceil((hi + 1.0) / 5.0) * 5.0;
        }
        return p;
    }

public:
    // The band's noise, for the QRN line and the S-meter axis.
    void setNoise(const BandNoise& noise)
    {
        if (noise.reads == noise_.reads && noise.dbOverS9 == noise_.dbOverS9 && noise.filterHz == noise_.filterHz)
            return;
        noise_ = noise;
        Refresh();
    }

private:
    static double xFor(const Plot& p, double bps)
    {
        const double t = (std::log10(bps) - std::log10(MIN_BPS))
                         / (std::log10(MAX_BPS) - std::log10(MIN_BPS));
        return p.left + t * (p.right - p.left);
    }

    static double yFor(const Plot& p, double dB)
    {
        return p.top + (dB - p.snrTop) / (p.snrBottom - p.snrTop) * (p.bottom - p.top);
    }

    std::vector<std::pair<const Submode*, wxPoint2DDouble>> dots(const Plot& p) const
    {
        std::vector<std::pair<const Submode*, wxPoint2DDouble>> out;
        for (const Submode& mode : shift_.usable().modes())
        {
            const std::optional<double> dB = mode.thresholdOn(shift_.path());
            if (mode.hasFacts() && dB)
                out.emplace_back(&mode, wxPoint2DDouble(xFor(p, mode.bitsPerSecond), yFor(p, *dB)));
        }
        return out;
    }

    // The dot within 9 px of a point, the nearest if several.
    static const Submode* dotAt(const std::vector<std::pair<const Submode*, wxPoint2DDouble>>& all,
                                const wxPoint& at)
    {
        const Submode* found = nullptr;
        double nearest = 81.0;  // 9 px, squared
        for (const auto& [mode, dot] : all)
        {
            const double d = (dot.m_x - at.x) * (dot.m_x - at.x) + (dot.m_y - at.y) * (dot.m_y - at.y);
            if (d < nearest)
            {
                nearest = d;
                found = mode;
            }
        }
        return found;
    }

    int gearAt(const wxPoint& at) const
    {
        const Plot p = plot();
        for (const Gear& gear : shift_.ladder())
        {
            const double dx = at.x - xFor(p, gear.bitsPerSecond);
            const double dy = at.y - yFor(p, gear.threshold);
            if (std::fabs(dx) <= KNOB_HALF_WIDTH + 3 && std::fabs(dy) <= KNOB_RADIUS + 3)
                return gear.number;
        }
        return 0;
    }

    void onPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxColour back = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
        const wxColour ink = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        const wxColour faint = blend(back, ink, 0.10);
        const wxColour soft = blend(back, ink, 0.55);
        dc.SetBackground(wxBrush(back));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
        if (!gc)
            return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        const wxFont font = GetFont();
        const wxFont small = font.Smaller();
        const Plot p = plot();

        const wxString empty = whyNoGears(shift_);
        if (!empty.empty())
        {
            gc->SetFont(font, soft);
            const wxString& text = empty;
            double w = 0, h = 0;
            gc->GetTextExtent(text, &w, &h);
            gc->DrawText(text, (GetClientSize().x - w) / 2, (GetClientSize().y - h) / 2);
            return;
        }

        // Grid and axes.
        gc->SetFont(small, soft);
        setPen(*gc, faint, 1);
        const double ticks[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
        const char* tickLabels[] = {"20", "50", "100", "200", "500", "1k", "2k", "5k", "10k"};
        for (int i = 0; i < 9; i++)
        {
            const double x = xFor(p, ticks[i]);
            gc->StrokeLine(x, p.top, x, p.bottom);
            double w = 0, h = 0;
            gc->GetTextExtent(tickLabels[i], &w, &h);
            gc->DrawText(tickLabels[i], x - w / 2, p.bottom + 4);
        }
        for (double dB = p.snrTop; dB <= p.snrBottom + 0.01; dB += 5.0)
        {
            const double y = yFor(p, dB);
            gc->StrokeLine(p.left, y, p.right, y);
            const wxString label = dB > 0 ? wxString::Format("+%.0f", dB)
                                   : dB < 0 ? wxString::FromUTF8("\xE2\x88\x92")
                                                  + wxString::Format("%.0f", -dB)
                                            : wxString("0");
            double w = 0, h = 0;
            gc->GetTextExtent(label, &w, &h);
            gc->DrawText(label, p.left - w - 6, y - h / 2);
        }
        {
            const wxString across = "bit/s";
            double w = 0, h = 0;
            gc->GetTextExtent(across, &w, &h);
            gc->DrawText(across, (p.left + p.right - w) / 2, p.bottom + margins().across);
            const wxString down = "S/N needed, dB in 2500 Hz (harder further down)";
            gc->GetTextExtent(down, &w, &h);
            gc->DrawText(down, 4, (p.top + p.bottom + w) / 2, PI / 2);
        }

        // Auto: the path's S/N as a dashed line, and everything needing more
        // than it less the margin shaded.
        const std::optional<double> snr = shift_.pathSnr();
        if (snr)
        {
            const double limit = std::max(p.top, std::min(p.bottom, yFor(p, *snr - AUTO_MARGIN_DB)));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(soft.Red(), soft.Green(), soft.Blue(), 48)));
            gc->DrawRectangle(p.left, limit, p.right - p.left, p.bottom - limit);
            const double line = yFor(p, *snr);
            if (line >= p.top && line <= p.bottom)
            {
                setPen(*gc, ink, 1, wxPENSTYLE_SHORT_DASH);
                gc->StrokeLine(p.left, line, p.right, line);
                gc->SetFont(small, ink);
                const wxString label = "path S/N " + utf8(formatDb(*snr));
                double w = 0, h = 0;
                gc->GetTextExtent(label, &w, &h);
                gc->DrawText(label, p.right - w - 4, line - h - 1);
            }
            gc->SetFont(small, soft);
            const wxString margin = wxString::Format("needs more than the path less %.0f dB",
                                                     AUTO_MARGIN_DB);
            double w = 0, h = 0;
            gc->GetTextExtent(margin, &w, &h);
            if (limit + h + 2 < p.bottom)
                gc->DrawText(margin, p.left + 6, p.bottom - h - 4);
        }

        drawNoise(*gc, p, small, soft);

        // Every other best mode on a thin dashed line.
        const int cap = widthCapHz(shift_.effectiveWidth());
        const std::vector<const Submode*> best = bestModes(shift_.usable(), shift_.path(), cap);
        if (best.size() > 1)
        {
            std::vector<wxPoint2DDouble> points;
            for (const Submode* mode : best)
                points.emplace_back(xFor(p, mode->bitsPerSecond),
                                    yFor(p, *mode->thresholdOn(shift_.path())));
            setPen(*gc, soft, 1, wxPENSTYLE_SHORT_DASH);
            gc->StrokeLines(points.size(), points.data());
        }

        // The dots; modes wider than the width cap are hollow grey circles.
        const auto all = dots(p);
        for (const auto& [mode, at] : all)
        {
            const double r = DOT_RADIUS;
            if (mode->widthHz > cap)
            {
                setPen(*gc, soft, 1.5);
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
            }
            else
            {
                setPen(*gc, back, 1);
                gc->SetBrush(wxBrush(fromRgb(modulationColour(mode->modulation))));
            }
            gc->DrawEllipse(at.m_x - r, at.m_y - r, 2 * r, 2 * r);
        }

        // The gears: knobs bearing their short names, joined by a heavy line.
        const std::vector<Gear>& ladder = shift_.ladder();
        const std::optional<Gear> current = shift_.current();
        const std::optional<Gear> suggestion = shift_.suggestion();
        // A mode chosen off the ladder is ringed in blue.
        if (current && current->number == 0)
        {
            const double r = DOT_RADIUS + 4;
            setPen(*gc, ACCENT, 2.5);
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->DrawEllipse(xFor(p, current->bitsPerSecond) - r, yFor(p, current->threshold) - r, 2 * r, 2 * r);
        }

        if (ladder.size() > 1)
        {
            std::vector<wxPoint2DDouble> points;
            for (const Gear& gear : ladder)
                points.emplace_back(xFor(p, gear.bitsPerSecond), yFor(p, gear.threshold));
            setPen(*gc, ink, 3);
            gc->StrokeLines(points.size(), points.data());
        }
        const wxFont knobFont = small.Bold();
        for (const Gear& gear : ladder)
        {
            const double x = xFor(p, gear.bitsPerSecond);
            const double y = yFor(p, gear.threshold);
            const bool isCurrent = current && current->number == gear.number;
            const bool tooHard = snr && dbShort(gear.threshold, *snr) > 0;
            // A gear the path can't carry in red; Auto's pick, while Auto is off,
            // ringed in green.
            const wxColour line = isCurrent ? ACCENT : tooHard ? badText() : ink;
            // The gear number only for a mode with no short name.
            const wxString label = gear.name.empty() ? wxString::Format("%d", gear.number)
                                                     : utf8(gear.name);
            gc->SetFont(knobFont, isCurrent ? *wxWHITE : line);
            double w = 0, h = 0;
            gc->GetTextExtent(label, &w, &h);
            const double r = std::max(KNOB_RADIUS, h / 2 + 2);
            const double half = std::max(KNOB_HALF_WIDTH, w / 2 + 5);
            if (!shift_.isAuto() && suggestion && suggestion->number == gear.number)
            {
                setPen(*gc, goodText(), 2.5);
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRoundedRectangle(x - half - 4, y - r - 4, 2 * (half + 4), 2 * (r + 4), r + 4);
            }
            setPen(*gc, line, 2);
            gc->SetBrush(wxBrush(isCurrent ? ACCENT : back));
            gc->DrawRoundedRectangle(x - half, y - r, 2 * half, 2 * r, r);
            gc->DrawText(label, x - w / 2, y - h / 2);
        }

        // The legend.
        {
            gc->SetFont(small, ink);
            const std::vector<LegendItem> legend = legendItems(p.left);
            for (std::size_t i = 0; i < legend.size(); i++)
            {
                const LegendItem& item = legend[i];
                if (i < LEGEND_KINDS.size())
                {
                    gc->SetPen(*wxTRANSPARENT_PEN);
                    gc->SetBrush(wxBrush(fromRgb(modulationColour(LEGEND_KINDS[i]))));
                }
                else
                {
                    setPen(*gc, soft, 1.5);
                    gc->SetBrush(*wxTRANSPARENT_BRUSH);
                }
                gc->DrawEllipse(item.x, item.y + 2, 9, 9);
                double w = 0, h = 0;
                gc->GetTextExtent(item.label, &w, &h);
                gc->DrawText(item.label, item.x + 13, item.y + 6.5 - h / 2);
            }
        }

        // Hovering a dot shows its name, width, speed and threshold, and
        // hovering the QRN label what it is.
        if (hovering_)
        {
            const Submode* hovered = dotAt(all, mouse_);
            const bool onQrn = !hovered && noise_.dbOverS9 && qrnBox_.Contains(mouse_.x, mouse_.y);
            const bool onNeedle = !hovered && !onQrn && noise_.dbOverS9 && needleBox_.Contains(mouse_.x, mouse_.y);
            if (hovered || onQrn || onNeedle)
            {
                const wxString text
                    = onQrn      ? qrnTip(*noise_.dbOverS9)
                      : onNeedle ? needleTip(*noise_.dbOverS9)
                            : utf8(shift_.shortName(hovered->name)) + "   " + utf8(hovered->name) + "\n"
                                  + wxString::Format("%d Hz", hovered->widthHz) + wxString::FromUTF8(" \xC2\xB7 ")
                                  + modulationLabel(hovered->modulation) + "\n"
                                  + utf8(formatSpeed(hovered->bitsPerSecond)) + "\n"
                                  + utf8(formatDb(*hovered->thresholdOn(shift_.path()))) + " on a "
                                  + pathLabel(shift_.path()) + " path";
                gc->SetFont(small, ink);
                double w = 0, h = 0;
                gc->GetTextExtent(text, &w, &h);
                double bx = mouse_.x + 14, by = mouse_.y + 14;
                if (bx + w + 12 > GetClientSize().x)
                    bx = mouse_.x - w - 26;
                if (by + h + 10 > p.bottom)
                    by = mouse_.y - h - 24;
                setPen(*gc, soft, 1);
                gc->SetBrush(wxBrush(back));
                gc->DrawRoundedRectangle(bx, by, w + 12, h + 10, 4);
                gc->DrawText(text, bx + 6, by + 5);
            }
        }
    }

    // The band's noise from the S-meter: a blue dashed line at 0 dB S/N
    // labelled with the rig's own reading, "QRN: -40" (dB over S9), and an
    // axis on the right saying what a station at each S/N reads on the
    // S-meter: the noise and the signal together across the rig's filter
    // (radio::meterRiseDb), 6 dB an S-unit. Gears for stations that lift
    // the needle less than half an S-unit off the noise are shaded: under
    // the needle.
    void drawNoise(wxGraphicsContext& gc, const Plot& p, const wxFont& small, const wxColour& soft)
    {
        using namespace ragtime::radio;
        qrnBox_ = wxRect();
        needleBox_ = wxRect();
        if (!noise_.reads)
            return;
        const wxColour ink = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        double w = 0, h = 0;
        const wxString title = noise_.filterHz
                                   ? wxString::Format("On your S-meter, %.0f Hz filter", *noise_.filterHz)
                                   : wxString("On your S-meter");
        gc.SetFont(small, soft);
        gc.GetTextExtent(title, &w, &h);
        gc.DrawText(title, GetClientSize().x - h - 2, (p.top + p.bottom + w) / 2, PI / 2);
        if (!noise_.dbOverS9)
        {
            const wxString waiting = "QRN: reading";
            gc.GetTextExtent(waiting, &w, &h);
            gc.DrawText(waiting, p.right + 6, p.top - h - 4);
            return;
        }
        const double noise = *noise_.dbOverS9;
        const double filter = noise_.filterHz.value_or(0.0);

        // Under the needle: from the weakest S/N to the one that lifts the
        // needle half an S-unit.
        const double needle = std::max(p.top, std::min(p.bottom, yFor(p, *snrForRise(NEEDLE_DB, filter))));
        double lineH = 0;
        gc.SetFont(small, ink);
        gc.GetTextExtent("S9", &w, &lineH);
        const bool restsShown = needle - p.top > lineH + 4;
        if (needle > p.top)
        {
            gc.SetPen(*wxTRANSPARENT_PEN);
            gc.SetBrush(wxBrush(wxColour(ACCENT.Red(), ACCENT.Green(), ACCENT.Blue(), 28)));
            gc.DrawRectangle(p.left, p.top, p.right - p.left, needle - p.top);
            setPen(gc, ACCENT, 1);
            if (needle < p.bottom)
                gc.StrokeLine(p.left, needle, p.right + 4, needle);
            // The needle stays on the noise all the way up: a bar on the
            // axis, with the noise's S-units.
            setPen(gc, ACCENT, 3);
            gc.StrokeLine(p.right + 2, p.top, p.right + 2, needle);
            gc.SetFont(small, ink);
            const wxString rests = utf8(sUnitText(noise));
            gc.GetTextExtent(rests, &w, &h);
            if (restsShown)
                gc.DrawText(rests, p.right + 7, (p.top + needle - h) / 2);
            gc.SetFont(small, ACCENT);
            const wxString under = "under the needle";
            gc.GetTextExtent(under, &w, &h);
            if (needle - p.top > h + 8)
            {
                const double bx = p.right - w - 6, by = p.top + 4;
                gc.DrawText(under, bx, by);
                needleBox_ = wxRect((int)bx, (int)by, (int)w + 1, (int)h + 1);
            }
        }

        // The axis: S1 to S9 a unit at a time, then S9+10 and up, each
        // where a station lifts the needle to it.
        setPen(gc, soft, 1);
        gc.SetFont(small, ink);
        auto tick = [&](double dbOverS9) {
            if (dbOverS9 - noise < NEEDLE_DB)
                return;
            const double y = yFor(p, *snrForRise(dbOverS9 - noise, filter));
            if (y < p.top - 0.5 || y > p.bottom + 0.5)
                return;
            // Not on top of the noise's own label.
            if (restsShown && y - needle < lineH)
                return;
            gc.StrokeLine(p.right, y, p.right + 4, y);
            const wxString label = utf8(sUnitText(dbOverS9));
            gc.GetTextExtent(label, &w, &h);
            gc.DrawText(label, p.right + 7, y - h / 2);
        };
        for (int s = 1; s <= 9; s++)
            tick((s - 9) * DB_PER_S_UNIT);
        for (int over = 10; over <= 60; over += 10)
            tick(over);

        // The noise itself: S/N 0.
        const double y = yFor(p, 0.0);
        if (y < p.top || y > p.bottom)
            return;
        setPen(gc, ACCENT, 2, wxPENSTYLE_SHORT_DASH);
        gc.StrokeLine(p.left, y, p.right, y);
        const wxString label = "QRN: " + utf8(levelText(noise));
        const wxFont bold = small.Bold();
        gc.SetFont(bold, *wxWHITE);
        gc.GetTextExtent(label, &w, &h);
        const double bw = w + 16, bh = h + 6;
        // At the right, unless the path S/N's label is there.
        const std::optional<double> snr = shift_.pathSnr();
        const bool clash = snr && std::fabs(yFor(p, *snr) - y) < bh + 14;
        const double bx = clash ? p.left + 8 : p.right - bw - 8, by = y - bh / 2;
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(ACCENT));
        gc.DrawRoundedRectangle(bx, by, bw, bh, bh / 2);
        gc.DrawText(label, bx + 8, by + 3);
        qrnBox_ = wxRect((int)bx, (int)by, (int)bw + 1, (int)bh + 1);
    }

    // "your 2400 Hz filter", or that its width isn't known.
    wxString filterText() const
    {
        if (noise_.filterHz)
            return wxString::Format("your %.0f Hz filter", *noise_.filterHz);
        return "your filter (its width not read: taken as 2500 Hz)";
    }

    wxString qrnTip(double noise) const
    {
        using namespace ragtime::radio;
        return wxString::Format("QRN: %s, the band's noise on your S-meter (%s): the quietest\n"
                                "reading between bursts in the last minute, in dB over S9.\n"
                                "The S-meter axis says how strong a station reads for each\n"
                                "gear: its signal and the noise together across %s,\n"
                                "6 dB an S-unit. Data2G's S/N is in 2500 Hz.",
                                utf8(levelText(noise)), utf8(sUnitText(noise)), filterText());
    }

    wxString needleTip(double noise) const
    {
        using namespace ragtime::radio;
        const double filter = noise_.filterHz.value_or(0.0);
        return wxString::Format("Under the needle: the gears up here copy stations too weak to\n"
                                "lift your S-meter half an S-unit off the noise (QRN %s, %s).\n"
                                "With %s, a station at S/N 0 lifts it %.1f dB,\n"
                                "and one at -12 dB, Data2G's slowest, %.1f dB.\n"
                                "Data2G hears what the needle can't show.",
                                utf8(levelText(noise)), utf8(sUnitText(noise)), filterText(),
                                meterRiseDb(0, filter), meterRiseDb(-12, filter));
    }

    const Gearshift& shift_;
    BandNoise noise_;
    wxRect qrnBox_, needleBox_;
    std::function<void(int)> onPickGear_;
    std::function<void(const std::string&)> onPickMode_;
    wxPoint mouse_;
    bool hovering_ = false;
};

GearshiftDialog::GearshiftDialog(wxWindow* owner, bool onTop, Gearshift& shift, std::function<void()> onChange,
                                 std::function<void()> onClosed, std::function<BandNoise()> noise)
    // A frame, not a dialog: window managers keep a dialog above the rest
    // of its program even with no parent (Openbox does). A frame floats on
    // its parent only when asked to.
    : wxFrame(owner, wxID_ANY, "Gearshift", wxDefaultPosition, wxDefaultSize,
              wxDEFAULT_FRAME_STYLE | (onTop ? wxFRAME_FLOAT_ON_PARENT : 0))
    , shift_(shift)
    , onChange_(std::move(onChange))
    , onClosed_(std::move(onClosed))
    , noise_(std::move(noise))
    , noiseTimer_(this)
{
    SetIcons(appIcons());
    auto* panel = new wxPanel(this);
    auto* top = new wxBoxSizer(wxVERTICAL);

    // Width chips, narrowest to widest, then Path chips, then Auto.
    auto* chips = new wxBoxSizer(wxHORIZONTAL);
    chips->Add(new wxStaticText(panel, wxID_ANY, "Width"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    for (std::size_t i = 0; i < ALL_WIDTHS.size(); i++)
    {
        const Width width = ALL_WIDTHS[i];
        auto* chip = new wxToggleButton(panel, wxID_ANY, utf8(widthLabel(width)),
                                        wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        chip->Bind(wxEVT_TOGGLEBUTTON, [this, width](wxCommandEvent&) {
            shift_.setWidth(width);
            changed();
        });
        widthChips_[i] = chip;
        chips->Add(chip, 0, wxRIGHT, 2);
    }
    chips->AddSpacer(18);
    chips->Add(new wxStaticText(panel, wxID_ANY, "Path"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    for (std::size_t i = 0; i < ALL_PATHS.size(); i++)
    {
        const Path path = ALL_PATHS[i];
        auto* chip = new wxToggleButton(panel, wxID_ANY, pathLabel(path), wxDefaultPosition,
                                        wxDefaultSize, wxBU_EXACTFIT);
        chip->SetToolTip(path == Path::Calm
                             ? "No fading (AWGN)"
                             : wxString("ITU-R F.1487 ") + pathLabel(path) + " fading channel");
        chip->Bind(wxEVT_TOGGLEBUTTON, [this, path](wxCommandEvent&) {
            shift_.setPath(path);
            changed();
        });
        pathChips_[i] = chip;
        chips->Add(chip, 0, wxRIGHT, 2);
    }
    chips->AddSpacer(18);
    auto_ = new wxCheckBox(panel, wxID_ANY,
                           wxString::Format("Auto (%.0f dB margin)", AUTO_MARGIN_DB));
    auto_->SetToolTip("Take the fastest gear that needs no more than the path's S/N, "
                      "less the margin");
    auto_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
        shift_.setAuto(e.IsChecked());
        changed();
    });
    chips->Add(auto_, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(chips, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    widthNote_ = new wxStaticText(panel, wxID_ANY, wxEmptyString);
    widthNote_->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    widthNote_->SetFont(widthNote_->GetFont().Smaller());
    top->Add(widthNote_, 0, wxLEFT | wxTOP, 10);

    // The map, and the gear list beside it.
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    map_ = new SubmodeMap(panel, shift_, [this](int gear) {
        if (shift_.selectGear(gear))
            changed();
    }, [this](const std::string& mode) {
        if (shift_.selectMode(mode))
            changed();
    });
    body->Add(map_, 1, wxEXPAND | wxRIGHT, 10);

    auto* side = new wxBoxSizer(wxVERTICAL);
    inGear_ = new wxStaticText(panel, wxID_ANY, wxEmptyString);
    inGear_->SetFont(inGear_->GetFont().Bold());
    side->Add(inGear_, 0, wxBOTTOM, 4);
    snr_ = new wxStaticText(panel, wxID_ANY, wxEmptyString);
    side->Add(snr_, 0, wxBOTTOM, 8);
    gears_ = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxSize(434, -1),
                            wxLC_REPORT | wxLC_SINGLE_SEL);
    int listWidth = 0;
    for (int i = 0; i < 5; i++)
    {
        const GearColumn& c = GEAR_COLUMNS[i];
        const int width = std::max(columnWidth(gears_, c.width, c.heading),
                                   columnWidth(gears_, c.width, wxString::FromUTF8(c.widest)));
        gears_->InsertColumn(i, c.heading, c.format, width);
        listWidth += width;
    }
    // The columns and the list's frame: 434 as drawn.
    gears_->SetMinSize(wxSize(std::max(434, listWidth + 6), -1));
    gears_->SetToolTip("Gears in red need more S/N than the path has, less Auto's 3 dB margin");
    gears_->Bind(wxEVT_LIST_ITEM_SELECTED, &GearshiftDialog::onGearSelected, this);
    side->Add(gears_, 1, wxEXPAND);
    // Any mode the map draws within the width, gear or not.
    auto* any = new wxBoxSizer(wxHORIZONTAL);
    any->Add(new wxStaticText(panel, wxID_ANY, "Any mode"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    anyMode_ = new wxChoice(panel, wxID_ANY);
    anyMode_->SetToolTip("Choose a mode that is not a gear. Clicking its dot on the map does the same.");
    anyMode_->Bind(wxEVT_CHOICE, [this](wxCommandEvent& e) {
        const int row = e.GetSelection();
        if (row >= 0 && row < static_cast<int>(anyModes_.size()) && shift_.selectMode(anyModes_[row]))
            changed();
    });
    any->Add(anyMode_, 1, wxALIGN_CENTER_VERTICAL);
    side->Add(any, 0, wxEXPAND | wxTOP, 6);
    body->Add(side, 0, wxEXPAND);
    top->Add(body, 1, wxEXPAND | wxALL, 10);

    // Where the thresholds came from.
    auto* foot = new wxBoxSizer(wxHORIZONTAL);
    source_ = new wxStaticText(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                               wxST_ELLIPSIZE_MIDDLE);
    foot->Add(source_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    load_ = new wxButton(panel, wxID_ANY, "Load thresholds...");
    load_->SetToolTip("Data2G's data2g/codes_data/mode_thresholds.json, or the Submode Map's file");
    load_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { loadFile(); });
    foot->Add(load_, 0, wxRIGHT, 6);
    // Done with a green check, not "X Close": every change has already
    // gone to the gearshift, so there is nothing to save or cancel (Jeff,
    // 2026-10-25: "X Close implies that you cancel something").
    auto* done = new wxButton(panel, wxID_OK, "Done");
    done->SetBitmap(checkIcon(goodText()));
    done->SetToolTip("Your choices are already in use; this just closes the gearshift");
    // Done, Escape and the title bar's X all close it.
    done->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE && !e.HasAnyModifiers())
            Close();
        else
            e.Skip();
    });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        noiseTimer_.Stop();
        Hide();
        if (onClosed_)
        {
            auto closed = std::move(onClosed_);
            onClosed_ = nullptr;
            closed();
        }
        Destroy();
    });
    foot->Add(done, 0);
    top->Add(foot, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    panel->SetSizer(top);
    auto* whole = new wxBoxSizer(wxVERTICAL);
    whole->Add(panel, 1, wxEXPAND);
    SetSizerAndFit(whole);
    // Fits Jeff's 1366x768 screen with room for the panel and title bar.
    // Bigger desktop text can ask for more, up to the screen.
    const wxSize fitted = GetSize();
    const int display = wxDisplay::GetFromWindow(owner ? owner : this);
    const wxRect screen = wxDisplay(display == wxNOT_FOUND ? 0u : (unsigned)display).GetClientArea();
    auto onScreen = [&](int w, int h) {
        return wxSize(std::min(std::max(w, fitted.x), screen.width), std::min(std::max(h, fitted.y), screen.height));
    };
    SetSize(onScreen(1144, 610));
    SetMinSize(onScreen(1004, 520));
    refresh();
    CentreOnParent();

    // The band's noise, as the rig reads it.
    if (noise_)
    {
        map_->setNoise(noise_());
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { map_->setNoise(noise_()); });
        noiseTimer_.Start(1000);
    }
}

void GearshiftDialog::changed()
{
    refresh();
    if (onChange_)
    {
        changing_ = true;
        onChange_();
        changing_ = false;
    }
}

void GearshiftDialog::follow()
{
    if (!changing_ && !refreshing_)
        refresh();
}

void GearshiftDialog::refresh()
{
    refreshing_ = true;
    for (std::size_t i = 0; i < ALL_WIDTHS.size(); i++)
    {
        // The 500 Hz switch greys the wider widths; the chip lit is the
        // width the gears come from.
        widthChips_[i]->Enable(!shift_.narrowOnly() || widthCapHz(ALL_WIDTHS[i]) <= widthCapHz(Width::Hz500));
        widthChips_[i]->SetValue(ALL_WIDTHS[i] == shift_.effectiveWidth());
    }
    widthNote_->SetLabel(shift_.narrowOnly()
                             ? "The 500 Hz switch is on: data2g-host uses only 500 Hz modes, so the gears do too."
                             : "Width picks the modes the gears come from; data2g-host may still use any "
                               "width. The 500 Hz switch on the connection bar limits it.");
    for (std::size_t i = 0; i < ALL_PATHS.size(); i++)
        pathChips_[i]->SetValue(ALL_PATHS[i] == shift_.path());
    auto_->SetValue(shift_.isAuto());

    const std::optional<Gear> current = shift_.current();
    const std::optional<double> snr = shift_.pathSnr();
    if (current)
        inGear_->SetLabel(utf8(gearLabel(*current)) + (current->name.empty() ? "" : ", " + utf8(current->mode))
                          + (shift_.isAuto() && snr ? " (Auto)" : ""));
    else
        inGear_->SetLabel("No gears");
    if (snr)
    {
        // The weakest station heard lately, as data2g-host heard it.
        const int bursts = static_cast<int>(shift_.snrBursts());
        snr_->SetLabel("Path S/N " + utf8(formatDb(*snr)) + ", " + utf8(shift_.snrFrom())
                       + (bursts == 1 ? wxString(", from its last burst")
                                      : wxString::Format(", the mean of its last %d bursts", bursts)));
        snr_->SetToolTip("The weakest station whose bursts data2g-host heard in the last 15 minutes. "
                         "Broadcasts go to every station, so Auto shifts for the weakest. This is how "
                         "well you hear them; they may hear you a little better or worse.");
    }
    else
    {
        snr_->SetLabel(shift_.isAuto() ? "No S/N reports yet: Auto holds the chosen gear"
                                       : "No S/N reports yet");
        snr_->SetToolTip("data2g-host reports an S/N with each burst it hears from a station "
                         "(since Data2G PR #98). None has come in the last 15 minutes.");
    }

    gears_->DeleteAllItems();
    const std::vector<Gear>& ladder = shift_.ladder();
    for (std::size_t i = 0; i < ladder.size(); i++)
    {
        const Gear& gear = ladder[i];
        const long row = gears_->InsertItem(static_cast<long>(i), utf8(gear.name));
        gears_->SetItem(row, 1, utf8(gear.mode));
        gears_->SetItem(row, 2, utf8(formatSpeed(gear.bitsPerSecond)));
        gears_->SetItem(row, 3, utf8(formatDb(gear.threshold)));
        gears_->SetItem(row, 4, utf8(formatDuration(secondsPerKilobyte(gear.bitsPerSecond))));
        // A file's longer mode names widen their column.
        for (int col = 0; col < 5; col++)
        {
            const int width = columnWidth(gears_, gears_->GetColumnWidth(col), gears_->GetItemText(row, col));
            if (width > gears_->GetColumnWidth(col))
                gears_->SetColumnWidth(col, width);
        }
        if (snr && dbShort(gear.threshold, *snr) > 0)
            gears_->SetItemTextColour(row, badText());
        if (current && current->number == gear.number)
        {
            gears_->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
            gears_->EnsureVisible(row);
        }
    }

    // Every mode within the width with an S/N on this path, slowest first.
    std::vector<const Submode*> modes;
    for (const Submode& mode : shift_.usable().modes())
    {
        if (mode.hasFacts() && mode.widthHz <= widthCapHz(shift_.effectiveWidth())
            && mode.thresholdOn(shift_.path()))
            modes.push_back(&mode);
    }
    std::sort(modes.begin(), modes.end(), [](const Submode* a, const Submode* b) {
        return std::make_pair(a->bitsPerSecond, a->name) < std::make_pair(b->bitsPerSecond, b->name);
    });
    anyMode_->Clear();
    anyModes_.clear();
    for (const Submode* mode : modes)
    {
        const std::string name = shift_.shortName(mode->name);
        anyMode_->Append(utf8(name == mode->name ? name : name + "  " + mode->name) + ", "
                         + utf8(formatSpeed(mode->bitsPerSecond)) + ", "
                         + utf8(formatDb(*mode->thresholdOn(shift_.path()))));
        anyModes_.push_back(mode->name);
        if (current && current->mode == mode->name)
            anyMode_->SetSelection(static_cast<int>(anyModes_.size()) - 1);
    }

    const wxArrayString sources = loadedThresholdSources();
    const int count = static_cast<int>(shift_.table().modes().size());
    if (load_->Show(!shift_.hostThresholds()))
        load_->GetContainingSizer()->Layout();
    if (shift_.hostThresholds())
        source_->SetLabel(wxString::Format("Thresholds from data2g-host (%d modes)",
                                           static_cast<int>(shift_.usable().modes().size())));
    else if (shift_.table().empty())
        source_->SetLabel("No thresholds found");
    else if (sources.empty())
        source_->SetLabel(wxString::Format("Thresholds for %d modes", count));
    else
        source_->SetLabel(wxString::Format("Thresholds from %s (%d modes)",
                                           wxJoin(sources, ';', '\0'), count));
    source_->SetToolTip(shift_.hostThresholds() ? wxString("From data2g-host's MODES list")
                        : sources.empty()
                            ? "Looked in " + wxJoin(data2gSearchFolders(), ',', '\0')
                            : wxJoin(sources, '\n', '\0'));

    map_->Refresh();
    refreshing_ = false;
}

void GearshiftDialog::onGearSelected(wxListEvent& event)
{
    if (refreshing_)
        return;
    if (shift_.selectGear(static_cast<int>(event.GetIndex()) + 1))
        CallAfter([this]() { changed(); });
}

void GearshiftDialog::loadFile()
{
    wxFileDialog picker(this, "Load Data2G thresholds", wxEmptyString, "mode_thresholds.json",
                        "JSON files (*.json)|*.json|All files|*",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (picker.ShowModal() != wxID_OK)
        return;
    SubmodeTable check;
    wxString error;
    if (!loadThresholds(picker.GetPath(), check, error))
    {
        wxMessageBox(error, "Thresholds not loaded", wxOK | wxICON_WARNING, this);
        return;
    }
    // The picked file goes on top of a Data2G checkout's, if one was found.
    rememberThresholdsPath(picker.GetPath());
    wxArrayString sources;
    wxString errors;
    shift_.setTable(loadAllThresholds(sources, errors));
    changed();
}
