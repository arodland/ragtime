// Broadcast files, seals and repair: several stations, each a HostLink,
// BroadcastChat, Seals and BroadcastFiles, on an air in memory that
// answers the command lines and carries every ACKMODE frame to every
// other station, except what a test makes a listener lose. No sockets and
// a simulated clock, so it runs everywhere the core builds.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Check.h"
#include "ragtime/broadcast/Airtime.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/BroadcastFiles.h"
#include "ragtime/broadcast/Seals.h"
#include "ragtime/files/FileRows.h"
#include "ragtime/files/Parity.h"
#include "ragtime/gearbox/Gearbox.h"
#include "ragtime/link/Data2GLink.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Records.h"

using namespace ragtime;
using broadcast::BroadcastChat;
using broadcast::BroadcastFile;
using broadcast::BroadcastFiles;
using broadcast::Seals;
using link::HostLink;
using FileState = BroadcastFile::State;

namespace
{

uint64_t now = 1792627200000; // 2026-10-22 00:00 UTC

constexpr int GROUP_PORT = 2;
const char* const MODES = "MODE qpsk-r1/3 1200 76 64 1.78 74.36\r"
                          "MODE fsk16r25-r1/3 500 38 9 4.98 89.22\r"
                          "MODE w48-16qam-r1/2 2400 236 64 1.21 37.50\r"
                          "OK\r";

struct Station
{
    Station(const std::string& call, uint32_t seed)
        : link({call, true, false}), chat(link), seals(link, chat), files(link, chat, seals)
    {
        BroadcastChat::Settings s;
        s.callsign = call;
        s.software = "Ragtime test";
        chat.setSettings(s, 0);
        seals.setChain(call, Seals::Chain());
        seals.setRandomSeed(seed);
        // Most of these tests repair chat, so they seal it; the default is
        // off (testChatUnsealed).
        seals.setSealText(true);
        seals.setMode("qpsk-r1/3");
        chat.onItems([this](const std::vector<protocol::Record>& items, uint64_t burst, const std::string& mode,
                            uint64_t at) { seals.going(items, burst, mode, at); });
        link.commandUp(now);
    }

    const std::string& call() const { return chat.settings().callsign; }

    void events()
    {
        for (const HostLink::Event& e : link.takeEvents())
        {
            std::vector<protocol::Record> records = chat.onEvent(e, now);
            seals.onEvent(e, now);
            files.onEvent(e, now);
            for (const protocol::Record& r : records)
            {
                seals.onRecord(r, now);
                files.onRecord(r, now);
                heard.push_back(r);
            }
            for (std::vector<protocol::Record> again = seals.takeRebuilt(); !again.empty(); again = seals.takeRebuilt())
            {
                for (const protocol::Record& r : again)
                {
                    chat.onRebuilt(r, now);
                    seals.onRecord(r, now);
                    files.onRecord(r, now);
                    rebuilt.push_back(r);
                }
            }
        }
    }

    void tick()
    {
        link.tick(now);
        seals.tick(now);
        files.tick(now);
        events();
    }

    size_t heardFrom(const std::string& from, protocol::RecordType type) const
    {
        size_t n = 0;
        for (const protocol::Record& r : heard)
        {
            if (r.header.callsign == from && r.type() == (uint8_t)type) n++;
        }
        return n;
    }

    size_t textsFrom(const std::string& from) const
    {
        size_t n = 0;
        for (const broadcast::Message& m : chat.messages())
        {
            if (m.from == from && m.file == 0) n++;
        }
        return n;
    }

    HostLink link;
    BroadcastChat chat;
    Seals seals;
    BroadcastFiles files;
    link::KissDecoder kiss;
    std::vector<protocol::Record> heard;
    std::vector<protocol::Record> rebuilt;  // from parity
    // Decides whether this station hears a record (true: lost), and may
    // spoil it.
    std::function<bool(protocol::Record&)> lose;
    // The S/N this station's host reports each station's bursts at; none
    // for a station not listed, as a host from before Data2G PR #98.
    std::map<std::string, double> hearsAt;
    // The mode its group bursts go in, as it last asked the host.
    std::string groupMode;
};

// A record on the air, and the mode its burst went in.
struct Aired
{
    std::string mode;
    protocol::Record record;
};

struct Air
{
    std::vector<std::unique_ptr<Station>> stations;
    size_t requestsOnAir = 0;
    size_t framesOnAir = 0;
    std::vector<uint64_t> sealsOnAir;   // when each Seal went
    std::vector<Aired> aired;

    Station& add(const std::string& call, uint32_t seed)
    {
        stations.push_back(std::make_unique<Station>(call, seed));
        return *stations.back();
    }

    // Ragtime closed and opened again: the station is made anew, its
    // broadcast parts read back from folder.
    Station& restart(size_t i, const std::string& folder, uint32_t seed)
    {
        std::string call = stations[i]->call();
        stations[i].reset();
        stations[i] = std::make_unique<Station>(call, seed);
        stations[i]->files.setPartsFolder(folder, now);
        return *stations[i];
    }

    void step()
    {
        for (auto& sp : stations)
        {
            Station& s = *sp;
            std::string lines = s.link.takeCommandOutput();
            std::string replies;
            size_t at = 0;
            while (at < lines.size())
            {
                size_t cr = lines.find('\r', at);
                std::string line = lines.substr(at, cr - at);
                at = cr + 1;
                if (line.compare(0, 10, "BCAST OPEN") == 0)
                    replies += "BCAST PORT " + std::to_string(GROUP_PORT) + "\r";
                else if (line.compare(0, 11, "BCAST MODE ") == 0)
                {
                    s.groupMode = line.substr(line.rfind(' ') + 1);
                    replies += "OK\r";
                }
                else if (line == "MODES")
                    replies += MODES;
                else
                    replies += "OK\r";
            }
            if (!replies.empty()) s.link.commandBytes(replies.data(), replies.size(), now);

            std::vector<uint8_t> out = s.link.takeKissOutput();
            std::vector<link::KissFrame> frames;
            s.kiss.feed(out.data(), out.size(), frames);
            if (frames.empty()) continue;
            std::string ptt = "PTT ON\rMODE qpsk-r1/3\r";
            s.link.commandBytes(ptt.data(), ptt.size(), now);
            for (auto& op : stations)
            {
                if (op.get() == &s) continue;
                auto snr = op->hearsAt.find(s.call());
                std::string from = "BCAST " + std::to_string(GROUP_PORT) + " HEARD " + s.call()
                                   + (snr != op->hearsAt.end() ? " " + std::to_string(snr->second) : std::string())
                                   + "\r";
                op->link.commandBytes(from.data(), from.size(), now);
            }
            for (const link::KissFrame& f : frames)
            {
                if (f.command != link::KISS_ACKMODE || f.payload.size() < 2) continue;
                std::vector<uint8_t> data(f.payload.begin() + 2, f.payload.end());
                CHECK(data.size() <= protocol::MAX_BROADCAST_FRAME);
                CHECK(!data.empty() && data[0] == protocol::FRAME_BYTE);
                framesOnAir++;
                std::vector<protocol::Record> records;
                protocol::decodeFrame(data, records);
                for (const protocol::Record& r : records)
                {
                    aired.push_back({s.groupMode, r});
                    if (r.type() == (uint8_t)protocol::RecordType::RepairRequest) requestsOnAir++;
                    if (r.type() == (uint8_t)protocol::RecordType::Seal) sealsOnAir.push_back(now);
                }
                for (auto& op : stations)
                {
                    if (op.get() == &s) continue;
                    std::vector<protocol::Record> kept;
                    for (protocol::Record r : records)
                    {
                        if (op->lose && op->lose(r)) continue;
                        kept.push_back(std::move(r));
                    }
                    if (kept.empty()) continue;
                    std::vector<uint8_t> heard = protocol::encodeFrame(kept);
                    std::vector<uint8_t> kiss = link::kissEncode(GROUP_PORT, heard);
                    op->link.kissBytes(kiss.data(), kiss.size(), now);
                }
                uint16_t tag = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
                std::vector<uint8_t> ack = link::kissEncodeAckMode(GROUP_PORT, tag, {});
                s.link.kissBytes(ack.data(), ack.size(), now);
            }
            std::string off = "PTT OFF\r";
            s.link.commandBytes(off.data(), off.size(), now);
        }
        for (auto& s : stations) s->events();
    }

    // The clock moves on by ms, a step at a time.
    void run(uint64_t ms, uint64_t stepMs = 250)
    {
        for (uint64_t t = 0; t < ms; t += stepMs)
        {
            now += stepMs;
            step();
            for (auto& s : stations) s->tick();
            step();
        }
    }
};

bool isType(const protocol::Record& r, protocol::RecordType type)
{
    return r.type() == (uint8_t)type;
}

// A file of n bytes of a pattern, in a scratch folder.
std::filesystem::path scratch()
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "ragtime-broadcast-files-test";
    std::filesystem::create_directories(dir);
    return dir;
}

std::string makeFile(const std::string& name, size_t n, uint32_t seed = 1)
{
    std::filesystem::path p = scratch() / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++)
    {
        x = x * 1103515245u + 12345u;
        out.put((char)(x >> 16));
    }
    return p.string();
}

