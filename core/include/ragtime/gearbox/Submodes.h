// Data2G's submodes and the S/N each one needs, as the Data2G Submode Map
// gives them (docs/GEARBOX.md, "The figures"). The gearshift is built on
// this table.
//
// Nothing of Data2G's is built in: whether Ragtime may carry Data2G's
// figures is Jeff's call. The table is read from a file, either the
// Submode Map's JSON or data2g-host's own codes_data/mode_thresholds.json,
// which every operator running data2g-host already has. Widths and speeds
// also come from the host's MODES list, and its thresholds too from hosts
// since Data2G PR #186.
#ifndef RAGTIME_GEARBOX_SUBMODES_H
#define RAGTIME_GEARBOX_SUBMODES_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ragtime::gearbox
{

// The map's four paths: no fading (AWGN), and the ITU-R F.1487 good, poor
// and disturbed fading channels. The path picks the gearbox.
enum class Path
{
    Calm,
    Good,
    Poor,
    Disturbed,
};

constexpr int PATH_COUNT = 4;
constexpr std::array<Path, PATH_COUNT> ALL_PATHS = {Path::Calm, Path::Good, Path::Poor,
                                                    Path::Disturbed};

// "Calm", "Good", "Poor", "Disturbed".
const char* pathLabel(Path path);
// The map's key for a path: "awgn", "mpg", "mpp", "mpd".
const char* pathKey(Path path);
std::optional<Path> pathFromKey(const std::string& key);

// What colours a mode's dot on the map.
enum class Modulation
{
    Fsk,
    Qpsk,
    Qam16,
    Qam64,
    Qam256,
    Polar,
    Other,
};

// From the map's "mod" field ("16-FSK", "QPSK", "QAM16", ...), or from the
// mode's name when that is empty or "Specialty" (the map's word for the
// polar and ack- modes): fsk16r25-r1/3, n10-64l-r3/4, polar-k96-f4.
Modulation modulationOf(const std::string& mapModulation, const std::string& modeName);
// "FSK", "QPSK", "16-QAM", "64-QAM", "256-QAM", "Polar", "Other".
const char* modulationLabel(Modulation modulation);
// The map's dot colour, 0xRRGGBB: FSK #0f766e, QPSK #1f5fbf, 16-QAM #7c3aed,
// 64-QAM #b8326b, 256-QAM #9a3412, polar #a16207.
std::uint32_t modulationColour(Modulation modulation);

struct Submode
{
    std::string name;
    int widthHz = 0;  // 0 until a file or the host's MODES says
    Modulation modulation = Modulation::Other;
    std::string rate;  // the code rate as the map writes it, "1/2"
    double bitsPerSecond = 0.0;  // payload rate, no burst overhead; 0 until known

    // Per path: the S/N (dB, average transmitted power over noise in
    // 2500 Hz) at which 10% of the smallest ARQ data bursts fail. Empty when
    // the mode never reached 90% copy on that path.
    std::array<std::optional<double>, PATH_COUNT> threshold;

    std::optional<double> thresholdOn(Path path) const
    {
        return threshold[static_cast<int>(path)];
    }

    // Known well enough to be a gear on some path: width and speed.
    bool hasFacts() const { return widthHz > 0 && bitsPerSecond > 0; }
};

// A mode's payload rate from the airtimes in data2g-host's MODES list: each
// codeword past the first adds (secondsAtMax - secondsAtOne) /
// (maxCodewords - 1) seconds and carries bytesPerCodeword bytes. 0 when the
// figures can't say.
double payloadBitsPerSecond(int bytesPerCodeword, int maxCodewords, double secondsAtOne,
                            double secondsAtMax);

// The ack- modes (ack-1f, n4-ack-8f, ...) carry data2g-host's own ARQ
// control, never data, so they are never gears (Jeff, 2026-10-22).
bool isArqControlMode(const std::string& name);

class SubmodeTable
{
public:
    // Reads a thresholds file into the table, adding or updating the modes
    // it names and keeping the rest. Two forms:
    //
    // - The Submode Map's: an object mapping each mode name to
    //   {"hz", "mod", "rate", "bps", "th": {"awgn", "mpg", "mpp", "mpd"}},
    //   a threshold being a number or null.
    // - data2g-host's mode_thresholds.json: {"channels": ["awgn", "mpg",
    //   "mpp", "mpd"], "modes": {name: [dB, ...]}}, 99 meaning never. It has
    //   no widths or speeds; those come from the host's MODES list.
    //
    // Other fields are ignored, and the ack- modes are left out. On bad
    // input it returns false, says why in *error, and leaves the table as
    // it was.
    bool parseJson(const std::string& text, std::string* error = nullptr);

    // Adds a mode, replacing one of the same name. ack- modes are refused.
    bool add(Submode mode);

    // A mode's width and speed, as the host's MODES list gives them. Adds
    // the mode if it is new. False for an ack- mode.
    bool setFacts(const std::string& name, int widthHz, double bitsPerSecond);

    const Submode* find(const std::string& name) const;
    const std::vector<Submode>& modes() const { return modes_; }
    bool empty() const { return modes_.empty(); }

    // For a MODES report that carries thresholds. False if the mode isn't
    // in the table.
    bool setThreshold(const std::string& name, Path path, std::optional<double> dB);

    // Only the modes data2g-host offers (the names from its MODES list).
    // An empty list means nothing is listed yet and keeps every mode.
    SubmodeTable offeredBy(const std::vector<std::string>& names) const;

private:
    std::vector<Submode> modes_;  // sorted by name
};

} // namespace ragtime::gearbox

#endif
