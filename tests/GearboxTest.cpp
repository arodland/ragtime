// The gearshift model. The figures here are placeholders made up to
// exercise each rule, not Data2G's: whether Ragtime may carry Data2G's
// thresholds file is still Jeff's call.
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/gearbox/Gearbox.h"
#include "ragtime/gearbox/Submodes.h"

using namespace ragtime::gearbox;

namespace
{

// Calm, 2.4 kHz: p-fsk-2 beats p-fsk-1 at the same S/N, p-fsk-3 is beaten
// on both counts, p-qpsk-1 and polar-p1 are best modes too close to the gear
// below to be gears, and p-64qam is added as the top gear. Good, 2.4 kHz:
// p-16qam-b is the fastest but too close to p-16qam, so it takes its place.
// ack-fast and n4-ack-1 would win everything if they were allowed.
const char* PLACEHOLDER_TABLE = R"({
  "p-fsk-1":   {"hz": 500,  "mod": "8-FSK",  "rate": "1/3", "bps": 30,
                "th": {"awgn": -12, "mpg": -3, "mpp": -7, "mpd": -8}},
  "p-fsk-2":   {"hz": 500,  "mod": "16-FSK", "rate": "1/2", "bps": 40,
                "th": {"awgn": -12, "mpg": -3, "mpp": -7, "mpd": -8}},
  "p-fsk-3":   {"hz": 500,  "mod": "32-FSK", "rate": "1/2", "bps": 35,
                "th": {"awgn": -11, "mpg": -2, "mpp": -6, "mpd": -7}},
  "p-200":     {"hz": 200,  "mod": "QPSK",   "rate": "1/5", "bps": 20,
                "th": {"awgn": -8, "mpg": 1, "mpp": -2, "mpd": 2}},
  "p-qpsk-1":  {"hz": 1200, "mod": "QPSK",   "rate": "1/3", "bps": 70,
                "th": {"awgn": -9, "mpg": 0, "mpp": -4, "mpd": -5}},
  "p-qpsk-2":  {"hz": 1200, "mod": "QPSK",   "rate": "1/2", "bps": 100,
                "th": {"awgn": -7, "mpg": 2, "mpp": -2, "mpd": -2}},
  "polar-p1":   {"hz": 1200, "mod": "Specialty", "rate": "0.10", "bps": 150,
                "th": {"awgn": -6, "mpg": null, "mpp": null, "mpd": null}},
  "p-16qam":   {"hz": 2300, "mod": "QAM16",  "rate": "1/2", "bps": 400,
                "th": {"awgn": 0, "mpg": 6, "mpp": 5, "mpd": 4}},
  "p-16qam-b": {"hz": 2400, "mod": "QAM16",  "rate": "2/3", "bps": 450,
                "th": {"awgn": null, "mpg": 8, "mpp": null, "mpd": null}},
  "p-64qam":   {"hz": 2400, "mod": "QAM64",  "rate": "3/4", "bps": 600,
                "th": {"awgn": 5, "mpg": null, "mpp": 12, "mpd": 11}},
  "ack-fast":  {"hz": 2400, "mod": "Specialty", "rate": "0.2", "bps": 10000,
                "th": {"awgn": -20, "mpg": -20, "mpp": -20, "mpd": -20}},
  "n4-ack-1":  {"hz": 200,  "mod": "Specialty", "rate": "0.1", "bps": 5000,
                "th": {"awgn": -20, "mpg": -20, "mpp": -20, "mpd": -20}}
})";

SubmodeTable placeholderTable()
{
    SubmodeTable table;
    std::string error;
    CHECK(table.parseJson(PLACEHOLDER_TABLE, &error));
    CHECK_EQ(error, std::string());
    return table;
}

std::string names(const std::vector<Gear>& ladder)
{
    std::string out;
    for (const Gear& gear : ladder)
        out += (out.empty() ? "" : " ") + gear.mode;
    return out;
}