std::vector<uint8_t> readAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::filesystem::path freshFolder(const std::string& name)
{
    std::filesystem::path dir = scratch() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// The file heard from a station; id 0 when there is none.
BroadcastFile fileFrom(const Station& s, const std::string& from)
{
    for (const BroadcastFile& f : s.files.files(now))
    {
        if (!f.outgoing && f.from == from) return f;
    }
    return BroadcastFile();
}

//-------------------------------------------------------------------------

void testAirtime()
{
    link::ModeInfo fsk = broadcast::findMode({}, "fsk16r25-r1/3");
    link::ModeInfo qpsk = broadcast::findMode({}, "qpsk-r1/3");
    CHECK(fsk.valid());
    CHECK(qpsk.valid());
    CHECK(!broadcast::findMode({}, "no-such-mode").valid());
    CHECK(broadcast::findMode({}, "", "qpsk-r1/3").valid());

    // docs/PLAN.md's table: fsk16r25-r1/3 about 16 s a one-codeword
    // burst and a 52 s window; qpsk-r1/3 about 3 s and 13 s.
    CHECK(broadcast::oneCodewordSeconds(fsk) > 15.0 && broadcast::oneCodewordSeconds(fsk) < 16.5);
    uint64_t wf = broadcast::listeningWindowMs(fsk);
    CHECK(wf > 49000 && wf < 54000);
    uint64_t wq = broadcast::listeningWindowMs(qpsk);
    CHECK(wq > 12000 && wq < 14000);

    // Pieces fill three FSK codewords, less Data2G's framing: COBS, the
    // zero ending it and the zero before a frame starting a codeword.
    CHECK_EQ(broadcast::pieceFrameBytes(fsk), 3 * 38 - 3);
    // Framed, a piece never spills into a codeword more: two 128-byte
    // codewords hold 253 bytes (256 framed), three 86-byte ones 254 (258).
    link::ModeInfo m128{"m128", 2400, 128, 32, 1.0, 10.0, {}};
    link::ModeInfo m86{"m86", 2400, 86, 32, 1.0, 10.0, {}};
    link::ModeInfo m278{"m278", 2400, 278, 32, 1.0, 10.0, {}};
    CHECK_EQ(broadcast::pieceFrameBytes(m128), 253);
    CHECK_EQ(broadcast::pieceFrameBytes(m86), 254);
    CHECK_EQ(broadcast::pieceFrameBytes(m278), (int)protocol::MAX_BROADCAST_FRAME);
    // One data codeword whatever its size.
    CHECK(std::fabs(broadcast::oneCodewordSeconds(m278) - (1.0 + 9.0 / 31)) < 1e-9);
    int bytes = broadcast::pieceBytesFor(fsk, "AG7EW");
    protocol::Record r;
    r.header.callsign = "AG7EW";
    protocol::FilePiece p;
    p.bytes.assign((size_t)bytes, 1);
    r.body = p;
    // The piece is even, so its frame may be a byte short.
    int frame = (int)protocol::encodeFrame({r}).size();
    CHECK(frame <= broadcast::pieceFrameBytes(fsk) && frame >= broadcast::pieceFrameBytes(fsk) - 1);
    CHECK(broadcast::pieceBytesFor(qpsk, "AG7EW") > bytes);
    CHECK((int)protocol::encodeFrame({r}).size() <= (int)protocol::MAX_BROADCAST_FRAME);

    // One frame a burst in FSK; several in OFDM.
    std::vector<std::vector<uint8_t>> frames(5, std::vector<uint8_t>(100, 0xA0));
    CHECK_EQ(broadcast::packBursts(frames, fsk).size(), (size_t)5);
    CHECK(broadcast::packBursts(frames, qpsk).size() < 5);

    // Records share frames while they fit.
    std::vector<protocol::Record> small;
    for (int i = 0; i < 30; i++)
    {
        protocol::Record t;
        t.header.callsign = "AG7EW";
        t.header.seq = (uint32_t)i + 1;
        protocol::Text text;
        text.text = "hello there";
        t.body = text;
        small.push_back(t);
    }
    auto packed = broadcast::packFrames(small);
    CHECK(packed.size() > 1 && packed.size() < 10);
    for (const auto& f : packed) CHECK(f.size() <= protocol::MAX_BROADCAST_FRAME);

    // The estimate box.
    broadcast::FileEstimate e = broadcast::estimateFile(7000, "FRED.TXT", "AG7EW", qpsk);
    CHECK(e.pieces > 0 && e.pieces * e.pieceBytes >= 7000);
    CHECK(e.airSeconds > 0.0 && e.wallSeconds >= e.airSeconds);
    CHECK_EQ(e.repairsMs, broadcast::MAX_OPEN_MS);
    broadcast::FileEstimate slow = broadcast::estimateFile(7000, "FRED.TXT", "AG7EW", fsk);
    CHECK(slow.airSeconds > e.airSeconds * 3);
}

// Three texts, then quiet: one seal lists them by their record digests.
void testSealAfterQuiet()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    air.run(2000);
    CHECK(!a.seals.nextSealMs());
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    a.chat.send("three", "", now);
    air.run(10000);
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)0);
    // The countdown: the over's quiet from its last item.
    std::optional<uint64_t> due = a.seals.nextSealMs();
    CHECK(due && *due > now && *due <= now + Seals::QUIET_MS);
    air.run(Seals::QUIET_MS);
    CHECK(!a.seals.nextSealMs());
    CHECK_EQ(a.seals.sealsSent(), (size_t)1);
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)1);

    std::vector<protocol::Record> texts;
    const protocol::Seal* seal = nullptr;
    for (const protocol::Record& r : b.heard)
    {
        if (isType(r, protocol::RecordType::Text)) texts.push_back(r);
        if (const protocol::Seal* s = r.as<protocol::Seal>()) seal = s;
    }
    CHECK(seal != nullptr);
    if (seal != nullptr && texts.size() == 3)
    {
        CHECK_EQ(seal->number, (uint16_t)1);
        CHECK(seal->previous == protocol::Digest{});
        CHECK_EQ(seal->firstItem, texts[0].header.seq);
        CHECK_EQ(seal->items.size(), (size_t)3);
        for (size_t i = 0; i < 3 && i < seal->items.size(); i++)
            CHECK(seal->items[i] == protocol::recordDigest(texts[i]));
    }
    air.run(60000);
    CHECK_EQ(air.requestsOnAir, (size_t)0);
    CHECK_EQ(a.seals.rounds(), (size_t)0);
    // Nobody asked, so nobody said they hold it: sealed, no more.
    for (const protocol::Record& t : texts)
    {
        Seals::ItemMark m = a.seals.ourMark(t.header.seq);
        CHECK(m.mark == Seals::Mark::Sealed && m.holders.empty());
        CHECK(b.seals.theirMark("AG7EW", t.header.seq).mark == Seals::Mark::Sealed);
    }

    // The chain: the next seal names this one.
    Seals::Chain chain = a.seals.chain();
    CHECK_EQ(chain.number, (uint16_t)1);
    a.chat.send("four", "", now);
    air.run(Seals::QUIET_MS + 5000);
    const protocol::Seal* second = nullptr;
    for (const protocol::Record& r : b.heard)
    {
        if (const protocol::Seal* s = r.as<protocol::Seal>()) second = s;
    }
    CHECK(second != nullptr && second->number == 2 && second->previous == chain.digest);
}

// A listener that missed a text asks for it after the seal and gets it.
void testRepairText()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    bool lost = false;
    b.lose = [&](protocol::Record& r) {
        const protocol::Text* t = r.as<protocol::Text>();
        if (t != nullptr && t->text == "two" && !lost)
        {
            lost = true;
            return true;
        }
        return false;
    };
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    a.chat.send("three", "", now);
    air.run(5000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)2);
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)3);
    // In its place, not at the bottom (Jeff, 2026-10-24).
    std::vector<std::string> order;
    for (const broadcast::Message& m : b.chat.messages())
    {
        if (m.from == "AG7EW") order.push_back(m.text);
    }
    CHECK(order == std::vector<std::string>({"one", "two", "three"}));
    CHECK_EQ(b.seals.requestsSent(), (size_t)1);
    CHECK_EQ(a.seals.requestsHeard(), (size_t)1);
    CHECK_EQ(a.seals.rounds(), (size_t)1);
    // The resend ended with the seal again; nothing more is lacked, so
    // the next window is quiet and the repairs end.
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)2);
    air.run(120000);
    CHECK_EQ(air.requestsOnAir, (size_t)1);
    CHECK_EQ(a.seals.rounds(), (size_t)1);
}

// Two listeners miss the same text: one asks, the other hears it and
// keeps quiet.
void testSuppression()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 7);
    Station& c = air.add("W1AW", 99);
    uint64_t start = now;
    auto loseTwo = [start](protocol::Record& r) {
        const protocol::Text* t = r.as<protocol::Text>();
        return t != nullptr && t->text == "two" && now < start + 30000;
    };
    b.lose = loseTwo;
    c.lose = loseTwo;
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)2);
    CHECK_EQ(c.textsFrom("AG7EW"), (size_t)2);
    CHECK_EQ(air.requestsOnAir, (size_t)1);
    CHECK_EQ(b.seals.requestsSent() + c.seals.requestsSent(), (size_t)1);
    CHECK_EQ(b.seals.requestsHeld() + c.seals.requestsHeld(), (size_t)1);
    CHECK_EQ(a.seals.rounds(), (size_t)1);
}

// A station that tunes in and hears only the seal doesn't ask for the
// over it missed.
void testLateJoiner()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    b.lose = [](protocol::Record& r) { return r.as<protocol::Text>() != nullptr; };
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)1);
    CHECK_EQ(air.requestsOnAir, (size_t)0);
}

// A listener that missed a seal (and a text under it) hears the next
// seal name it, asks for the seal, then for the text.
void testMissedSeal()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    uint64_t start = now;
    b.lose = [start](protocol::Record& r) {
        const protocol::Text* t = r.as<protocol::Text>();
        if (t != nullptr && t->text == "two") return true;
        const protocol::Seal* s = r.as<protocol::Seal>();
        return s != nullptr && s->number == 1 && now < start + 200000;
    };
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    air.run(Seals::QUIET_MS + 30000);
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)0);
    CHECK_EQ(air.requestsOnAir, (size_t)0);
    // A later over.
    now = std::max(now, start + 200000);
    a.chat.send("three", "", now);
    air.run(Seals::QUIET_MS + 120000);
    CHECK(b.heardFrom("AG7EW", protocol::RecordType::Seal) >= 2);
    CHECK(b.seals.requestsSent() >= 1);
    // "two" itself is lost each time it goes as a line, but parity over it
    // comes too and rebuilds it.
    bool sealOne = false;
    for (const protocol::Record& r : b.heard)
    {
        const protocol::Seal* s = r.as<protocol::Seal>();
        if (s != nullptr && s->number == 1) sealOne = true;
    }
    CHECK(sealOne);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)3);
    CHECK(b.seals.itemsRebuilt() >= 1);
}

// A file, received automatically by one station, on request by another,
// and repaired for a third that lost pieces.
void testFiles()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    Station& c = air.add("W1AW", 3);
    Station& d = air.add("G4XYZ", 4);
    std::filesystem::path bDir = freshFolder("b");
    std::filesystem::path cDir = freshFolder("c");
    std::filesystem::path dDir = freshFolder("d");
    b.files.setAutoReceive(bDir.string());
    d.files.setAutoReceive(dDir.string());
    // Each listed piece is lost the first time it goes.
    auto losePieces = [](std::vector<uint16_t> which) {
        auto lost = std::make_shared<std::set<uint16_t>>();
        return [which, lost](protocol::Record& r) {
            const protocol::FilePiece* p = r.as<protocol::FilePiece>();
            if (p == nullptr || std::find(which.begin(), which.end(), p->index) == which.end()) return false;
            return lost->insert(p->index).second;
        };
    };
    d.lose = losePieces({3, 7});
    // More than the parity sent for D's gaps will fill.
    c.lose = losePieces({1, 5, 9, 11});
    air.run(2000);

    std::string path = makeFile("fred.txt", 3000);
    std::string error;
    uint64_t id = a.files.send(path, "qpsk-r1/3", now, error);
    CHECK(id != 0);
    CHECK(error.empty());
    std::vector<files::FileRow> rows = a.files.rows(now);
    CHECK_EQ(rows.size(), (size_t)1);
    if (!rows.empty())
    {
        CHECK(rows[0].via == files::Via::Broadcast);
        CHECK_EQ(rows[0].peer, std::string(files::BROADCAST_PEER));
        CHECK(rows[0].outgoing);
        CHECK(rows[0].look == files::FileRow::Look::Moving);
    }

    air.run(60000);
    std::vector<uint8_t> want = readAll(path);
    // B had every piece and saved it at once, before any seal.
    BroadcastFile fb = fileFrom(b, "AG7EW");
    CHECK(fb.id != 0 && fb.state == FileState::Saved);
    if (fb.id != 0) CHECK(readAll(fb.path) == want);
    // C waits for the operator.
    BroadcastFile fc = fileFrom(c, "AG7EW");
    CHECK(fc.id != 0 && fc.state == FileState::Heard);
    // D lost pieces 3 and 7 and asked for them after the seal.
    BroadcastFile fd = fileFrom(d, "AG7EW");
    CHECK(fd.id != 0 && fd.state == FileState::Saved);
    if (fd.id != 0) CHECK(readAll(fd.path) == want);
    CHECK_EQ(d.seals.requestsSent(), (size_t)1);
    CHECK_EQ(c.seals.requestsSent(), (size_t)0);
    // The share is open for repairs.
    rows = a.files.rows(now);
    CHECK_EQ(rows.size(), (size_t)1);
    if (!rows.empty())
    {
        CHECK(rows[0].look == files::FileRow::Look::Repairing);
        CHECK(rows[0].expiresAtMs > now);
    }

    // C presses Receive late: what came for D fills some of its gaps, its
    // request opens a window, and parity for the rest comes.
    size_t rounds = a.seals.rounds();
    if (fc.id != 0) CHECK(c.files.receive(fc.id, (cDir / "fred-c.txt").string(), now, error));
    air.run(60000);
    fc = fileFrom(c, "AG7EW");
    CHECK(fc.id != 0 && fc.state == FileState::Saved);
    if (fc.id != 0) CHECK_EQ(fc.path, (cDir / "fred-c.txt").string());
    CHECK(readAll((cDir / "fred-c.txt").string()) == want);
    CHECK_EQ(a.seals.rounds(), rounds + 1);

    // The Files tab's log.
    std::vector<files::FinishedFile> done = b.files.takeFinished();
    CHECK_EQ(done.size(), (size_t)1);
    if (!done.empty())
    {
        CHECK(done[0].result == files::FinishedFile::Result::Saved);
        CHECK_EQ(done[0].peer, std::string("AG7EW"));
        CHECK(!done[0].outgoing);
    }

    // Closing repairs ends the share: Sent.
    CHECK(a.files.closeRepairs(id));
    done = a.files.takeFinished();
    CHECK_EQ(done.size(), (size_t)1);
    if (!done.empty()) CHECK(done[0].result == files::FinishedFile::Result::Sent);
    CHECK(a.files.rows(now).empty());
}

