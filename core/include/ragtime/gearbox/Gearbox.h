// The gearshift: gear ladders built from the Submode Map, and Auto's pick
// from the path's S/N. docs/GEARBOX.md is the spec;
// tools/gearbox/gearbox_ladder.py is the reference the ladders must match.
#ifndef RAGTIME_GEARBOX_GEARBOX_H
#define RAGTIME_GEARBOX_GEARBOX_H

#include <array>
#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ragtime/gearbox/Submodes.h"

namespace ragtime::gearbox
{

// The width chips, narrowest to widest: the map's four groups. A width is a
// cap, so 2.4 kHz still uses the narrow modes.
enum class Width
{
    Hz200,
    Hz500,
    Hz1200,
    Hz2400,
};

constexpr std::array<Width, 4> ALL_WIDTHS = {Width::Hz200, Width::Hz500, Width::Hz1200,
                                             Width::Hz2400};

// 200, 500, 1200, 2400. 2.4 kHz takes the 2.3 kHz modes too.
int widthCapHz(Width width);
// "200 Hz", "500 Hz", "1.2 kHz", "2.4 kHz".
const char* widthLabel(Width width);

constexpr double GEAR_RATIO = 1.8;       // each gear at least this much faster
constexpr double TOP_GEAR_RATIO = 1.3;   // or the fastest mode replaces the top gear
constexpr double AUTO_MARGIN_DB = 3.0;   // Auto's margin below the path's S/N

struct Gear
{
    int number = 0;  // from 1, the most robust; 0 for a mode off the ladder
    std::string mode;  // Data2G's name, which host commands use
    std::string name;  // the short name, "E2C" (empty when unknown)
    int widthHz = 0;
    Modulation modulation = Modulation::Other;
    double bitsPerSecond = 0.0;
    double threshold = 0.0;  // S/N this mode needs on the ladder's path
};

// Short mode names (docs/GEARBOX.md, "Mode names"): a letter for the S/N
// the mode needs on a Calm path, then its speed. "E2C" needs about −5 dB
// and moves about 200 bit/s; "Z8k" needs +18.75 dB or more and moves about
// 8000. The popup shows Data2G's own names beside them.
//
// The letter: A is −12.5 dB, each letter 1.25 dB more, so K is 0 dB and
// every four letters is 5 dB. Below A reads A, above Z reads Z.
char snrLetter(double calmDb);
// The speed: whole bit/s below 100 ("29"), then hundreds as C ("1C" to
// "9C"), then thousands as k ("1k", "8k"). Empty for no speed.
std::string speedShorthand(double bitsPerSecond);
// A mode's name before ties are broken, "E2C". Empty when its Calm S/N or
// its speed is unknown.
std::string baseShortName(const Submode& mode);
// The short name of every mode in the table that has one, by Data2G name.
// When two modes come out the same, the harder one (higher Calm S/N, then
// slower, then by name) moves up a letter, again until it is unique; a mode
// that would pass Z keeps no short name.
std::map<std::string, std::string> shortNames(const SubmodeTable& table);

// The best modes no wider than capHz on a path: those no other mode beats on
// both speed and S/N, most robust first. Modes with no threshold on the path
// are left out.
std::vector<const Submode*> bestModes(const SubmodeTable& table, Path path, int capHz);

// The gears: gear 1 is the most robust best mode, and each next gear the
// most robust best mode at least GEAR_RATIO times faster than the one below.
// The fastest best mode is always the top gear: added if it is at least
// TOP_GEAR_RATIO times faster than the last gear, else in its place.
std::vector<Gear> gearLadder(const SubmodeTable& table, Path path, int capHz);

// Auto's pick: the index of the fastest gear needing no more than
// pathSnr - margin, or 0 (gear 1) when even gear 1 needs more. -1 for an
// empty ladder.
int autoGearIndex(const std::vector<Gear>& ladder, double pathSnr,
                  double margin = AUTO_MARGIN_DB);

// How many dB short a path is for a mode, with Auto's margin: positive when
// short, zero or negative when the mode fits.
double dbShort(double threshold, double pathSnr, double margin = AUTO_MARGIN_DB);

// Seconds to move 1 KB (1024 bytes) at a raw payload rate.
double secondsPerKilobyte(double bitsPerSecond);

// "1.0 s", "26 s", "4 min 16 s".
std::string formatDuration(double seconds);
// "139 bit/s", "8222 bit/s".
std::string formatSpeed(double bitsPerSecond);
// The Gear button's speedometer (Jeff, 2026-10-23): where its needle
// points for a speed, from 0 (the dial's left end) to 1 (its right end).
// The scale is logarithmic, DIAL_SLOWEST_BPS to DIAL_FASTEST_BPS, so its
// five ticks are 16, 80, 400, 2000 and 10000 bit/s, five times apart;
// Data2G's 29 bit/s sits near the left and 8000 near the right. Slower
// and faster speeds stop at the ends, and no speed rests at 0.
constexpr double DIAL_SLOWEST_BPS = 16.0;
constexpr double DIAL_FASTEST_BPS = 10000.0;
double dialPosition(double bitsPerSecond);
// "+1.1 dB", "−3.0 dB" (a real minus sign, UTF-8).
std::string formatDb(double dB);
// "Gear E2C", or Data2G's name when the gear has no short name. Gears go by their names; the numbers stay internal.
std::string gearLabel(const Gear& gear);

// The path's S/N, from a station's last few bursts: their mean in dB.
class PathSnr
{
public:
    static constexpr std::size_t BURSTS = 5;