std::string names(const std::vector<const Submode*>& modes)
{
    std::string out;
    for (const Submode* mode : modes)
        out += (out.empty() ? "" : " ") + mode->name;
    return out;
}

void testParsing()
{
    const SubmodeTable table = placeholderTable();
    CHECK_EQ(table.modes().size(), 10u);  // the ack- modes left out
    CHECK(table.find("ack-fast") == nullptr);
    CHECK(table.find("n4-ack-1") == nullptr);

    const Submode* polar = table.find("polar-p1");
    CHECK(polar != nullptr);
    if (polar)
    {
        CHECK_EQ(polar->widthHz, 1200);
        CHECK(polar->modulation == Modulation::Polar);
        CHECK_EQ(polar->rate, std::string("0.10"));
        CHECK(polar->thresholdOn(Path::Calm) == std::optional<double>(-6.0));
        CHECK(!polar->thresholdOn(Path::Good));
    }
    const Submode* fsk = table.find("p-fsk-3");
    CHECK(fsk && fsk->modulation == Modulation::Fsk);
    const Submode* qam = table.find("p-64qam");
    CHECK(qam && qam->modulation == Modulation::Qam64);
    CHECK(table.find("p-200") && table.find("p-200")->modulation == Modulation::Qpsk);

    // Bad input says why and leaves the table alone.
    SubmodeTable kept = placeholderTable();
    std::string error;
    CHECK(!kept.parseJson("{\"x\": {\"hz\": 500}}", &error));
    CHECK(error.find("bps") != std::string::npos);
    CHECK_EQ(kept.modes().size(), 10u);
    CHECK(!kept.parseJson("{\"x\": {\"hz\": 500, \"bps\": 30,}}", &error));
    CHECK(!kept.parseJson("[1, 2]", &error));
    CHECK(!kept.parseJson("{} trailing", &error));
    CHECK(!kept.parseJson("{\"x\": {\"hz\": 500, \"bps\": 30, \"th\": {\"awgn\": \"low\"}}}",
                          &error));
    CHECK(!kept.parseJson(std::string(200, '[') + std::string(200, ']'), &error));
    CHECK(!kept.parseJson("", &error));
    CHECK_EQ(kept.modes().size(), 10u);

    // Escapes, exponents and fields the gearbox doesn't use.
    SubmodeTable odd;
    CHECK(odd.parseJson("{\"caf\\u00e9-\\ud83c\\udfb5\": {\"hz\": 5e2, \"bps\": 3.25E1, "
                        "\"lead\": [1, true, false, null], \"th\": {\"mpd\": -1.5e0}}}",
                        &error));
    const Submode* cafe = odd.find("caf\xC3\xA9-\xF0\x9F\x8E\xB5");
    CHECK(cafe != nullptr);
    if (cafe)
    {
        CHECK_EQ(cafe->widthHz, 500);
        CHECK_EQ(cafe->bitsPerSecond, 32.5);
        CHECK(cafe->modulation == Modulation::Other);
        CHECK(cafe->thresholdOn(Path::Disturbed) == std::optional<double>(-1.5));
        CHECK(!cafe->thresholdOn(Path::Calm));
    }
    CHECK(!odd.parseJson("{\"\\ud83c\": {\"hz\": 1, \"bps\": 1}}", &error));

    // Thresholds from a MODES report, one at a time.
    SubmodeTable live = placeholderTable();
    CHECK(live.setThreshold("polar-p1", Path::Good, -1.0));
    CHECK(live.find("polar-p1")->thresholdOn(Path::Good) == std::optional<double>(-1.0));
    CHECK(!live.setThreshold("ack-fast", Path::Good, -1.0));
    CHECK(!live.add(Submode{"ack-1f", 1200, Modulation::Other, "", 222, {}}));

    // A mode the map doesn't know, from the host's MODES list.
    CHECK(live.setFacts("w48-64l-r2/3", 2400, 6556.0));
    CHECK(live.find("w48-64l-r2/3")->modulation == Modulation::Qam64);
    CHECK(live.find("w48-64l-r2/3")->hasFacts());
    CHECK(!live.setFacts("n10-ack-4f", 500, 55.0));

    CHECK_EQ(placeholderTable().offeredBy({}).modes().size(), 10u);
    CHECK_EQ(names(gearLadder(placeholderTable().offeredBy({"p-fsk-2", "p-qpsk-2", "nope"}),
                              Path::Calm, 2400)),
             std::string("p-fsk-2 p-qpsk-2"));
}