// The share's sensitivity window, and the operator's hand on it.
void testOpenWindow()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    air.add("KC2G", 2);
    air.run(2000);
    std::string error;
    uint64_t id = a.files.send(makeFile("small.bin", 500), "qpsk-r1/3", now, error);
    CHECK(id != 0);
    air.run(30000);
    BroadcastFile f;
    CHECK(a.files.find(id, now, f));
    CHECK(f.state == FileState::Repairing);
    Seals::SealView seal = a.seals.sealOf(f.seq, now);
    CHECK(seal.sealed && seal.open);
    // The newest seal stays open for two hours at most.
    CHECK(seal.openUntilMs > now + broadcast::MAX_OPEN_MS - 60000);
    // Kept open longer than that.
    uint64_t until = seal.openUntilMs + 30 * 60 * 1000;
    CHECK(a.files.keepOpen(id, until));
    air.run(broadcast::MAX_OPEN_MS, 60000);
    CHECK(a.files.find(id, now, f));
    CHECK(f.state == FileState::Repairing);
    CHECK(f.keptOpen);
    CHECK_EQ(f.repairsUntilMs, until);
    air.run(31 * 60 * 1000, 60000);
    CHECK(a.files.find(id, now, f));
    CHECK(f.state == FileState::Sent);
}

// A spoiled piece fails the digest: everything is asked for again, once.
void testDigestMismatch()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("spoiled");
    b.files.setAutoReceive(dir.string());
    bool spoiled = false;
    b.lose = [&](protocol::Record& r) {
        protocol::FilePiece* p = r.as<protocol::FilePiece>();
        if (p != nullptr && p->index == 1 && !spoiled)
        {
            spoiled = true;
            p->bytes[0] ^= 0xFF;
        }
        return false;
    };
    air.run(2000);
    std::string error;
    std::string path = makeFile("spoil.bin", 1200, 9);
    CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
    air.run(120000);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Saved);
    if (f.id != 0) CHECK(readAll(f.path) == readAll(path));
    CHECK(b.seals.requestsSent() >= 1);
}

// The announce lost, and only the pieces heard: they show the listener was
// there, so it asks for the announce and then for the rest.
void testLostAnnounce()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("unannounced");
    b.files.setAutoReceive(dir.string());
    bool lost = false;
    b.lose = [&lost](protocol::Record& r) {
        if (r.as<protocol::FileAnnounce>() == nullptr || lost) return false;
        lost = true;
        return true;
    };
    air.run(2000);
    std::string error;
    std::string path = makeFile("unannounced.bin", 1500, 4);
    CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
    air.run(180000);
    CHECK(lost);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Saved);
    if (f.id != 0) CHECK(readAll(f.path) == readAll(path));
    CHECK(b.seals.requestsSent() >= 1);
}

// The file's seal lost, or the request for its repair: the seal goes
// again after a quiet window, and the listener asks again.
void testLostSealOrRequest()
{
    for (int lostRequest = 0; lostRequest < 2; lostRequest++)
    {
        Air air;
        Station& a = air.add("AG7EW", 1);
        Station& b = air.add("KC2G", 2);
        std::filesystem::path dir = freshFolder(lostRequest ? "lostrequest" : "lostseal");
        b.files.setAutoReceive(dir.string());
        bool sealLost = false, pieceLost = false, requestLost = false;
        b.lose = [&](protocol::Record& r) {
            const protocol::FilePiece* p = r.as<protocol::FilePiece>();
            if (p != nullptr && p->index == 2 && !pieceLost) return pieceLost = true;
            if (!lostRequest && r.as<protocol::Seal>() != nullptr && !sealLost) return sealLost = true;
            return false;
        };
        a.lose = [&](protocol::Record& r) {
            if (lostRequest && r.as<protocol::RepairRequest>() != nullptr && !requestLost) return requestLost = true;
            return false;
        };
        air.run(2000);
        std::string error;
        std::string path = makeFile("flushed.bin", 1500, 6);
        CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
        air.run(240000);
        CHECK(pieceLost && (lostRequest ? requestLost : sealLost));
        BroadcastFile f = fileFrom(b, "AG7EW");
        CHECK(f.id != 0 && f.state == FileState::Saved);
        if (f.id != 0) CHECK(readAll(f.path) == readAll(path));
        CHECK_EQ(a.seals.rounds(), (size_t)1);
    }
}

// Every copy of a file's seal is lost (the first, and the two that go
// again after quiet windows), and so are a piece and, in one run, the
// announce. SEAL_LATE_MS after the last of the file came, the listener
// asks for the seal over it under seal 0, the seal goes again, and the
// repairs follow.
void testAllSealsLost()
{
    for (int lostAnnounce = 0; lostAnnounce < 2; lostAnnounce++)
    {
        Air air;
        Station& a = air.add("AG7EW", 1);
        Station& b = air.add("KC2G", 2);
        b.files.setAutoReceive(freshFolder(lostAnnounce ? "allseals-announce" : "allseals").string());
        uint64_t start = now;
        b.lose = [&](protocol::Record& r) {
            if (r.as<protocol::Seal>() != nullptr) return now < start + 150000;
            if (lostAnnounce && r.as<protocol::FileAnnounce>() != nullptr) return now < start + 150000;
            const protocol::FilePiece* p = r.as<protocol::FilePiece>();
            return p != nullptr && p->index == 2 && now < start + 150000;
        };
        size_t sealZero = 0;
        a.lose = [&](protocol::Record& r) {
            const protocol::RepairRequest* q = r.as<protocol::RepairRequest>();
            if (q != nullptr && q->seal == 0)
            {
                sealZero++;
                CHECK(q->wantSeal);
                CHECK_EQ(q->files.size(), (size_t)1);
            }
            return false;
        };
        air.run(2000);
        std::string error;
        std::string path = makeFile("sealless.bin", 1500, 8);
        CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
        air.run(150000);
        CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)0);
        CHECK_EQ(sealZero, (size_t)0);
        air.run(Seals::SEAL_LATE_MS + 120000);
        CHECK_EQ(sealZero, (size_t)1);
        CHECK(b.heardFrom("AG7EW", protocol::RecordType::Seal) >= 1);
        BroadcastFile f = fileFrom(b, "AG7EW");
        CHECK(f.id != 0 && f.state == FileState::Saved);
        if (f.id != 0) CHECK(readAll(f.path) == readAll(path));
    }
}

// The same, with the sender's repairs closed: the request for the seal is
// refused, naming the file, and the listener's file ends Incomplete with
// the reason instead of asking on.
void testAllSealsLostRefused()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    b.files.setAutoReceive(freshFolder("allseals-refused").string());
    b.lose = [](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return r.as<protocol::Seal>() != nullptr || (p != nullptr && p->index == 2);
    };
    air.run(2000);
    std::string error;
    std::string path = makeFile("closed.bin", 1500, 9);
    uint64_t id = a.files.send(path, "qpsk-r1/3", now, error);
    CHECK(id != 0);
    air.run(150000);
    CHECK(a.files.closeRepairs(id));
    air.run(Seals::SEAL_LATE_MS + 60000);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Incomplete);
    CHECK(f.refusal.has_value());
    CHECK_EQ(a.seals.refusalsSent(), (size_t)1);
    size_t asked = b.seals.requestsSent();
    air.run(4 * Seals::SEAL_LATE_MS);
    CHECK_EQ(b.seals.requestsSent(), asked);
}

// The sender stops a share part way: what went is sealed, and a listener
// that asks for the rest on the group gets all of it there, with nothing
// for the sender's operator to do. Closing repairs ends it as Cancelled.
void testStoppedShare()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("stopped");
    b.files.setAutoReceive(dir.string());
    air.run(2000);
    std::string error;
    std::string path = makeFile("stopped.bin", 2500, 8);
    uint64_t id = a.files.send(path, "qpsk-r1/3", now, error);
    CHECK(id != 0);
    BroadcastFile mine;
    for (int i = 0; i < 2000 && a.files.find(id, now, mine) && mine.held < 3; i++) air.run(250);
    CHECK(mine.held >= 3 && mine.held < mine.pieces);
    CHECK(a.files.cancel(id));
    CHECK(a.files.find(id, now, mine));
    CHECK(mine.state == FileState::Repairing && mine.stopped);
    air.run(240000);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Saved);
    if (f.id != 0) CHECK(readAll(f.path) == readAll(path));
    CHECK(b.seals.requestsSent() >= 1);
    CHECK(a.files.find(id, now, mine) && mine.state == FileState::Repairing);
    CHECK(a.files.closeRepairs(id));
    CHECK(a.files.find(id, now, mine) && mine.state == FileState::Cancelled);
    std::vector<files::FinishedFile> done = a.files.takeFinished();
    CHECK(!done.empty() && done.back().result == files::FinishedFile::Result::Cancelled);

    // Stopped before anything went: it just ends.
    uint64_t second = a.files.send(makeFile("never.bin", 2500, 9), "qpsk-r1/3", now, error);
    CHECK(a.files.cancel(second));
    CHECK(a.files.find(second, now, mine) && mine.state == FileState::Cancelled);
}

// The listener stops asking: the file goes back to Incomplete with what
// came kept, no requests go, and Ask again carries on to Saved. Reject
// (ignore) throws it away and is logged as Declined.
void testStopAndAskAgain()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("askagain");
    b.files.setAutoReceive(dir.string());
    b.lose = [](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && p->index >= 4;
    };
    air.run(2000);
    std::string error;
    std::string path = makeFile("askagain.bin", 2500, 10);
    CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
    // The first pass, with B hearing only its first pieces.
    for (int i = 0; i < 4000; i++)
    {
        BroadcastFile f = fileFrom(b, "AG7EW");
        if (f.id != 0 && f.held >= 4) break;
        air.run(250);
    }
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Receiving);
    CHECK(b.files.stop(f.id, now));
    f = fileFrom(b, "AG7EW");
    CHECK(f.state == FileState::Incomplete && f.stopped && f.held == 4);
    size_t asked = b.seals.requestsSent();
    air.run(180000);
    CHECK_EQ(b.seals.requestsSent(), asked);
    f = fileFrom(b, "AG7EW");
    CHECK(f.state == FileState::Incomplete);
    CHECK(b.files.takeFinished().empty());

    // Ask again: it hears the rest now.
    b.lose = nullptr;
    CHECK(b.files.receive(f.id, "", now, error));
    air.run(180000);
    f = fileFrom(b, "AG7EW");
    CHECK(f.state == FileState::Saved);
    if (f.state == FileState::Saved) CHECK(readAll(f.path) == readAll(path));

    // Reject: thrown away, logged as Declined.
    b.files.takeFinished();
    CHECK(a.files.send(makeFile("reject.bin", 1200, 11), "qpsk-r1/3", now, error) != 0);
    air.run(3000);
    BroadcastFile r;
    for (const BroadcastFile& x : b.files.files(now))
    {
        if (!x.outgoing && x.name == "reject.bin") r = x;
    }
    CHECK(r.id != 0 && b.files.ignore(r.id));
    std::vector<files::FinishedFile> done = b.files.takeFinished();
    CHECK(!done.empty() && done.back().result == files::FinishedFile::Result::Declined);
}