    void record(double dB);
    void clear() { bursts_.clear(); }
    std::optional<double> average() const;
    std::size_t count() const { return bursts_.size(); }

private:
    std::deque<double> bursts_;
};

// A mode in data2g-host's MODES list, as the gearshift needs it. The host's
// width and speed win over a file's; so do its thresholds, when MODES
// carries them (Data2G PR #186), "never" included.
struct HostMode
{
    std::string name;
    int widthHz = 0;
    double bitsPerSecond = 0.0;  // payloadBitsPerSecond() of its airtimes
    std::array<std::optional<double>, PATH_COUNT> threshold;
    bool hasThresholds = false;  // MODES gave thresholds, so none means never
};

// The gearshift's state: width, path, Auto, and the gear in effect. The
// popup draws it and the Gear button shows current().
//
// The width only picks which modes the gears come from: choosing 500 Hz
// to get a narrow mode leaves data2g-host free to use any width. The 500 Hz
// switch on the connection bar is what limits the host (BW500), and while
// it is on the gears come only from 500 Hz modes (narrowOnly).
class Gearshift
{
public:
    // The thresholds table. Without one there are no gears.
    void setTable(SubmodeTable table);
    const SubmodeTable& table() const { return table_; }

    // data2g-host's MODES list: gears use only those modes. Empty until the
    // host has listed them, meaning every mode in the table.
    void setHostModes(std::vector<HostMode> modes);
    // The table with the host's figures, cut to what it offers: what the
    // map draws.
    const SubmodeTable& usable() const { return usable_; }
    // data2g-host's MODES gave thresholds: no file is needed.
    bool hostThresholds() const;

    void setWidth(Width width);
    Width width() const { return width_; }
    // The 500 Hz switch. It caps the width without changing the one chosen,
    // so turning it off brings the chosen width back.
    void setNarrowOnly(bool on);
    bool narrowOnly() const { return narrowOnly_; }
    // The width the ladder uses: the chosen one, no wider than 500 Hz while
    // narrowOnly.
    Width effectiveWidth() const;
    void setPath(Path path);
    Path path() const { return path_; }

    // With Auto on and a path S/N known, the gear in effect is Auto's pick;
    // with no S/N reports yet, it stays the chosen gear.
    void setAuto(bool on) { auto_ = on; }
    bool isAuto() const { return auto_; }

    // Chooses a gear by number (from 1) and turns Auto off. False if there
    // is no such gear.
    bool selectGear(int number);
    // Chooses any mode, on the ladder or off it, and turns Auto off. The
    // choice holds while the mode is drawn on the map within the width;
    // meanwhile the gear is picked as for a ladder change. True if it is
    // in effect now.
    bool selectMode(const std::string& mode);
    // The mode chosen with selectMode, empty after selectGear.
    const std::string& chosenMode() const { return chosenMode_; }

    // The path's S/N (dB, average power in 2500 Hz), the mean of `bursts`
    // bursts from the station `from`; none until data2g-host reports one.
    void setPathSnr(std::optional<double> dB, std::size_t bursts = 0, std::string from = std::string());
    std::optional<double> pathSnr() const { return pathSnr_; }
    std::size_t snrBursts() const { return snrBursts_; }
    const std::string& snrFrom() const { return snrFrom_; }

    // A mode's short name, from every mode the table and host know (not
    // only those offered, so names don't change with what the host offers).
    // Data2G's name when it has none, as for ack- modes.
    std::string shortName(const std::string& mode) const;

    // The ladder for the current width, path and offered modes.
    const std::vector<Gear>& ladder() const { return ladder_; }
    // The chosen gear (Auto off), even while Auto overrides it: a mode off
    // the ladder has number 0.
    std::optional<Gear> chosen() const;
    // Auto's pick, whether or not Auto is on. Empty with no path S/N.
    std::optional<Gear> suggestion() const;
    // The gear in effect: Auto's pick or the chosen gear.
    std::optional<Gear> current() const;

private:
    void rebuild();

    SubmodeTable table_;
    SubmodeTable usable_;
    std::vector<HostMode> hostModes_;
    Width width_ = Width::Hz2400;
    bool narrowOnly_ = false;
    Path path_ = Path::Calm;
    bool auto_ = false;
    std::optional<double> pathSnr_;
    std::size_t snrBursts_ = 0;
    std::string snrFrom_;
    std::map<std::string, std::string> names_;
    std::vector<Gear> ladder_;
    int chosen_ = 0;  // index into ladder_
    std::string chosenMode_;
    std::optional<Gear> offLadder_;  // chosenMode_, when it is off the ladder
};

} // namespace ragtime::gearbox

#endif