// data2g-host's own codes_data/mode_thresholds.json form: thresholds only,
// 99 for never, widths and speeds from MODES or the map's file.
void testHostForm()
{
    const char* hostFile = R"({
      "_source": "placeholder figures",
      "channels": ["awgn", "mpg", "mpp", "mpd"],
      "modes": {
        "p-fsk-2":     [-13.0, -4, -8, -9],
        "p-polar":     [-5.5, -1, -2, 99],
        "n4-qpsk-r1/3": [-10, 0.5, -1.5, 99.0],
        "ack-4f":      [-20, -20, -20, -20]
      }
    })";

    SubmodeTable table = placeholderTable();
    std::string error;
    CHECK(table.parseJson(hostFile, &error));
    CHECK_EQ(error, std::string());
    CHECK(table.find("ack-4f") == nullptr);
    // Updated thresholds, the map's width and speed kept.
    const Submode* fsk = table.find("p-fsk-2");
    CHECK(fsk && fsk->thresholdOn(Path::Calm) == std::optional<double>(-13.0));
    CHECK(fsk && fsk->widthHz == 500 && fsk->bitsPerSecond == 40.0);
    // New modes have no width or speed yet, so they are never gears.
    const Submode* narrow = table.find("n4-qpsk-r1/3");
    CHECK(narrow != nullptr);
    if (narrow)
    {
        CHECK(!narrow->hasFacts());
        CHECK(narrow->modulation == Modulation::Qpsk);
        CHECK(!narrow->thresholdOn(Path::Disturbed));
        CHECK(narrow->thresholdOn(Path::Good) == std::optional<double>(0.5));
    }
    CHECK(table.find("p-polar") && table.find("p-polar")->modulation == Modulation::Other);
    CHECK_EQ(names(gearLadder(table, Path::Calm, 200)), std::string("p-200"));
    table.setFacts("n4-qpsk-r1/3", 200, 88.0);
    CHECK_EQ(names(gearLadder(table, Path::Calm, 200)), std::string("n4-qpsk-r1/3"));

    // Thresholds alone, from an empty table.
    SubmodeTable bare;
    CHECK(bare.parseJson(hostFile, &error));
    CHECK_EQ(bare.modes().size(), 3u);
    CHECK(gearLadder(bare, Path::Calm, 2400).empty());

    CHECK(!bare.parseJson(R"({"channels": ["awgn"], "modes": {"x": [1, 2]}})", &error));
    CHECK(error.find("x") != std::string::npos);
    CHECK(!bare.parseJson(R"({"channels": "awgn", "modes": {}})", &error));
    CHECK(!bare.parseJson(R"({"channels": ["awgn"], "modes": {"x": ["low"]}})", &error));
    CHECK_EQ(bare.modes().size(), 3u);
    // An unknown channel is skipped.
    CHECK(bare.parseJson(R"({"channels": ["awgn", "hf9"], "modes": {"y-qpsk": [1, 2]}})",
                         &error));
    CHECK(bare.find("y-qpsk")->thresholdOn(Path::Calm) == std::optional<double>(1.0));

    // Speeds from MODES airtimes: 116-byte codewords, 64 at most, 1.21 s
    // for one and 37.50 s for all.
    CHECK(std::fabs(payloadBitsPerSecond(116, 64, 1.21, 37.50) - 1611.1) < 0.5);
    CHECK_EQ(payloadBitsPerSecond(116, 1, 1.21, 1.21), 0.0);
    CHECK_EQ(payloadBitsPerSecond(0, 64, 1.21, 37.50), 0.0);
    CHECK_EQ(payloadBitsPerSecond(116, 64, 37.50, 1.21), 0.0);

    CHECK(modulationOf("", "fsk16r25-r1/3") == Modulation::Fsk);
    CHECK(modulationOf("Specialty", "polar-k96-f4") == Modulation::Polar);
    CHECK(modulationOf("", "n10-256l-r3/4") == Modulation::Qam256);
    CHECK(modulationOf("", "w48-16qam-r1/2") == Modulation::Qam16);
    CHECK(modulationOf("QAM64", "anything") == Modulation::Qam64);
    CHECK(modulationOf("OFDM", "qpsk-r1/2") == Modulation::Other);
    CHECK_EQ(modulationColour(Modulation::Polar), 0xa16207u);
}