// Requests that bring nothing: the listener gives up asking, and the file
// goes to Incomplete with what came kept, rather than looking as if it is
// still being repaired. Ask again starts asking afresh under the seal (a
// request goes, and the rest comes); once the seal is over 2 hours old
// there is nothing to ask under, and the file says so (sealAged).
void testGiveUpAndAskAgain()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("giveup");
    b.files.setAutoReceive(dir.string());
    bool deaf = true;
    b.lose = [&deaf](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return deaf && p != nullptr && p->index >= 3;
    };
    air.run(2000);
    std::string error;

    // The sender stops part way, as in Jeff's test, and B hears none of
    // the repairs.
    auto share = [&](const std::string& name, uint8_t pattern) {
        std::string path = makeFile(name, 2500, pattern);
        uint64_t id = a.files.send(path, "qpsk-r1/3", now, error);
        BroadcastFile mine;
        for (int i = 0; i < 2000 && a.files.find(id, now, mine) && mine.held < 5; i++) air.run(250);
        CHECK(a.files.cancel(id));
        return path;
    };
    auto fileNamed = [&](const std::string& name) {
        BroadcastFile f;
        for (const BroadcastFile& x : b.files.files(now))
        {
            if (!x.outgoing && x.name == name) f = x;
        }
        return f;
    };

    // Until it gives up: repair bursts leave room to answer between them
    // (HostLink::YIELD_GAP_MS), so the rounds take a while.
    auto untilIncomplete = [&](const std::string& name) {
        for (int i = 0; i < 30 * 60 && fileNamed(name).state != FileState::Incomplete; i++) air.run(1000, 500);
    };
    std::string path = share("giveup.bin", 12);
    untilIncomplete("giveup.bin");
    BroadcastFile f = fileNamed("giveup.bin");
    CHECK(f.id != 0 && f.state == FileState::Incomplete && !f.stopped);
    CHECK(f.sealed && !f.sealAged && f.held == 3);
    size_t asked = b.seals.requestsSent();
    CHECK(asked >= (size_t)Seals::MAX_ASKS);
    air.run(3 * 60 * 1000, 500);
    CHECK_EQ(b.seals.requestsSent(), asked);

    // Ask again on a clear link: a request goes, and the rest comes.
    deaf = false;
    CHECK(b.files.receive(f.id, "", now, error));
    air.run(Seals::LATE_SPREAD_MS + 1000);
    CHECK(b.seals.requestsSent() > asked);
    air.run(5 * 60 * 1000, 500);
    f = fileNamed("giveup.bin");
    CHECK(f.state == FileState::Saved);
    if (f.state == FileState::Saved) CHECK(readAll(f.path) == readAll(path));

    // Given up, and then the seal ages out: Ask again has nothing to ask
    // under, and nothing goes.
    deaf = true;
    share("aged.bin", 13);
    untilIncomplete("aged.bin");
    f = fileNamed("aged.bin");
    CHECK(f.state == FileState::Incomplete && f.sealed);
    // Two hours after the last of its seals went (it goes again a time or
    // two after the last round).
    air.run(broadcast::MAX_OPEN_MS + 10 * 60 * 1000, 5000);
    f = fileNamed("aged.bin");
    CHECK(f.state == FileState::Incomplete && !f.sealed && f.sealAged);
    asked = b.seals.requestsSent();
    CHECK(b.files.receive(f.id, "", now, error));
    air.run(60000);
    CHECK_EQ(b.seals.requestsSent(), asked);
    f = fileNamed("aged.bin");
    CHECK(f.state == FileState::Incomplete && f.held == 3);
}

// The seal badge. Ours: Awaiting until the seal goes; Repairing while a
// station's request for it is being answered; Held by the stations whose
// requests under the seal didn't ask for it; Sealed when nobody has said.
// Theirs: Sealed when the seal's digest matches what we hold, Mismatch
// when it doesn't, until a good copy comes.
void testSealMarks()
{
    using Mark = Seals::Mark;
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    Station& c = air.add("W1AW", 3);
    bool lost = false, spoilt = false;
    b.lose = [&](protocol::Record& r) {
        const protocol::Text* t = r.as<protocol::Text>();
        if (t != nullptr && t->text == "two" && !lost) return lost = true;
        return false;
    };
    c.lose = [&](protocol::Record& r) {
        protocol::Text* t = r.as<protocol::Text>();
        if (t != nullptr && t->text == "three" && !spoilt)
        {
            t->text = "thr33";
            spoilt = true;
        }
        return false;
    };
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    a.chat.send("three", "", now);
    air.run(5000);
    auto seqOf = [&](const std::string& text) {
        for (const broadcast::Message& m : a.chat.messages())
        {
            if (m.ours && m.text == text) return m.seq;
        }
        return (uint32_t)0;
    };
    uint32_t one = seqOf("one"), two = seqOf("two"), three = seqOf("three");
    CHECK(one != 0 && two == one + 1 && three == one + 2);
    CHECK(a.seals.ourMark(one).mark == Mark::Awaiting);
    CHECK(a.seals.ourMark(three).mark == Mark::Awaiting);

    // The seal goes; B asks for "two", so it holds the other two.
    for (int i = 0; i < 2000 && a.seals.requestsHeard() == 0; i++) air.run(250);
    CHECK(a.seals.requestsHeard() >= 1);
    Seals::ItemMark m = a.seals.ourMark(two);
    CHECK(m.mark == Mark::Repairing && m.sealedMs != 0);
    CHECK(m.wanting.size() >= 1 && m.wanting[0] == "KC2G");
    m = a.seals.ourMark(one);
    CHECK(m.mark == Mark::Held && !m.holders.empty() && m.holders[0] == "KC2G");
    // W1AW's spoilt copy of "three" doesn't match the seal.
    CHECK(c.seals.theirMark("AG7EW", three).mark == Mark::Mismatch);
    CHECK(c.seals.theirMark("AG7EW", one).mark == Mark::Sealed);

    air.run(Seals::QUIET_MS + 120000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)3);
    // Sent again, and KC2G hasn't asked since: nothing claimed for it.
    // W1AW's request (for "three") showed it holds "two".
    m = a.seals.ourMark(two);
    CHECK(m.mark == Mark::Held && m.wanting.empty());
    CHECK(m.sentAgainFor == std::vector<std::string>{"KC2G"});
    CHECK(m.holders == std::vector<std::string>{"W1AW"});
    m = a.seals.ourMark(three);
    CHECK(m.mark == Mark::Held);
    CHECK(std::find(m.holders.begin(), m.holders.end(), "KC2G") != m.holders.end());
    CHECK(b.seals.theirMark("AG7EW", two).mark == Mark::Sealed);
    CHECK(c.seals.theirMark("AG7EW", three).mark == Mark::Sealed);

    // Two hours on, the seals are pruned and forgotten: the marks stay.
    air.run(broadcast::MAX_OPEN_MS + 60000, 5000);
    CHECK(a.seals.ourMark(one).mark == Mark::Held);
    CHECK(a.seals.ourMark(two).holders == std::vector<std::string>{"W1AW"});
    CHECK(b.seals.theirMark("AG7EW", one).mark == Mark::Sealed);
}

// Many pieces lost: the request is cut to fit a frame, and the rest come
// in later rounds.
void testManyLost()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("many");
    b.files.setAutoReceive(dir.string());
    std::set<uint16_t> lost;
    b.lose = [&lost](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && p->index % 2 == 0 && lost.insert(p->index).second;
    };
    air.run(2000);
    std::string error;
    std::string path = makeFile("big.bin", 60000, 5);
    CHECK(a.files.send(path, "w48-16qam-r1/2", now, error) != 0);
    air.run(15 * 60 * 1000, 500);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Saved);
    if (f.id != 0) CHECK(readAll(f.path) == readAll(path));
    // 135 pieces lost, every other one: one request asks for parity
    // enough for them all, and one round brings it.
    CHECK_EQ(b.seals.requestsSent(), (size_t)1);
    CHECK_EQ(a.seals.rounds(), (size_t)1);
}

// Loses a share of what from sends, at random.
std::function<bool(protocol::Record&)> loseShare(const std::string& from, double share, uint32_t seed)
{
    auto rng = std::make_shared<std::mt19937>(seed);
    return [from, share, rng](protocol::Record& r) {
        if (r.header.callsign != from) return false;
        return std::bernoulli_distribution(share)(*rng);
    };
}

// The path Jeff sees across the US: one band for everyone leaves some
// stations losing most of every burst. Parity, asked for with the loss
// allowed for, gets the file to all of them in a round or two, and no
// more repair pieces go than the neediest station wants.
void testHeavyLossFile()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    std::vector<Station*> listeners;
    const char* calls[] = {"KC2G", "W1AW", "G4XYZ", "VK3ABC", "K7RA"};
    double losses[] = {0.6, 0.6, 0.1, 0.1, 0.1};
    for (int i = 0; i < 5; i++)
    {
        Station& st = air.add(calls[i], (uint32_t)(10 + i));
        st.lose = loseShare("AG7EW", losses[i], (uint32_t)(100 + i));
        st.files.setAutoReceive(freshFolder(std::string("heavy-") + calls[i]).string());
        listeners.push_back(&st);
    }
    air.run(2000);
    std::string error;
    std::string path = makeFile("heavy.bin", 6000, 9);
    CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
    air.run(60 * 60 * 1000, 500);
    std::vector<uint8_t> want = readAll(path);
    for (Station* st : listeners)
    {
        BroadcastFile f = fileFrom(*st, "AG7EW");
        CHECK(f.id != 0 && f.state == FileState::Saved);
        if (f.state == FileState::Saved) CHECK(readAll(f.path) == want);
    }
    BroadcastFile ours;
    for (const BroadcastFile& f : a.files.files(now)) ours = f;
    size_t repairs = 0;
    for (const Aired& on : air.aired)
    {
        const protocol::FilePiece* p = on.record.as<protocol::FilePiece>();
        if (p != nullptr && p->index >= ours.pieces) repairs++;
    }
    // Every repair piece is parity, and few rounds bring them.
    CHECK(repairs > 0);
    CHECK_EQ(repairs, a.seals.paritySent());
    CHECK(a.seals.rounds() <= 4);
}

// Sealed chat on the same path: three stations each lose half of the
// lines, different halves. Parity over the lines fills every gap, with
// the lines rebuilt from it shown as if heard.
void testHeavyLossChat()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    std::vector<Station*> listeners;
    const char* calls[] = {"KC2G", "W1AW", "G4XYZ"};
    for (int i = 0; i < 3; i++)
    {
        Station& st = air.add(calls[i], (uint32_t)(20 + i));
        auto rng = std::make_shared<std::mt19937>(200 + i);
        // Lines and their parity are lost; seals and the rest get through.
        st.lose = [rng](protocol::Record& r) {
            bool line = r.as<protocol::Text>() != nullptr || r.as<protocol::ItemParity>() != nullptr;
            return r.header.callsign == "AG7EW" && line && std::bernoulli_distribution(0.5)(*rng);
        };
        listeners.push_back(&st);
    }
    air.run(2000);
    for (int i = 0; i < 12; i++) a.chat.send("line " + std::to_string(i), "", now);
    air.run(Seals::QUIET_MS + 40 * 60 * 1000, 500);
    size_t rebuilt = 0;
    for (Station* st : listeners)
    {
        CHECK_EQ(st->textsFrom("AG7EW"), (size_t)12);
        rebuilt += st->seals.itemsRebuilt();
        for (const protocol::Record& r : st->rebuilt) CHECK(r.as<protocol::Text>() != nullptr);
    }
    CHECK(rebuilt > 0);
    size_t parity = 0;
    for (const Aired& on : air.aired) parity += on.record.as<protocol::ItemParity>() != nullptr ? 1 : 0;
    CHECK(parity > 0);
    CHECK(a.seals.rounds() <= 6);
}

// ItemParity heard before its seal waits for it; one whose lines don't
// match the seal rebuilds nothing.
void testItemParityBeforeSeal()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    // B misses "two" and the first seal; then hears everything.
    uint64_t start = now;
    b.lose = [start](protocol::Record& r) {
        const protocol::Text* t = r.as<protocol::Text>();
        if (t != nullptr && t->text == "two" && now < start + 100000) return true;
        return r.as<protocol::Seal>() != nullptr && now < start + 100000;
    };
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    a.chat.send("three", "", now);
    air.run(Seals::QUIET_MS + 10000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)2);
    // Parity over the three lines, by hand, as A would send it: heard
    // before the seal, it waits.
    std::vector<std::vector<uint8_t>> lines;
    for (const Aired& r : air.aired)
    {
        if (r.record.as<protocol::Text>() != nullptr && r.record.header.callsign == "AG7EW")
            lines.push_back(protocol::encodeRecord(r.record));
    }
    CHECK_EQ(lines.size(), (size_t)3);
    if (lines.size() != 3) return;
    size_t length = 0;
    for (const auto& l : lines) length = std::max(length, l.size());
    length = files::parity::symbolLength(length);
    std::vector<files::parity::Symbol> data;
    for (size_t i = 0; i < 3; i++) data.push_back({(uint16_t)i, &lines[i]});
    protocol::ItemParity p;
    p.seal = 1;
    p.items = {true, true, true};
    p.index = 0;
    p.bytes = files::parity::parityOf(protocol::ITEM_PARITY_BASE, data, length);
    protocol::Header h;
    h.callsign = "AG7EW";
    h.seq = 3;
    h.time = (uint32_t)(now / 1000);
    protocol::Record record;
    record.header = h;
    record.body = std::move(p);
    // A copy with a byte changed rebuilds nothing.
    protocol::Record bad = record;
    std::get<protocol::ItemParity>(bad.body).bytes[3] ^= 0x40;
    std::get<protocol::ItemParity>(bad.body).index = 1;
    b.seals.onRecord(bad, now);
    b.seals.onRecord(record, now);
    CHECK(b.seals.takeRebuilt().empty());
    // The next seal names the one missed, B asks for it, and once it
    // comes "two" is rebuilt from the good parity, without asking for it.
    a.chat.send("four", "", now);
    air.run(Seals::QUIET_MS + 120000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)4);
    CHECK_EQ(b.seals.itemsRebuilt(), (size_t)1);
    size_t twos = 0;
    for (const Aired& on : air.aired)
    {
        const protocol::Text* t = on.record.as<protocol::Text>();
        if (t != nullptr && t->text == "two") twos++;
    }
    CHECK_EQ(twos, (size_t)1);
}

// What can't go.
void testRefused()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    air.run(2000);
    std::string error;
    CHECK_EQ(a.files.send(makeFile("too-big.bin", 300 * 1024 + 1), "qpsk-r1/3", now, error), (uint64_t)0);
    CHECK(error.find("300 KB") != std::string::npos);
    CHECK_EQ(a.files.send((scratch() / "not-there.bin").string(), "qpsk-r1/3", now, error), (uint64_t)0);
    CHECK_EQ(a.files.send(makeFile("empty.bin", 0), "qpsk-r1/3", now, error), (uint64_t)0);
    // 300 KB in the slowest gear is far over two hours on the air.
    CHECK_EQ(a.files.send(makeFile("slow.bin", 300 * 1024), "fsk16r25-r1/3", now, error), (uint64_t)0);
    CHECK(error.find("two hours") != std::string::npos);
}

// Ignore throws away what came, and asks for nothing.
void testIgnore()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    b.lose = [](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && p->index == 0;
    };
    air.run(2000);
    std::string error;
    CHECK(a.files.send(makeFile("ignore.bin", 2000), "qpsk-r1/3", now, error) != 0);
    air.run(5000);
    std::vector<BroadcastFiles::Event> events = b.files.takeEvents();
    bool announced = false;
    for (const auto& e : events) announced = announced || e.type == BroadcastFiles::Event::Type::Announced;
    CHECK(announced);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Heard);
    if (f.id != 0) CHECK(b.files.ignore(f.id));
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(b.seals.requestsSent(), (size_t)0);
    std::vector<files::FinishedFile> done = b.files.takeFinished();
    CHECK_EQ(done.size(), (size_t)1);
    if (!done.empty()) CHECK(done[0].result == files::FinishedFile::Result::Declined);
}

// The .part files in a folder.
size_t partFiles(const std::filesystem::path& dir)
{
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    {
        if (e.path().extension() == ".part") n++;
    }
    return n;
}

BroadcastFile fileNamed(const Station& s, const std::string& name)
{
    for (const BroadcastFile& f : s.files.files(now))
    {
        if (!f.outgoing && f.name == name) return f;
    }
    return BroadcastFile();
}

// Broadcast parts kept on disk: Ragtime closes part way through a file,
// and opens again with the pieces it held and the seal to ask under, so
// the repair request still goes and the file comes whole. Saved, the
// part file goes.
void testPartsAcrossRestart()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    air.add("KC2G", 2);
    std::filesystem::path parts = freshFolder("restart-parts");
    std::filesystem::path dir = freshFolder("restart");
    air.stations[1]->files.setPartsFolder(parts.string(), now);
    air.stations[1]->files.setAutoReceive(dir.string());
    air.stations[1]->lose = [](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && p->index >= 3;
    };
    air.run(2000);
    std::string error;
    std::string path = makeFile("restart.bin", 2500, 14);
    CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
    // The first pass and its seal, with KC2G hearing three pieces.
    for (int i = 0; i < 4000; i++)
    {
        BroadcastFile f = fileNamed(*air.stations[1], "restart.bin");
        if (f.id != 0 && f.sealed) break;
        air.run(250);
    }
    air.run(BroadcastFiles::PART_SAVE_MS + 1000);
    BroadcastFile f = fileNamed(*air.stations[1], "restart.bin");
    CHECK(f.id != 0 && f.state == FileState::Receiving && f.sealed && f.held == 3);
    CHECK_EQ(partFiles(parts), (size_t)1);
    std::string saveTo = f.path;

    // Closed and opened again, on a clear link now.
    Station& b = air.restart(1, parts.string(), 3);
    f = fileNamed(b, "restart.bin");
    CHECK(f.id != 0 && f.state == FileState::Receiving && f.held == 3 && f.sealed);
    CHECK(!saveTo.empty() && f.path == saveTo);
    CHECK(b.chat.messages().empty());
    air.run(Seals::LATE_SPREAD_MS + 2000);
    CHECK(b.seals.requestsSent() > 0);
    air.run(5 * 60 * 1000, 500);
    f = fileNamed(b, "restart.bin");
    CHECK(f.state == FileState::Saved);
    if (f.state == FileState::Saved) CHECK(readAll(f.path) == readAll(path));
    CHECK_EQ(partFiles(parts), (size_t)0);
}

// A part stopped (or given up) is kept seven days, across restarts, and
// then goes. Files that aren't parts Ragtime wrote, or don't fit their
// announce, are passed over and deleted; others in the folder are left
// alone. Ignore (Reject) drops the part at once.
void testPartsExpire()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    air.add("KC2G", 2);
    std::filesystem::path parts = freshFolder("expire-parts");
    std::filesystem::path dir = freshFolder("expire");
    air.stations[1]->files.setPartsFolder(parts.string(), now);
    air.stations[1]->files.setAutoReceive(dir.string());
    air.stations[1]->lose = [](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && p->index >= 2;
    };
    air.run(2000);
    std::string error;
    CHECK(a.files.send(makeFile("expire.bin", 2500, 15), "qpsk-r1/3", now, error) != 0);
    for (int i = 0; i < 4000 && fileNamed(*air.stations[1], "expire.bin").held < 2; i++) air.run(250);
    BroadcastFile f = fileNamed(*air.stations[1], "expire.bin");
    CHECK(f.id != 0 && air.stations[1]->files.stop(f.id, now));
    air.run(BroadcastFiles::PART_SAVE_MS + 1000);
    CHECK_EQ(partFiles(parts), (size_t)1);

    // Junk beside it: not a part, a part that isn't one, and a cut copy.
    {
        std::ofstream(parts / "notes.txt") << "leave me";
        std::ofstream(parts / "junk.part") << "ragtime broadcast part 1\nannounce zz\n";
        std::filesystem::path real;
        for (const auto& e : std::filesystem::directory_iterator(parts))
        {
            if (e.path().filename() != "junk.part" && e.path().extension() == ".part") real = e.path();
        }
        std::ifstream in(real, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::ofstream(parts / "cut.part", std::ios::binary) << text.substr(0, text.size() / 3);
    }

    // Six days on: still there, stopped, with its pieces, and nothing to
    // ask under (the seal is long over).
    now += 6ull * 24 * 60 * 60 * 1000;
    Station& b = air.restart(1, parts.string(), 3);
    f = fileNamed(b, "expire.bin");
    CHECK(f.id != 0 && f.state == FileState::Incomplete && f.stopped && f.held == 2);
    CHECK_EQ(partFiles(parts), (size_t)1);
    CHECK(std::filesystem::exists(parts / "notes.txt"));
    air.run(60000);
    CHECK_EQ(b.seals.requestsSent(), (size_t)0);

    // Eight days on: gone, file and all.
    now += 2ull * 24 * 60 * 60 * 1000;
    Station& c = air.restart(1, parts.string(), 4);
    CHECK_EQ(fileNamed(c, "expire.bin").id, (uint64_t)0);
    CHECK_EQ(partFiles(parts), (size_t)0);

    // Ignore: a file heard and not received has its part dropped.
    c.files.setAutoReceive("");
    c.lose = nullptr;
    air.stations[1]->lose = [](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && p->index >= 2;
    };
    air.run(2000);
    CHECK(a.files.send(makeFile("ignored.bin", 2500, 16), "qpsk-r1/3", now, error) != 0);
    for (int i = 0; i < 4000 && fileNamed(c, "ignored.bin").held < 2; i++) air.run(250);
    air.run(BroadcastFiles::PART_SAVE_MS + 1000);
    f = fileNamed(c, "ignored.bin");
    CHECK(f.id != 0 && f.state == FileState::Heard);
    CHECK_EQ(partFiles(parts), (size_t)1);
    CHECK(c.files.ignore(f.id));
    CHECK_EQ(partFiles(parts), (size_t)0);
}

} // namespace