void testLadders()
{
    const SubmodeTable table = placeholderTable();

    CHECK_EQ(names(bestModes(table, Path::Calm, 2400)),
             std::string("p-fsk-2 p-qpsk-1 p-qpsk-2 polar-p1 p-16qam p-64qam"));
    CHECK_EQ(names(gearLadder(table, Path::Calm, 2400)),
             std::string("p-fsk-2 p-qpsk-2 p-16qam p-64qam"));
    CHECK_EQ(names(gearLadder(table, Path::Good, 2400)),
             std::string("p-fsk-2 p-qpsk-2 p-16qam-b"));
    CHECK_EQ(names(gearLadder(table, Path::Calm, 1200)),
             std::string("p-fsk-2 p-qpsk-2 polar-p1"));
    CHECK_EQ(names(gearLadder(table, Path::Calm, 500)), std::string("p-fsk-2"));
    CHECK_EQ(names(gearLadder(table, Path::Calm, 200)), std::string("p-200"));
    CHECK(gearLadder(table, Path::Calm, 100).empty());
    CHECK(gearLadder(SubmodeTable(), Path::Calm, 2400).empty());

    const std::vector<Gear> ladder = gearLadder(table, Path::Poor, 2400);
    CHECK_EQ(names(ladder), std::string("p-fsk-2 p-qpsk-2 p-16qam p-64qam"));
    CHECK_EQ(ladder.size(), 4u);
    if (ladder.size() == 4)
    {
        CHECK_EQ(ladder[0].number, 1);
        CHECK_EQ(ladder[3].number, 4);
        CHECK_EQ(ladder[2].threshold, 5.0);  // the Poor path's figure
        CHECK_EQ(ladder[2].widthHz, 2300);
        CHECK(ladder[2].modulation == Modulation::Qam16);
        CHECK_EQ(gearLabel(ladder[2]), std::string("Gear K4C"));
    }

    CHECK_EQ(widthCapHz(Width::Hz1200), 1200);
    CHECK_EQ(std::string(widthLabel(Width::Hz1200)), std::string("1.2 kHz"));
    CHECK_EQ(std::string(widthLabel(Width::Hz2400)), std::string("2.4 kHz"));
    CHECK_EQ(std::string(pathLabel(Path::Disturbed)), std::string("Disturbed"));
    CHECK(pathFromKey("mpp") == std::optional<Path>(Path::Poor));
    CHECK(!pathFromKey("hf"));
}