// Chat seals are off by default (Jeff, 2026-10-22: on a poor link a seal
// takes 30 to 60 s, and plain chat must never pay for it unasked): a chat
// over goes with no seal, and a file is still sealed, listing only its
// announce.
void testChatUnsealed()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    a.seals.setSealText(false);
    air.run(2000);
    a.chat.send("CQ CQ", "", now);
    a.chat.send("de AG7EW", "", now);
    air.run(10000);
    // No seal coming, so no countdown.
    CHECK(!a.seals.nextSealMs());
    air.run(Seals::QUIET_MS + 50000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)2);
    CHECK_EQ(a.seals.sealsSent(), (size_t)0);
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)0);
    CHECK_EQ(air.requestsOnAir, (size_t)0);
    for (const broadcast::Message& m : a.chat.messages())
    {
        if (m.ours) CHECK(a.seals.ourMark(m.seq).mark == Seals::Mark::None);
    }
    for (const broadcast::Message& m : b.chat.messages()) CHECK(b.seals.theirMark(m.from, m.seq).mark == Seals::Mark::None);

    // A file between two lines of chat: its seal lists the announce alone.
    a.chat.send("a file for the net", "", now);
    std::string error;
    CHECK(a.files.send(makeFile("net.txt", 600), "qpsk-r1/3", now, error) != 0);
    a.chat.send("73", "", now);
    air.run(Seals::QUIET_MS + 60000);
    const protocol::Seal* seal = nullptr;
    uint32_t announce = 0;
    for (const protocol::Record& r : b.heard)
    {
        if (const protocol::Seal* s = r.as<protocol::Seal>()) seal = s;
        if (isType(r, protocol::RecordType::FileAnnounce)) announce = r.header.seq;
    }
    CHECK_EQ(a.seals.sealsSent(), (size_t)1);
    CHECK(seal != nullptr && announce != 0);
    if (seal != nullptr)
    {
        CHECK_EQ(seal->firstItem, announce);
        CHECK_EQ(seal->items.size(), (size_t)1);
    }
    BroadcastFile file = fileFrom(b, "AG7EW");
    CHECK(file.id != 0);

    // Chat between two files leaves a gap in the numbers: two seals, made
    // together and keyed together, not one keying each.
    air.sealsOnAir.clear();
    CHECK(a.files.send(makeFile("one.txt", 300), "qpsk-r1/3", now, error) != 0);
    a.chat.send("and another", "", now);
    CHECK(a.files.send(makeFile("two.txt", 300), "qpsk-r1/3", now, error) != 0);
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(a.seals.sealsSent(), (size_t)3);
    // Each goes three times (a file's seal is repeated), always as a pair.
    CHECK_EQ(air.sealsOnAir.size(), (size_t)6);
    for (size_t i = 0; i + 1 < air.sealsOnAir.size(); i += 2) CHECK_EQ(air.sealsOnAir[i], air.sealsOnAir[i + 1]);
    CHECK_EQ(air.requestsOnAir, (size_t)0);
}

//-------------------------------------------------------------------------
// Repair gears and refusals
//-------------------------------------------------------------------------

// A ladder with made-up thresholds, most robust first.
std::vector<gearbox::Gear> ladder()
{
    std::vector<gearbox::Gear> gears(3);
    gears[0].number = 1;
    gears[0].mode = "fsk16r25-r1/3";
    gears[0].threshold = -10.0;
    gears[1].number = 2;
    gears[1].mode = "qpsk-r1/3";
    gears[1].threshold = -2.0;
    gears[2].number = 3;
    gears[2].mode = "w48-16qam-r1/2";
    gears[2].threshold = 10.0;
    return gears;
}

// Where a record went on the air: its index in air.aired, from index
// from, or -1.
long airedAt(const Air& air, size_t from, const std::function<bool(const protocol::Record&)>& is)
{
    for (size_t i = from; i < air.aired.size(); i++)
    {
        if (is(air.aired[i].record)) return (long)i;
    }
    return -1;
}

std::function<bool(const protocol::Record&)> textIs(const std::string& text)
{
    return [text](const protocol::Record& r) {
        const protocol::Text* t = r.as<protocol::Text>();
        return t != nullptr && t->text == text;
    };
}

std::function<bool(protocol::Record&)> loseOnce(const std::string& text)
{
    auto lost = std::make_shared<bool>(false);
    return [text, lost](protocol::Record& r) {
        const protocol::Text* t = r.as<protocol::Text>();
        if (t == nullptr || t->text != text || *lost) return false;
        *lost = true;
        return true;
    };
}

// Each item goes again in the gear the weakest station that asked for it
// can copy, never faster than it first went, fastest first, and the seal
// last in the slowest gear used.
void testRepairGear()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    Station& c = air.add("W1AW", 3);
    for (auto& st : air.stations) st->seals.setLadder(ladder());
    b.hearsAt["AG7EW"] = -5.0;  // less 3 dB: -8 dB, gear 1
    b.hearsAt["W1AW"] = 0.0;
    c.hearsAt["AG7EW"] = 20.0;  // gear 3, but "three" went in gear 2
    c.hearsAt["KC2G"] = 0.0;
    b.lose = loseOnce("two");
    c.lose = loseOnce("three");
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    a.chat.send("three", "", now);
    air.run(Seals::QUIET_MS + 120000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)3);
    CHECK_EQ(c.textsFrom("AG7EW"), (size_t)3);
    CHECK_EQ(b.seals.snrOf("AG7EW", now).value_or(99), -5.0);

    // The requests say how well each asker hears AG7EW.
    for (const Aired& on : air.aired)
    {
        const protocol::RepairRequest* r = on.record.as<protocol::RepairRequest>();
        if (r == nullptr) continue;
        if (on.record.header.callsign == "KC2G") CHECK(r->snr && *r->snr == -5.0);
        if (on.record.header.callsign == "W1AW") CHECK(r->snr && *r->snr == 20.0);
    }
    CHECK_EQ(a.seals.rounds(), (size_t)1);
    auto isSeal = [](const protocol::Record& r) { return r.as<protocol::Seal>() != nullptr; };
    long seal = airedAt(air, 0, isSeal);
    CHECK(seal >= 0);
    long three = airedAt(air, (size_t)seal, textIs("three"));
    long two = airedAt(air, (size_t)seal, textIs("two"));
    long again = airedAt(air, (size_t)seal + 1, isSeal);
    CHECK(three > seal && two > three && again > two);
    if (three > 0 && two > 0 && again > 0)
    {
        CHECK_EQ(air.aired[(size_t)three].mode, std::string("qpsk-r1/3"));
        CHECK_EQ(air.aired[(size_t)two].mode, std::string("fsk16r25-r1/3"));
        CHECK_EQ(air.aired[(size_t)again].mode, std::string("fsk16r25-r1/3"));
    }
}

// A request heard speaks for us only if its asker's gear is no faster
// than ours: the weak station asks too when the strong one asked first,
// and the line goes again once, in the weak one's gear.
void testSuppressionByGear(uint32_t weakSeed, uint32_t strongSeed, const std::string& firstAsker)
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", weakSeed);
    Station& c = air.add("W1AW", strongSeed);
    for (auto& st : air.stations) st->seals.setLadder(ladder());
    b.hearsAt["AG7EW"] = -5.0;
    c.hearsAt["AG7EW"] = 20.0;
    b.lose = loseOnce("two");
    c.lose = loseOnce("two");
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    air.run(Seals::QUIET_MS + 120000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)2);
    CHECK_EQ(c.textsFrom("AG7EW"), (size_t)2);
    long first = airedAt(air, 0, [](const protocol::Record& r) { return r.as<protocol::RepairRequest>() != nullptr; });
    CHECK(first >= 0);
    if (first >= 0) CHECK_EQ(air.aired[(size_t)first].record.header.callsign, firstAsker);
    // The strong station first: the weak one still asks. The weak one
    // first: it speaks for the strong one too.
    CHECK_EQ(air.requestsOnAir, firstAsker == "W1AW" ? (size_t)2 : (size_t)1);
    long seal = airedAt(air, 0, [](const protocol::Record& r) { return r.as<protocol::Seal>() != nullptr; });
    long two = airedAt(air, (size_t)seal, textIs("two"));
    CHECK(two > seal);
    if (two > 0) CHECK_EQ(air.aired[(size_t)two].mode, std::string("fsk16r25-r1/3"));
    CHECK_EQ(airedAt(air, (size_t)two + 1, textIs("two")), -1L);
}

// The slow gears' airtime in a round is capped: a weak station's file
// comes in several rounds, not one long one.
void testSlowCap(double capSeconds, bool oneRound)
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    a.seals.setLadder(ladder());
    a.seals.setSlowResendSeconds(capSeconds);
    b.hearsAt["AG7EW"] = -5.0;
    std::filesystem::path dir = freshFolder(oneRound ? "slow-one" : "slow-many");
    b.files.setAutoReceive(dir.string());
    // The whole first pass is lost; the repairs come.
    std::set<uint16_t> lost;
    bool sealed = false;
    b.lose = [&lost, &sealed](protocol::Record& r) {
        if (r.as<protocol::Seal>() != nullptr) sealed = true;
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        if (p == nullptr || sealed) return false;
        lost.insert(p->index);
        return true;
    };
    air.run(2000);
    std::string error;
    std::string path = makeFile("slow.bin", 1200, 21);
    CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
    air.run(40 * 60 * 1000, 1000);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Saved);
    if (f.state == FileState::Saved) CHECK(readAll(f.path) == readAll(path));
    if (oneRound)
        CHECK_EQ(a.seals.rounds(), (size_t)1);
    else
        CHECK(a.seals.rounds() >= 3);
    // Its repairs, parity pieces, went in the slow gear: at least as many
    // as were lost.
    size_t slow = 0, fast = 0;
    for (const Aired& on : air.aired)
    {
        const protocol::FilePiece* p = on.record.as<protocol::FilePiece>();
        if (p == nullptr || p->index < lost.size()) continue;
        (on.mode == "fsk16r25-r1/3" ? slow : fast)++;
    }
    CHECK(slow >= lost.size());
    CHECK_EQ(fast, (size_t)0);
}

// A file the sender cancelled and closed: a request for the rest is
// refused once, the listener's file is Incomplete with the reason, and
// it can't be asked for again.
void testRefusedCancelled()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("refused");
    b.files.setAutoReceive(dir.string());
    b.lose = [](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && p->index == 1;
    };
    bool dropRequests = true;
    a.lose = [&dropRequests](protocol::Record& r) { return dropRequests && r.as<protocol::RepairRequest>() != nullptr; };
    air.run(2000);
    std::string error;
    uint64_t id = a.files.send(makeFile("refused.bin", 2500, 31), "qpsk-r1/3", now, error);
    BroadcastFile mine;
    for (int i = 0; i < 2000 && a.files.find(id, now, mine) && mine.held < 4; i++) air.run(250);
    CHECK(a.files.cancel(id));
    for (int i = 0; i < 2000 && b.seals.requestsSent() == 0; i++) air.run(250);
    CHECK(b.seals.requestsSent() >= 1);
    CHECK(a.files.closeRepairs(id));
    dropRequests = false;

    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && !f.refusal);
    CHECK(b.files.receive(f.id, "", now, error));
    air.run(120000);
    CHECK_EQ(a.seals.refusalsSent(), (size_t)1);
    CHECK_EQ(b.seals.refusalsHeard(), (size_t)1);
    f = fileFrom(b, "AG7EW");
    CHECK(f.state == FileState::Incomplete);
    CHECK(f.refusal && *f.refusal == (uint8_t)protocol::RefusalReason::Cancelled);
    CHECK_EQ(std::string(broadcast::refusalText(*f.refusal)), std::string("cancelled it"));
    bool row = false;
    for (const files::FileRow& r : b.files.rows(now))
    {
        if (r.id == f.id) row = r.state.find("Not repaired: AG7EW cancelled it") == 0;
    }
    CHECK(row);
    // Asking again asks nothing: it would only be refused again.
    CHECK(!b.seals.askAgain("AG7EW", f.seq, now));
    size_t asked = air.requestsOnAir;
    CHECK(b.files.receive(f.id, "", now, error));
    air.run(120000);
    CHECK_EQ(air.requestsOnAir, asked);
    CHECK_EQ(a.seals.refusalsSent(), (size_t)1);
    CHECK(fileFrom(b, "AG7EW").state == FileState::Incomplete);
}

// After a restart the sender holds none of its old seals: a request
// under one is refused (no longer held), and the listener stops asking.
void testRefusedAfterRestart()
{
    Air air;
    air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    b.lose = loseOnce("two");
    bool dropRequests = true;
    air.stations[0]->lose = [&dropRequests](protocol::Record& r) {
        return dropRequests && r.as<protocol::RepairRequest>() != nullptr;
    };
    air.run(2000);
    air.stations[0]->chat.send("one", "", now);
    air.stations[0]->chat.send("two", "", now);
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)1);
    CHECK(b.seals.requestsSent() >= 1);
    Seals::Chain chain = air.stations[0]->seals.chain();
    CHECK(chain.number != 0);

    Station& a = air.restart(0, freshFolder("restart-refused").string(), 5);
    a.seals.setChain("AG7EW", chain);
    dropRequests = false;
    air.run(2000);
    uint32_t two = 0;
    for (const protocol::Record& r : b.heard)
    {
        if (const protocol::Seal* s = r.as<protocol::Seal>()) two = s->firstItem + 1;
    }
    CHECK(b.seals.askAgain("AG7EW", two, now));
    air.run(60000);
    CHECK_EQ(a.seals.refusalsSent(), (size_t)1);
    CHECK(b.seals.refusal("AG7EW", two) == std::optional<uint8_t>((uint8_t)protocol::RefusalReason::NotHeld));
    CHECK(!b.seals.askAgain("AG7EW", two, now));
}

// The S/N a request carries is the mean of the sender's last few bursts,
// not the lowest of 15 minutes: one deep fade no longer puts every repair
// in the slowest gear (Jeff's AG7EW-KC2G test, 2026-10-25).
void testRequestSnrIsMean()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    a.seals.setSealText(false);
    air.run(2000);
    b.hearsAt["AG7EW"] = -14.0;
    a.chat.send("in a fade", "", now);
    air.run(5000);
    CHECK_EQ(b.seals.snrOf("AG7EW", now).value_or(99), -14.0);
    b.hearsAt["AG7EW"] = 6.0;
    for (int i = 0; i < 4; i++)
    {
        a.chat.send("out of it " + std::to_string(i), "", now);
        air.run(5000);
    }
    // (-14 + 4 x 6) / 5.
    CHECK_EQ(b.seals.snrOf("AG7EW", now).value_or(99), 2.0);
    a.chat.send("and one more", "", now);
    air.run(5000);
    CHECK_EQ(b.seals.snrOf("AG7EW", now).value_or(99), 6.0);
}

// The repair pieces of a file, by the mode they went in.
std::map<std::string, size_t> parityModes(const Air& air, uint16_t pieces)
{
    std::map<std::string, size_t> modes;
    for (const Aired& on : air.aired)
    {
        const protocol::FilePiece* p = on.record.as<protocol::FilePiece>();
        if (p != nullptr && p->index >= pieces) modes[on.mode]++;
    }
    return modes;
}

// A weak station whose first pass was all lost, and the sender's file.
struct WeakFile
{
    Air air;
    Station* a = nullptr;
    Station* b = nullptr;
    uint64_t id = 0;
    std::string path;
    bool sealed = false;
    bool loseAll = false;

    explicit WeakFile(const std::string& name)
    {
        a = &air.add("AG7EW", 1);
        b = &air.add("KC2G", 2);
        a->seals.setLadder(ladder());
        b->hearsAt["AG7EW"] = -5.0;  // gear 1, fsk16r25-r1/3, by Auto
        b->files.setAutoReceive(freshFolder(name).string());
        b->lose = [this](protocol::Record& r) {
            if (r.as<protocol::Seal>() != nullptr) sealed = true;
            return r.as<protocol::FilePiece>() != nullptr && (!sealed || loseAll);
        };
        air.run(2000);
        std::string error;
        path = makeFile(name + ".bin", 1200, 41);
        id = a->files.send(path, "qpsk-r1/3", now, error);
        CHECK(id != 0);
    }

    BroadcastFile ours() const
    {
        BroadcastFile f;
        a->files.find(id, now, f);
        return f;
    }
};

// A round the slow cap cuts short sends fewer pieces than were asked for;
// the listener doesn't count those never sent as lost, so it doesn't ask
// for more and more (in Jeff's test 21, then 41).
void testCappedRoundNotLoss()
{
    WeakFile w("capped-round");
    w.a->seals.setSlowResendSeconds(100);   // about one piece a round in gear 1
    w.air.run(60 * 60 * 1000, 1000);
    BroadcastFile f = fileFrom(*w.b, "AG7EW");
    CHECK(f.state == FileState::Saved);
    std::vector<uint32_t> wants;
    for (const Aired& on : w.air.aired)
    {
        const protocol::RepairRequest* r = on.record.as<protocol::RepairRequest>();
        if (r == nullptr || on.record.header.callsign != "KC2G") continue;
        for (const auto& file : r->files) wants.push_back(file.second);
    }
    CHECK(wants.size() >= 3);
    for (size_t i = 1; i < wants.size(); i++) CHECK(wants[i] <= wants[i - 1]);
}

// The floor: repairs go no slower than it, though the asker hears us
// worse.
void testRepairFloor()
{
    WeakFile w("floor");
    w.a->seals.setRepairFloor("qpsk-r1/3");
    w.air.run(30 * 60 * 1000, 1000);
    CHECK(fileFrom(*w.b, "AG7EW").state == FileState::Saved);
    std::map<std::string, size_t> modes = parityModes(w.air, w.ours().pieces);
    CHECK(modes["qpsk-r1/3"] > 0);
    CHECK_EQ(modes["fsk16r25-r1/3"], (size_t)0);
}

// The operator's gear for a file wins over the askers' S/N, faster than
// the first pass and all.
void testRepairGearOverride()
{
    WeakFile w("override");
    CHECK(w.a->files.setRepairGear(w.id, "w48-16qam-r1/2"));
    CHECK_EQ(w.ours().repairGear, std::string("w48-16qam-r1/2"));
    w.air.run(30 * 60 * 1000, 1000);
    CHECK(fileFrom(*w.b, "AG7EW").state == FileState::Saved);
    std::map<std::string, size_t> modes = parityModes(w.air, w.ours().pieces);
    CHECK(modes["w48-16qam-r1/2"] > 0);
    CHECK_EQ(modes.size(), (size_t)1);
    // Back to Auto.
    CHECK(w.a->files.setRepairGear(w.id, ""));
    CHECK_EQ(w.ours().repairGear, std::string());
}

// The cap on a file's repair airtime: once its repairs have had it, they
// close, the asker hears so, and Re-open repairs gives as much again.
void testRepairCapAndReopen()
{
    WeakFile w("cap");
    w.loseAll = true;   // nothing ever comes: it asks until refused
    w.a->seals.setRepairCapSeconds(200);
    w.air.run(60 * 60 * 1000, 1000);
    BroadcastFile f = w.ours();
    CHECK(f.state == FileState::Sent);
    CHECK(f.repairCapped);
    CHECK(f.repairSeconds >= 200.0 && f.repairSeconds < 200.0 + 2 * 90.0);
    CHECK(f.reopenable);
    BroadcastFile theirs = fileFrom(*w.b, "AG7EW");
    CHECK(theirs.state == FileState::Incomplete);
    CHECK(theirs.refusal && *theirs.refusal == (uint8_t)protocol::RefusalReason::Closed);

    // Re-opened, and now the pieces come: the seal goes again, the
    // listener asks again, and the file is saved.
    w.loseAll = false;
    std::string error;
    CHECK(w.a->files.reopen(w.id, now + 30 * 60 * 1000, now, error));
    CHECK(w.ours().state == FileState::Repairing);
    CHECK_EQ(w.ours().repairSeconds, 0.0);
    // Room enough this time.
    w.a->seals.setRepairCapSeconds(30 * 60);
    // The listener asks again by itself, as it was receiving it.
    w.air.run(30 * 60 * 1000, 1000);
    CHECK(fileFrom(*w.b, "AG7EW").state == FileState::Saved);
    // Its seal gone (pruned), it can't re-open: broadcast it again.
    w.a->files.closeRepairs(w.id);
    CHECK(w.ours().state == FileState::Sent);
    w.air.run(3 * 60 * 60 * 1000, 10000);
    CHECK(!w.ours().reopenable);
    CHECK(!w.a->files.reopen(w.id, now + 60000, now, error));
    CHECK(!error.empty());
    CHECK_EQ(w.a->files.findSent(w.path, 1200), w.id);
}

// Close repairs now takes back the round's queued repair bursts: only the
// one already with the host still goes.
void testCloseTakesBack()
{
    WeakFile w("take-back");
    w.loseAll = true;
    std::map<std::string, size_t> before;
    uint16_t pieces = w.ours().pieces;
    for (int i = 0; i < 3600 && before.empty(); i++)
    {
        w.air.run(1000, 1000);
        before = parityModes(w.air, pieces);
    }
    CHECK(!before.empty());
    size_t queued = 0;
    for (const auto& o : w.a->link.outgoing()) queued += o.label == "repairs" ? 1 : 0;
    CHECK(queued >= 2);
    CHECK(w.a->files.closeRepairs(w.id));
    w.air.run(20 * 60 * 1000, 1000);
    size_t sentBefore = 0, sentAfter = 0;
    for (const auto& m : before) sentBefore += m.second;
    for (const auto& m : parityModes(w.air, pieces)) sentAfter += m.second;
    // The burst with the host carries one piece in gear 1.
    CHECK(sentAfter <= sentBefore + 1);
}