void testAuto()
{
    const std::vector<Gear> ladder = gearLadder(placeholderTable(), Path::Calm, 2400);
    // Thresholds -12, -7, 0, +5 dB; Auto keeps 3 dB in hand.
    CHECK_EQ(autoGearIndex(ladder, 3.5), 2);
    CHECK_EQ(autoGearIndex(ladder, 3.0), 2);  // exactly on the margin fits
    CHECK_EQ(autoGearIndex(ladder, 2.9), 1);
    CHECK_EQ(autoGearIndex(ladder, -30.0), 0);  // nothing fits: gear 1
    CHECK_EQ(autoGearIndex(ladder, 30.0), 3);
    CHECK_EQ(autoGearIndex(ladder, 3.0, 0.0), 2);
    CHECK_EQ(autoGearIndex(ladder, 5.0, 0.0), 3);
    CHECK_EQ(autoGearIndex({}, 10.0), -1);

    CHECK_EQ(dbShort(0.0, 1.0), 2.0);
    CHECK_EQ(dbShort(-7.0, 1.0), -5.0);

    PathSnr snr;
    CHECK(!snr.average());
    for (double dB : {100.0, 1.0, 2.0, 3.0, 4.0, 5.0})
        snr.record(dB);
    snr.record(std::nan(""));
    CHECK_EQ(snr.count(), PathSnr::BURSTS);
    CHECK(snr.average() == std::optional<double>(3.0));  // the 100 dB burst rolled off
    snr.clear();
    CHECK(!snr.average());
}

void testFormatting()
{
    CHECK_EQ(secondsPerKilobyte(8192.0), 1.0);
    CHECK_EQ(secondsPerKilobyte(0.0), 0.0);
    CHECK_EQ(formatDuration(secondsPerKilobyte(8222.0)), std::string("1.0 s"));
    CHECK_EQ(formatDuration(9.94), std::string("9.9 s"));
    CHECK_EQ(formatDuration(9.96), std::string("10 s"));
    CHECK_EQ(formatDuration(secondsPerKilobyte(319.0)), std::string("26 s"));
    CHECK_EQ(formatDuration(secondsPerKilobyte(32.0)), std::string("4 min 16 s"));
    CHECK_EQ(formatSpeed(138.9), std::string("139 bit/s"));
    CHECK_EQ(formatSpeed(72.5), std::string("72 bit/s"));
    CHECK_EQ(formatDb(1.125), std::string("+1.1 dB"));
    CHECK_EQ(formatDb(-3.0), std::string("\xE2\x88\x92" "3.0 dB"));
    CHECK_EQ(formatDb(-0.04), std::string("+0.0 dB"));
}