// Jeff and KC2G, 2026-10-26: a file stopped part way, a round of repairs
// for KC2G, repairs closed and KC2G refused, then re-opened and whole. Both
// screens show it as it goes: the sender who asked and the round going
// (to whom, pieces sent of those in it), the listener the repair pieces it
// holds counted toward the whole; and each hears of the re-open.
void testRepairsShown()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    b.files.setAutoReceive(freshFolder("shown").string());
    bool loseRepairs = false;
    uint16_t total = 0;
    b.lose = [&](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && loseRepairs && total != 0 && p->index >= total;
    };
    air.run(2000);
    std::string error;
    uint64_t id = a.files.send(makeFile("shown.bin", 6000, 51), "qpsk-r1/3", now, error);
    CHECK(id != 0);
    BroadcastFile mine;
    for (int i = 0; i < 2000 && a.files.find(id, now, mine) && mine.held < 4; i++) air.run(250);
    total = mine.pieces;
    CHECK(mine.held >= 4 && mine.held + 4 < mine.pieces);
    CHECK(a.files.cancel(id));

    // The round, as it goes.
    bool roundShown = false, rowShown = false, coveredShown = false;
    uint32_t mostSent = 0;
    for (int i = 0; i < 4000; i++)
    {
        air.run(250);
        a.files.find(id, now, mine);
        if (mine.roundPieces > 0)
        {
            roundShown = roundShown || (mine.repairingFor == std::vector<std::string>{"KC2G"});
            CHECK(mine.roundSent <= mine.roundPieces);
            mostSent = std::max(mostSent, mine.roundSent);
            for (const files::FileRow& r : a.files.rows(now))
            {
                if (r.id == id && r.state.find("Stopped at ") == 0
                    && r.state.find("sending repairs to KC2G, ") != std::string::npos)
                    rowShown = true;
            }
        }
        BroadcastFile theirs = fileFrom(b, "AG7EW");
        if (theirs.state == FileState::Receiving && theirs.repairHeld > 0)
        {
            CHECK(theirs.held + theirs.repairHeld < theirs.pieces);
            CHECK(std::count(theirs.pieceMap.begin(), theirs.pieceMap.end(), files::PIECE_COVERED)
                  == theirs.repairHeld);
            for (const files::FileRow& r : b.files.rows(now))
            {
                // Its pieces' bytes, and as many again for each repair piece.
                uint64_t pb = (uint64_t)broadcast::pieceBytesFor(broadcast::findMode({}, "qpsk-r1/3"), "AG7EW");
                if (r.id == theirs.id) coveredShown = coveredShown || r.done > (uint64_t)theirs.held * pb;
            }
        }
        // The round goes on to its end, though the file is whole.
        if (theirs.state == FileState::Saved && mine.roundPieces == 0) break;
    }
    CHECK(roundShown);
    CHECK(rowShown);
    CHECK(mostSent > 0);
    CHECK(coveredShown);
    CHECK(fileFrom(b, "AG7EW").state == FileState::Saved);
    // Between rounds: who asked, and what has gone.
    a.files.find(id, now, mine);
    CHECK(mine.state == FileState::Repairing);
    CHECK_EQ(mine.roundPieces, (uint32_t)0);
    CHECK(mine.askedBy == std::vector<std::string>{"KC2G"});
    CHECK(mine.repairPieces >= mostSent);
    std::string words = broadcast::repairText(mine);
    CHECK(words.find("KC2G asked, ") == 0 && words.find(" repair pieces sent") != std::string::npos);
    bool repairingEvent = false;
    for (const auto& e : a.files.takeEvents())
        repairingEvent = repairingEvent || (e.type == BroadcastFiles::Event::Type::Repairing && e.id == id);
    CHECK(repairingEvent);
    CHECK(a.files.closeRepairs(id));
    CHECK(a.files.find(id, now, mine) && mine.state == FileState::Cancelled && mine.reopenable);

    // Again, to a station that hears no repairs: closed, it is refused;
    // re-opened, it asks again by itself and the file comes whole.
    Station& c = air.add("W1AW", 3);
    c.files.setAutoReceive(freshFolder("shown-c").string());
    c.lose = [&](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && (p->index >= total || p->index % 2 == 0) && loseRepairs;
    };
    loseRepairs = true;
    uint64_t second = a.files.send(makeFile("shown2.bin", 3000, 52), "qpsk-r1/3", now, error);
    CHECK(second != 0);
    a.files.find(second, now, mine);
    total = mine.pieces;
    for (int i = 0; i < 4000 && fileFrom(c, "AG7EW").id == 0; i++) air.run(250);
    for (int i = 0; i < 4000 && a.files.find(second, now, mine) && mine.repairPieces == 0; i++) air.run(250);
    CHECK(mine.repairPieces > 0);
    CHECK(a.files.closeRepairs(second));
    for (int i = 0; i < 4000 && !fileFrom(c, "AG7EW").refusal; i++) air.run(250);
    air.run(1000);
    BroadcastFile theirs = fileFrom(c, "AG7EW");
    CHECK(theirs.state == FileState::Incomplete && theirs.refusal);
    c.files.takeEvents();
    a.files.takeEvents();
    loseRepairs = false;
    CHECK(a.files.reopen(second, now + 30 * 60 * 1000, now, error));
    a.files.find(second, now, mine);
    CHECK(mine.state == FileState::Repairing);
    CHECK_EQ(mine.repairPieces, (uint32_t)0);
    CHECK(mine.askedBy == std::vector<std::string>{"W1AW"});
    bool reopenedHere = false;
    for (const auto& e : a.files.takeEvents())
        reopenedHere = reopenedHere || (e.type == BroadcastFiles::Event::Type::Reopened && e.id == second);
    CHECK(reopenedHere);
    for (int i = 0; i < 8000 && fileFrom(c, "AG7EW").state != FileState::Saved; i++) air.run(250);
    theirs = fileFrom(c, "AG7EW");
    CHECK(theirs.state == FileState::Saved);
    bool reopenedThere = false;
    for (const auto& e : c.files.takeEvents())
        reopenedThere = reopenedThere || (e.type == BroadcastFiles::Event::Type::Reopened && e.id == theirs.id);
    CHECK(reopenedThere);
    a.files.find(second, now, mine);
    CHECK(mine.repairPieces > 0);
}

// An inline picture: its announce says so, a listener that shows
// pictures as they come takes one under its cap without asking, and any
// first part of it heard is there to show while the rest comes. One over
// the cap, or a listener that doesn't, waits for Receive like any file.
void testPictures()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    Station& c = air.add("W1AW", 3);
    std::filesystem::path bDir = freshFolder("pictures-b");
    b.files.setAutoPictures(bDir.string(), 64 * 1024);
    air.run(2000);

    std::string path = makeFile("shack.j2k", 2500, 7);
    std::vector<uint8_t> want = readAll(path);
    std::string error;
    uint64_t id = a.files.send(path, "qpsk-r1/3", now, error, true);
    CHECK(id != 0);
    std::vector<uint8_t> ours;
    CHECK(a.files.pictureBytes(id, ours) && ours == want);

    // As it comes, what there is to show is always the start of it.
    bool partShown = false;
    for (int i = 0; i < 120; i++)
    {
        air.run(1000);
        BroadcastFile f = fileFrom(b, "AG7EW");
        if (f.id == 0) continue;
        CHECK(f.picture);
        std::vector<uint8_t> part;
        if (!b.files.pictureBytes(f.id, part)) continue;
        CHECK(part.size() <= want.size());
        CHECK(std::equal(part.begin(), part.end(), want.begin()));
        partShown = partShown || (part.size() > 0 && part.size() < want.size());
        if (f.state == FileState::Saved) break;
    }
    CHECK(partShown);
    BroadcastFile fb = fileFrom(b, "AG7EW");
    CHECK(fb.state == FileState::Saved && fb.picture);
    CHECK(fb.path.find("pictures-b") != std::string::npos);
    if (fb.id != 0)
    {
        CHECK(readAll(fb.path) == want);
        std::vector<uint8_t> saved;
        CHECK(b.files.pictureBytes(fb.id, saved) && saved == want);
    }
    // C shows no pictures by itself: heard, waiting for Receive.
    BroadcastFile fc = fileFrom(c, "AG7EW");
    CHECK(fc.picture && fc.state == FileState::Heard);

    // Over the cap: heard, not taken.
    Station& d = air.add("VK2ABC", 4);
    std::filesystem::path dDir = freshFolder("pictures-d");
    d.files.setAutoPictures(dDir.string(), 1000);
    std::string big = makeFile("big.j2k", 1500, 8);
    CHECK(a.files.send(big, "qpsk-r1/3", now, error, true) != 0);
    air.run(60000);
    bool heard = false;
    for (const BroadcastFile& f : d.files.files(now))
    {
        if (f.outgoing || f.name != "big.j2k") continue;
        heard = true;
        CHECK(f.picture && f.state == FileState::Heard);
    }
    CHECK(heard);

    // A file that isn't a picture is never taken as one.
    std::string plain = makeFile("notes.txt", 500, 9);
    CHECK(a.files.send(plain, "qpsk-r1/3", now, error) != 0);
    air.run(60000);
    for (const BroadcastFile& f : b.files.files(now))
    {
        if (f.name == "notes.txt") CHECK(!f.picture && f.state == FileState::Heard);
    }
}

// A voice message: its announce says kind 0x03, a listener that takes
// voice as it comes takes it under its cap into the voice folder, and
// while pieces are missing voiceBytes gives the whole file's size with the
// holes named, the rest as sent.
void testVoice()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path bDir = freshFolder("voice-b");
    b.files.setAutoVoice(bDir.string(), 64 * 1024);
    // Piece 2 is lost the first time it goes.
    auto lost = std::make_shared<bool>(false);
    b.lose = [lost](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        if (p == nullptr || p->index != 2 || *lost) return false;
        *lost = true;
        return true;
    };
    air.run(2000);

    std::string path = makeFile("hello.sot", 3000, 11);
    std::vector<uint8_t> want = readAll(path);
    std::string error;
    uint64_t id = a.files.send(path, "qpsk-r1/3", now, error, false, true);
    CHECK(id != 0);
    BroadcastFile mine;
    CHECK(a.files.find(id, now, mine) && mine.voice && !mine.picture);
    std::vector<uint8_t> ours;
    std::vector<BroadcastFiles::Hole> holes;
    CHECK(a.files.voiceBytes(id, ours, holes) && ours == want && holes.empty());

    bool sawHole = false;
    for (int i = 0; i < 180; i++)
    {
        air.run(1000);
        BroadcastFile f = fileFrom(b, "AG7EW");
        if (f.id == 0) continue;
        CHECK(f.voice && !f.picture);
        std::vector<uint8_t> part;
        if (!b.files.voiceBytes(f.id, part, holes)) continue;
        CHECK_EQ(part.size(), want.size());
        // What is held is as sent; the holes are whole pieces.
        std::vector<bool> missing(part.size(), false);
        for (const BroadcastFiles::Hole& h : holes)
        {
            CHECK(h.begin < h.end && h.end <= part.size());
            for (uint64_t x = h.begin; x < h.end && x < part.size(); x++) missing[(size_t)x] = true;
        }
        bool same = true;
        for (size_t x = 0; x < part.size() && x < want.size(); x++)
            if (!missing[x] && part[x] != want[x]) same = false;
        CHECK(same);
        bool middle = false;
        for (const BroadcastFiles::Hole& h : holes) middle = middle || (h.begin > 0 && h.end < part.size());
        sawHole = sawHole || middle;
        if (f.state == FileState::Saved) break;
    }
    CHECK(sawHole);
    BroadcastFile fb = fileFrom(b, "AG7EW");
    CHECK(fb.state == FileState::Saved && fb.voice);
    CHECK(fb.path.find("voice-b") != std::string::npos);
    if (fb.id != 0) CHECK(readAll(fb.path) == want);
}

int main()
{
    testAirtime();
    testSealAfterQuiet();
    testRepairText();
    testSuppression();
    testLateJoiner();
    testMissedSeal();
    testFiles();
    testOpenWindow();
    testDigestMismatch();
    testLostAnnounce();
    testLostSealOrRequest();
    testAllSealsLost();
    testAllSealsLostRefused();
    testStoppedShare();
    testStopAndAskAgain();
    testGiveUpAndAskAgain();
    testSealMarks();
    testManyLost();
    testRefused();
    testIgnore();
    testChatUnsealed();
    testPartsAcrossRestart();
    testPartsExpire();
    testRepairGear();
    testSuppressionByGear(2, 3, "KC2G");
    testSuppressionByGear(3, 2, "W1AW");
    testSlowCap(60 * 60, true);
    testSlowCap(20, false);
    testRefusedCancelled();
    testRefusedAfterRestart();
    testHeavyLossFile();
    testHeavyLossChat();
    testItemParityBeforeSeal();
    testRequestSnrIsMean();
    testCappedRoundNotLoss();
    testRepairFloor();
    testRepairGearOverride();
    testRepairCapAndReopen();
    testCloseTakesBack();
    testRepairsShown();
    testPictures();
    testVoice();
    std::filesystem::remove_all(scratch());
    return ragtime_test::finish();
}