void testNames()
{
    // The letter: A at -12.5 dB, 1.25 dB a letter, K at 0, clamped to A..Z.
    CHECK_EQ(snrLetter(-12.5), 'A');
    CHECK_EQ(snrLetter(-20.0), 'A');
    CHECK_EQ(snrLetter(-11.875), 'B');  // halves go up
    CHECK_EQ(snrLetter(-5.0), 'G');
    CHECK_EQ(snrLetter(0.0), 'K');
    CHECK_EQ(snrLetter(0.6), 'K');
    CHECK_EQ(snrLetter(0.7), 'L');
    CHECK_EQ(snrLetter(18.75), 'Z');
    CHECK_EQ(snrLetter(40.0), 'Z');

    // The speed: bit/s below 100, then hundreds (C), then thousands (k).
    CHECK_EQ(speedShorthand(0.0), std::string());
    CHECK_EQ(speedShorthand(-5.0), std::string());
    CHECK_EQ(speedShorthand(29.4), std::string("29"));
    CHECK_EQ(speedShorthand(72.5), std::string("72"));  // as formatSpeed rounds
    CHECK_EQ(speedShorthand(99.4), std::string("99"));
    CHECK_EQ(speedShorthand(99.6), std::string("1C"));
    CHECK_EQ(speedShorthand(149.0), std::string("1C"));
    CHECK_EQ(speedShorthand(151.0), std::string("2C"));
    CHECK_EQ(speedShorthand(949.0), std::string("9C"));
    CHECK_EQ(speedShorthand(950.0), std::string("1k"));
    CHECK_EQ(speedShorthand(8222.0), std::string("8k"));
    CHECK_EQ(speedShorthand(12600.0), std::string("13k"));

    const std::map<std::string, std::string> placeholder = shortNames(placeholderTable());
    CHECK_EQ(placeholder.at("p-fsk-1"), std::string("A30"));
    CHECK_EQ(placeholder.at("p-fsk-3"), std::string("B35"));
    CHECK_EQ(placeholder.at("p-qpsk-2"), std::string("E1C"));
    CHECK_EQ(placeholder.at("polar-p1"), std::string("F2C"));
    CHECK_EQ(placeholder.at("p-16qam"), std::string("K4C"));
    CHECK_EQ(placeholder.at("p-64qam"), std::string("O6C"));
    CHECK(placeholder.count("p-16qam-b") == 0);  // no Calm figure, no name
    CHECK(placeholder.count("ack-fast") == 0);

    // Ties: the harder mode moves up a letter until its name is free. t-b
    // and t-a need the same S/N, so the slower t-a is the harder one.
    const char* ties = R"({
      "t-a":  {"hz": 200,  "mod": "QPSK", "rate": "1/2", "bps": 200,
               "th": {"awgn": -5.0, "mpg": null, "mpp": null, "mpd": null}},
      "t-b":  {"hz": 200,  "mod": "QPSK", "rate": "1/2", "bps": 220,
               "th": {"awgn": -5.0, "mpg": null, "mpp": null, "mpd": null}},
      "t-c":  {"hz": 500,  "mod": "QPSK", "rate": "1/2", "bps": 210,
               "th": {"awgn": -4.5, "mpg": null, "mpp": null, "mpd": null}},
      "t-d":  {"hz": 500,  "mod": "QPSK", "rate": "2/3", "bps": 190,
               "th": {"awgn": -2.6, "mpg": null, "mpp": null, "mpd": null}},
      "t-y":  {"hz": 2300, "mod": "QAM64", "rate": "3/4", "bps": 8000,
               "th": {"awgn": 30, "mpg": null, "mpp": null, "mpd": null}},
      "t-z":  {"hz": 2300, "mod": "QAM64", "rate": "4/5", "bps": 8100,
               "th": {"awgn": 31, "mpg": null, "mpp": null, "mpd": null}}
    })";
    SubmodeTable table;
    CHECK(table.parseJson(ties));
    const std::map<std::string, std::string> tied = shortNames(table);
    CHECK_EQ(tied.at("t-b"), std::string("G2C"));
    CHECK_EQ(tied.at("t-a"), std::string("H2C"));
    CHECK_EQ(tied.at("t-c"), std::string("I2C"));
    CHECK_EQ(tied.at("t-d"), std::string("J2C"));  // I2C, its own, went to t-c
    CHECK_EQ(tied.at("t-y"), std::string("Z8k"));
    CHECK(tied.count("t-z") == 0);  // would pass Z

    std::vector<Gear> ladder = gearLadder(placeholderTable(), Path::Calm, 2400);
    CHECK_EQ(ladder[2].name, std::string("K4C"));
    CHECK_EQ(gearLabel(ladder[2]), std::string("Gear K4C"));
    ladder[2].name.clear();
    CHECK_EQ(gearLabel(ladder[2]), std::string("Gear p-16qam"));
}

void testGearshift()
{
    Gearshift shift;
    CHECK(!shift.current());
    CHECK(shift.ladder().empty());

    shift.setTable(placeholderTable());
    CHECK(shift.width() == Width::Hz2400);
    CHECK(shift.path() == Path::Calm);
    CHECK(!shift.isAuto());
    CHECK_EQ(shift.ladder().size(), 4u);
    CHECK_EQ(shift.current()->number, 1);
    CHECK_EQ(shift.current()->name, std::string("A40"));
    CHECK_EQ(shift.shortName("p-16qam"), std::string("K4C"));
    CHECK_EQ(shift.shortName("n4-ack-1"), std::string("n4-ack-1"));
    CHECK_EQ(shift.shortName("p-16qam-b"), std::string("p-16qam-b"));

    CHECK(shift.selectGear(3));
    CHECK(!shift.selectGear(0));
    CHECK(!shift.selectGear(5));
    CHECK_EQ(shift.current()->mode, std::string("p-16qam"));

    // Narrower: p-16qam is gone, so the fastest gear no faster than it.
    shift.setWidth(Width::Hz1200);
    CHECK_EQ(shift.current()->mode, std::string("polar-p1"));
    // Wider again: polar-p1 isn't a gear on 2.4 kHz, so no shift up.
    shift.setWidth(Width::Hz2400);
    CHECK_EQ(shift.current()->mode, std::string("p-qpsk-2"));
    // A path where the chosen mode is still a gear keeps it.
    shift.setPath(Path::Good);
    CHECK_EQ(shift.current()->mode, std::string("p-qpsk-2"));
    CHECK_EQ(shift.current()->number, 2);
    shift.setPath(Path::Calm);

    // Auto with no S/N reports keeps the chosen gear.
    shift.setAuto(true);
    CHECK(!shift.suggestion());
    CHECK_EQ(shift.current()->mode, std::string("p-qpsk-2"));
    shift.setPathSnr(8.0, 2, "W1AW");
    CHECK(shift.pathSnr() == std::optional<double>(8.0));
    CHECK_EQ(shift.snrBursts(), 2u);
    CHECK_EQ(shift.snrFrom(), std::string("W1AW"));
    CHECK_EQ(shift.suggestion()->mode, std::string("p-64qam"));
    CHECK_EQ(shift.current()->mode, std::string("p-64qam"));
    CHECK_EQ(shift.chosen()->mode, std::string("p-qpsk-2"));
    // Picking a gear by hand turns Auto off; the suggestion stays.
    CHECK(shift.selectGear(1));
    CHECK(!shift.isAuto());
    CHECK_EQ(shift.current()->mode, std::string("p-fsk-2"));
    CHECK_EQ(shift.suggestion()->mode, std::string("p-64qam"));
    shift.setPathSnr(std::nullopt);
    CHECK(!shift.suggestion());
    CHECK_EQ(shift.snrBursts(), 0u);

    // Only what the host offers, with the host's own figures.
    shift.selectGear(2);
    HostMode fsk1{"p-fsk-1", 500, 31.0, {}, false};
    HostMode qpsk1{"p-qpsk-1", 1200, 0.0, {}, false};  // no airtimes: the file's speed stands
    HostMode qam16{"p-16qam", 2300, 390.0, {}, false};
    qam16.threshold[static_cast<int>(Path::Calm)] = -1.0;
    HostMode control{"n4-ack-1", 200, 5000.0, {}, false};
    shift.setHostModes({fsk1, qpsk1, qam16, control});
    CHECK_EQ(names(shift.ladder()), std::string("p-fsk-1 p-qpsk-1 p-16qam"));
    CHECK_EQ(shift.usable().modes().size(), 3u);
    CHECK_EQ(shift.current()->mode, std::string("p-qpsk-1"));
    CHECK_EQ(shift.ladder()[0].bitsPerSecond, 31.0);
    CHECK_EQ(shift.ladder()[1].bitsPerSecond, 70.0);
    CHECK_EQ(shift.ladder()[2].bitsPerSecond, 390.0);
    CHECK_EQ(shift.ladder()[2].threshold, -1.0);
    // Names follow the host's figures, and cover modes it doesn't offer.
    CHECK_EQ(shift.ladder()[0].name, std::string("A31"));
    CHECK_EQ(shift.ladder()[2].name, std::string("J4C"));
    CHECK_EQ(shift.shortName("p-fsk-2"), std::string("A40"));
    // The table itself keeps the file's figures.
    CHECK_EQ(shift.table().find("p-16qam")->bitsPerSecond, 400.0);
    CHECK(!shift.hostThresholds());
    // Thresholds in MODES (Data2G PR #186) win over the file's, "never"
    // included.
    HostMode given = qam16;
    given.hasThresholds = true;
    HostMode never = fsk1;
    never.hasThresholds = true;
    shift.setHostModes({given, never, qpsk1});
    CHECK(shift.hostThresholds());
    CHECK_EQ(names(shift.ladder()), std::string("p-qpsk-1 p-16qam"));
    CHECK(!shift.usable().find("p-16qam")->thresholdOn(Path::Good));
    shift.setHostModes({});
    CHECK_EQ(shift.ladder().size(), 4u);

    // The 500 Hz switch caps the ladder but keeps the chosen width.
    shift.setNarrowOnly(true);
    CHECK(shift.narrowOnly());
    CHECK(shift.width() == Width::Hz2400);
    CHECK(shift.effectiveWidth() == Width::Hz500);
    CHECK_EQ(names(shift.ladder()), std::string("p-fsk-2"));
    shift.setWidth(Width::Hz200);  // narrower than the cap: taken as it is
    CHECK(shift.effectiveWidth() == Width::Hz200);
    CHECK_EQ(names(shift.ladder()), std::string("p-200"));
    shift.setWidth(Width::Hz2400);
    shift.setNarrowOnly(false);
    CHECK(shift.effectiveWidth() == Width::Hz2400);
    CHECK_EQ(shift.ladder().size(), 4u);
    // And choosing 500 Hz by hand leaves the switch alone.
    shift.setWidth(Width::Hz500);
    CHECK(!shift.narrowOnly());

    // Any mode, off the ladder: it holds while drawn within the width.
    shift.setWidth(Width::Hz2400);
    shift.setAuto(true);
    CHECK(shift.selectMode("polar-p1"));
    CHECK(!shift.isAuto());
    CHECK_EQ(shift.chosenMode(), std::string("polar-p1"));
    CHECK_EQ(shift.current()->mode, std::string("polar-p1"));
    CHECK_EQ(shift.current()->number, 0);
    shift.setWidth(Width::Hz200);  // too wide now: the ladder's pick stands in
    CHECK_EQ(shift.current()->mode, std::string("p-200"));
    shift.setWidth(Width::Hz2400);  // and the chosen mode comes back
    CHECK_EQ(shift.current()->mode, std::string("polar-p1"));
    // A mode that is a gear selects that gear.
    CHECK(shift.selectMode("p-16qam"));
    CHECK_EQ(shift.current()->number, 3);
    CHECK(!shift.selectMode("no-such-mode"));
    CHECK(shift.selectGear(1));
    CHECK(shift.chosenMode().empty());
    CHECK_EQ(shift.current()->mode, std::string("p-fsk-2"));
}

} // namespace

void testDial()
{
    // A log scale from 16 to 10000 bit/s, ticks five times apart.
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    CHECK(near(dialPosition(16), 0.0));
    CHECK(near(dialPosition(80), 0.25));
    CHECK(near(dialPosition(400), 0.5));
    CHECK(near(dialPosition(2000), 0.75));
    CHECK(near(dialPosition(10000), 1.0));
    // Off the scale stops at the ends; no speed rests at the left.
    CHECK(near(dialPosition(5), 0.0));
    CHECK(near(dialPosition(50000), 1.0));
    CHECK(near(dialPosition(0), 0.0));
    CHECK(near(dialPosition(-1), 0.0));
    CHECK(near(dialPosition(std::nan("")), 0.0));
    // Faster always points further round.
    CHECK(dialPosition(29) < dialPosition(200));
    CHECK(dialPosition(200) < dialPosition(8000));
}

int main()
{
    testDial();
    testParsing();
    testHostForm();
    testLadders();
    testAuto();
    testFormatting();
    testNames();
    testGearshift();
    return ragtime_test::finish();
}
