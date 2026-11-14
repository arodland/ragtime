// HostLink against a host played by hand: the opening, the group, bursts
// with their ACKMODE tags and outcomes, a session holding the group, and
// sessions called, answered, left and lost. The clock is simulated.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/link/HostLink.h"

using namespace ragtime::link;
using Type = HostLink::Event::Type;
using BurstState = HostLink::BurstState;

namespace
{

// The command lines written, without their CRs.
std::vector<std::string> linesOf(const std::string& output)
{
    std::vector<std::string> lines;
    std::string line;
    for (char c : output)
    {
        if (c == '\r')
        {
            lines.push_back(line);
            line.clear();
        }
        else
        {
            line.push_back(c);
        }
    }
    return lines;
}

void say(HostLink& link, const std::string& lines, uint64_t nowMs)
{
    link.commandBytes(lines.data(), lines.size(), nowMs);
}

bool hasEvent(const std::vector<HostLink::Event>& events, Type type)
{
    for (const auto& e : events)
    {
        if (e.type == type) return true;
    }
    return false;
}

// The tags of the ACKMODE frames written to the KISS port.
std::vector<uint16_t> tagsWritten(HostLink& link, int port, std::vector<std::vector<uint8_t>>* frames = nullptr)
{
    std::vector<uint8_t> out = link.takeKissOutput();
    KissDecoder decoder;
    std::vector<KissFrame> kiss;
    decoder.feed(out.data(), out.size(), kiss);
    std::vector<uint16_t> tags;
    for (const KissFrame& f : kiss)
    {
        if (f.command == KISS_SETHARDWARE) continue;   // REFS ON, as the ports come up
        CHECK(f.command == KISS_ACKMODE && f.port == port && f.payload.size() >= 2);
        if (f.payload.size() < 2) continue;
        tags.push_back((uint16_t)((f.payload[0] << 8) | f.payload[1]));
        if (frames != nullptr) frames->emplace_back(f.payload.begin() + 2, f.payload.end());
    }
    return tags;
}

void ackTags(HostLink& link, int port, const std::vector<uint16_t>& tags, uint64_t nowMs)
{
    std::vector<uint8_t> in;
    for (uint16_t tag : tags)
    {
        std::vector<uint8_t> ack = kissEncodeAckMode(port, tag, {});
        in.insert(in.end(), ack.begin(), ack.end());
    }
    link.kissBytes(in.data(), in.size(), nowMs);
}

// A link through its opening, with the group open on port 2 and the
// known modes listed.
HostLink opened(uint64_t nowMs, bool narrow = false)
{
    HostLink link({"ag7ew", true, narrow});
    link.commandUp(nowMs);
    link.takeCommandOutput();
    std::string answers = "OK\rOK\rWRONG\rOK\rOK\rBCAST PORT 2\r";
    for (const ModeInfo& m : knownModes())
    {
        answers += "MODE " + m.name + " " + std::to_string(m.bandwidthHz) + " " + std::to_string(m.bytesPerCodeword) +
                   " " + std::to_string(m.maxCodewords) + " 1.0 10.0\r";
    }
    answers += "OK\r";
    say(link, answers, nowMs);
    link.takeEvents();
    // KISS P and REFS ON went as the ports came up.
    std::vector<uint8_t> up = {0xC0, KISS_PERSIST, HostLink::KISS_P, 0xC0};
    std::vector<uint8_t> refs = kissEncodeRefs(true);
    up.insert(up.end(), refs.begin(), refs.end());
    CHECK(link.takeKissOutput() == up);
    return link;
}

void testTheOpening()
{
    HostLink link({"ag7ew", true, false});
    CHECK(!link.commandConnected());
    link.commandUp(1000);
    std::vector<std::string> lines = linesOf(link.takeCommandOutput());
    const std::vector<std::string> expected = {"MYCALL AG7EW", "CHAT ON", "DATA2G ON", "LISTEN ON", "BW2300",
                                               "BCAST OPEN D2G FROM AG7EW", "MODES"};
    CHECK(lines == expected);
    CHECK(!link.groupOpen());

    say(link, "OK\rOK\rWRONG\rOK\rOK\rBCAST PORT 2\rMODE fsk16r25-r1/3 500 38 9 4.98 89.22\r", 1100);
    CHECK(link.groupOpen());
    CHECK_EQ(link.groupPort(), 2);
    // The list counts once its OK comes.
    CHECK(link.modes().empty());
    say(link, "MODE qpsk-r1/3 1200 76 64 1.78 74.36\rOK\r", 1200);
    CHECK_EQ(link.modes().size(), (size_t)2);
    std::vector<HostLink::Event> events = link.takeEvents();
    CHECK(hasEvent(events, Type::GroupOpen));
    CHECK(hasEvent(events, Type::ModesChanged));
    CHECK(link.modes()[1].thresholds.empty());

    // Thresholds, from hosts since Data2G PR #186.
    HostLink given({"ag7ew", true, false});
    given.commandUp(0);
    given.takeCommandOutput();
    say(given, "OK\rOK\rWRONG\rOK\rOK\rBCAST PORT 2\rMODE qpsk-r1/3 1200 76 64 1.78 74.36\r"
               "CHANNELS awgn mpg\rTHRESHOLD qpsk-r1/3 -4.5 -\rTHRESHOLD qpsk-r1/3 1\rOK\r", 100);
    CHECK_EQ(given.modes().size(), (size_t)1);
    CHECK(given.modes()[0].thresholds.size() == 2 && given.modes()[0].thresholds.at("awgn") == -4.5 &&
          !given.modes()[0].thresholds.at("mpg"));

    // Not taking sessions, and the 500 Hz button.
    HostLink narrow({"W1AW", false, true});
    narrow.commandUp(0);
    lines = linesOf(narrow.takeCommandOutput());
    CHECK(lines.size() == 7 && lines[3] == "LISTEN OFF" && lines[4] == "BW500");

    // Changed later: the host is told, and the modes asked for again.
    link.setNarrow(true, 2000);
    link.setAcceptSessions(false, 2000);
    lines = linesOf(link.takeCommandOutput());
    CHECK(lines == std::vector<std::string>({"BW500", "MODES", "LISTEN OFF"}));
    link.setNarrow(true, 2000);
    CHECK(link.takeCommandOutput().empty());
}

void testReports()
{
    HostLink link = opened(0);
    say(link, "PTT ON\rBUSY ON\rMODE qpsk-r1/3\rBCAST 2 HEARD W1AW\rBCAST 2 LOST 3\rIAMALIVE\r", 10);
    CHECK(link.ptt() && link.busy());
    std::vector<HostLink::Event> events = link.takeEvents();
    CHECK(hasEvent(events, Type::Ptt) && hasEvent(events, Type::Busy) && hasEvent(events, Type::ModeUsed) &&
          hasEvent(events, Type::HeardFrom) && hasEvent(events, Type::GroupLost));
    // The same again is no news.
    say(link, "PTT ON\r", 20);
    CHECK(!hasEvent(link.takeEvents(), Type::Ptt));

    // The host drops while keyed: PTT is off.
    {
        HostLink keyed = opened(0);
        say(keyed, "PTT ON\r", 10);
        keyed.takeEvents();
        keyed.commandDown(20);
        bool off = false;
        for (const auto& e : keyed.takeEvents()) off = off || (e.type == Type::Ptt && !e.on);
        CHECK(off && !keyed.ptt());
        keyed.commandUp(30);
        keyed.commandDown(40);
        CHECK(!hasEvent(keyed.takeEvents(), Type::Ptt));
    }

    // Frames heard on the group port come up; other ports' don't.
    std::vector<uint8_t> in = kissEncode(2, {0xA0, 1, 2});
    std::vector<uint8_t> other = kissEncode(5, {0xA0, 9});
    in.insert(in.end(), other.begin(), other.end());
    link.kissBytes(in.data(), in.size(), 30);
    events = link.takeEvents();
    int heard = 0;
    for (const auto& e : events)
    {
        if (e.type == Type::Heard)
        {
            heard++;
            CHECK(e.frame == std::vector<uint8_t>({0xA0, 1, 2}));
        }
    }
    CHECK_EQ(heard, 1);
}

void testSnrReports()
{
    HostLink link = opened(0);
    // A host since Data2G PR #98 ends HEARD and MISSED with an SNR.
    say(link, "BCAST 2 HEARD W1AW 12.3\rBCAST * MISSED qpsk-r1/3 4 -2.5\rBCAST 2 HEARD K1ABC\r", 10);
    std::vector<HostLink::Event> events = link.takeEvents();
    CHECK_EQ(events.size(), (size_t)3);
    if (events.size() == 3)
    {
        CHECK(events[0].type == Type::HeardFrom && events[0].text == "W1AW" && events[0].snr == 12.3);
        CHECK(events[1].type == Type::Missed && events[1].text == "qpsk-r1/3" && events[1].count == 4 &&
              events[1].snr == -2.5);
        CHECK(events[2].type == Type::HeardFrom && events[2].text == "K1ABC" && !events[2].snr);
    }

    // In a session, SN once the partner's burst has likely come.
    CHECK(link.call("W1AW", 100));
    say(link, "OK\rCONNECTED AG7EW W1AW 2300\r", 200);
    link.takeCommandOutput();
    link.takeEvents();
    say(link, "BUFFER 300\r", 300);    // our own bytes going in: no
    CHECK(link.takeCommandOutput().empty());
    say(link, "BUFFER 100\r", 400);    // acknowledged: the partner sent
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"SN"}));
    // Not before the partner's first burst, nor from an older host: no refusal.
    say(link, "WRONG\r", 410);
    CHECK(!hasEvent(link.takeEvents(), Type::Refused));

    // At most every SN_GAP_MS: asked again once it has passed.
    link.sessionHeard(500);
    CHECK(link.takeCommandOutput().empty());
    link.tick(400 + HostLink::SN_GAP_MS);
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"SN"}));
    say(link, "SN 6.5\r", 5500);
    events = link.takeEvents();
    CHECK(events.size() == 1 && events[0].type == Type::SessionSnr && events[0].peer == "W1AW" &&
          events[0].snr == 6.5);
    // The same figure again is the same burst.
    link.sessionHeard(20000);
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"SN"}));
    say(link, "SN 6.5\r", 20010);
    CHECK(!hasEvent(link.takeEvents(), Type::SessionSnr));
    // Nothing to ask once the session is over.
    say(link, "DISCONNECTED\r", 21000);
    link.sessionHeard(40000);
    link.tick(40000);
    CHECK(link.takeCommandOutput().empty());
}

void testABurstGoesInItsMode()
{
    HostLink link = opened(0);
    uint64_t id = link.broadcast({{0xA0, 1}, {0xA0, 2}}, "fsk16r25-r1/3", 100);
    CHECK(id != 0);
    // The mode first, and nothing on the KISS port until the host says OK.
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"BCAST MODE 2 fsk16r25-r1/3"}));
    CHECK(link.takeKissOutput().empty());
    CHECK(link.burstState(id) == BurstState::Queued);
    say(link, "OK\r", 200);
    CHECK_EQ(link.groupMode(), std::string("fsk16r25-r1/3"));
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint16_t> tags = tagsWritten(link, 2, &frames);
    CHECK_EQ(tags.size(), (size_t)2);
    CHECK(frames == std::vector<std::vector<uint8_t>>({{0xA0, 1}, {0xA0, 2}}));
    CHECK(link.burstState(id) == BurstState::Sending);
    // The host has it: too late to cancel.
    CHECK(!link.cancel(id));

    // Sent once every tag is back.
    ackTags(link, 2, {tags[0]}, 300);
    CHECK(link.burstState(id) == BurstState::Sending);
    ackTags(link, 2, {tags[1]}, 400);
    CHECK(link.burstState(id) == BurstState::Sent);

    // The next in the same mode goes straight out.
    uint64_t next = link.broadcast({{0xA0, 3}}, "fsk16r25-r1/3", 500);
    CHECK(link.takeCommandOutput().empty());
    CHECK_EQ(tagsWritten(link, 2).size(), (size_t)1);
    CHECK(link.burstState(next) == BurstState::Sending);

    // One at a time: a third waits for the second, and can be cancelled.
    uint64_t third = link.broadcast({{0xA0, 4}}, "", 600);
    CHECK(link.takeKissOutput().empty());
    CHECK(link.burstState(third) == BurstState::Queued);
    CHECK(link.cancel(third));
    CHECK(link.burstState(third) == BurstState::NotSent);
}

void testAModeTheHostRefuses()
{
    HostLink link = opened(0);
    uint64_t id = link.broadcast({{0xA0, 1}}, "no-such-mode", 100);
    link.takeCommandOutput();
    say(link, "WRONG\r", 200);
    CHECK(link.burstState(id) == BurstState::NotSent);
    CHECK(link.takeKissOutput().empty());
    CHECK(hasEvent(link.takeEvents(), Type::Refused));
}

void testTheModeWaitsForEarlierAnswers()
{
    // A burst queued before the opening is answered asks for its mode only
    // once every earlier command has its answer, so the OK is the mode's.
    HostLink link({"AG7EW", true, false});
    link.commandUp(0);
    link.takeCommandOutput();
    say(link, "OK\rOK\rWRONG\rOK\rOK\rBCAST PORT 2\r", 10);
    uint64_t id = link.broadcast({{0xA0, 1}}, "qpsk-r1/3", 20);
    CHECK(link.takeCommandOutput().empty());
    say(link, "MODE qpsk-r1/3 1200 76 64 1.78 74.36\rOK\r", 30);
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"BCAST MODE 2 qpsk-r1/3"}));
    say(link, "OK\r", 40);
    CHECK(link.burstState(id) == BurstState::Sending);
}

void testNotSentAfterTwoMinutes()
{
    HostLink link = opened(0);
    uint64_t id = link.broadcast({{0xA0, 1}}, "", 0);
    tagsWritten(link, 2);
    link.tick(HostLink::NOT_SENT_MS - 1000);
    CHECK(link.burstState(id) == BurstState::Sending);
    link.tick(HostLink::NOT_SENT_MS);
    CHECK(link.burstState(id) == BurstState::NotSent);
    // The next goes once the last is given up.
    uint64_t next = link.broadcast({{0xA0, 2}}, "", HostLink::NOT_SENT_MS + 10);
    CHECK_EQ(tagsWritten(link, 2).size(), (size_t)1);
    CHECK(link.burstState(next) == BurstState::Sending);
}

void testASessionHoldsTheGroup()
{
    HostLink link = opened(0);
    say(link, "CONNECTED W1AW AG7EW 2300\r", 0);
    CHECK(link.sessionState() == HostLink::SessionState::Connected);
    uint64_t id = link.broadcast({{0xA0, 1}}, "", 1000);
    std::vector<uint16_t> tags = tagsWritten(link, 2);
    // The host has it, but holds it for the session.
    CHECK(link.burstState(id) == BurstState::Held);

    // Not given up however long the session lasts.
    for (uint64_t t = 1000; t <= 20 * 60 * 1000; t += 1000) link.tick(t);
    CHECK(link.burstState(id) == BurstState::Held);

    // The host lets it go in a lull (Data2G PR #59).
    ackTags(link, 2, tags, 20 * 60 * 1000 + 10);
    CHECK(link.burstState(id) == BurstState::Sent);

    // Another held, then the session ends: Sending again, and the two
    // minutes start then.
    uint64_t second = link.broadcast({{0xA0, 2}}, "", 1300000);
    tagsWritten(link, 2);
    CHECK(link.burstState(second) == BurstState::Held);
    say(link, "DISCONNECTED\r", 1400000);
    CHECK(link.burstState(second) == BurstState::Sending);
    link.tick(1400000 + HostLink::NOT_SENT_MS - 1);
    CHECK(link.burstState(second) == BurstState::Sending);
    link.tick(1400000 + HostLink::NOT_SENT_MS);
    CHECK(link.burstState(second) == BurstState::NotSent);
}

void testDroppedAndPortLost()
{
    HostLink link = opened(0);
    uint64_t id = link.broadcast({{0xA0, 1}}, "", 0);
    tagsWritten(link, 2);
    say(link, "BCAST 2 DROPPED 1\r", 10);
    CHECK(link.burstState(id) == BurstState::NotSent);

    uint64_t second = link.broadcast({{0xA0, 2}}, "", 20);
    uint64_t third = link.broadcast({{0xA0, 3}}, "", 20);
    tagsWritten(link, 2);
    say(link, "CONNECTED AG7EW W1AW\r", 30);
    link.takeEvents();
    link.commandDown(40);
    // The one with the host isn't Sent; the one still ours waits.
    CHECK(link.burstState(second) == BurstState::NotSent);
    CHECK(link.burstState(third) == BurstState::Queued);
    CHECK(!link.groupOpen());
    CHECK(link.sessionState() == HostLink::SessionState::None);
    std::vector<HostLink::Event> events = link.takeEvents();
    CHECK(hasEvent(events, Type::GroupClosed));
    bool lost = false;
    for (const auto& e : events) lost = lost || (e.type == Type::SessionEnded && e.sessionEnd == HostLink::SessionEnd::HostLost);
    CHECK(lost);

    // Back again, it goes.
    link.commandUp(50);
    link.takeCommandOutput();
    say(link, "OK\rOK\rWRONG\rOK\rOK\rBCAST PORT 3\rOK\r", 60);
    CHECK_EQ(tagsWritten(link, 3).size(), (size_t)1);
    CHECK(link.burstState(third) == BurstState::Sending);
}

void testPauseSending()
{
    HostLink link = opened(0);
    link.setPaused(true, 0);
    uint64_t id = link.broadcast({{0xA0, 1}}, "", 10);
    CHECK(link.takeKissOutput().empty());
    CHECK(link.burstState(id) == BurstState::Queued);
    link.setPaused(false, 20);
    CHECK_EQ(tagsWritten(link, 2).size(), (size_t)1);
}

void testCallingAndLeaving()
{
    HostLink link = opened(0);
    CHECK(link.call("w1aw", 100));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"CONNECT AG7EW W1AW"}));
    CHECK(link.sessionState() == HostLink::SessionState::Calling);
    CHECK(!link.call("K1ABC", 110)); // one session at a time
    say(link, "OK\rCONNECTED AG7EW W1AW 500\r", 200);
    CHECK(link.sessionState() == HostLink::SessionState::Connected);
    CHECK_EQ(link.sessionPeer(), std::string("W1AW"));
    CHECK(!link.sessionIncoming());
    bool connected = false;
    for (const auto& e : link.takeEvents())
    {
        if (e.type == Type::SessionConnected) connected = e.peer == "W1AW" && !e.incoming && e.number == 500;
    }
    CHECK(connected);

    say(link, "BUFFER 312\r", 300);
    CHECK_EQ(link.buffer(), 312);
    CHECK(hasEvent(link.takeEvents(), Type::Buffer));
    CHECK(link.leave(400));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"DISCONNECT"}));
    CHECK(link.sessionState() == HostLink::SessionState::Leaving);
    // Leave now, while the host waits for the 312 bytes.
    CHECK(link.leaveNow(500));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"ABORT"}));
    say(link, "OK\rOK\rDISCONNECTED\rBUFFER 0\r", 600);
    CHECK(link.sessionState() == HostLink::SessionState::None);
    CHECK_EQ(link.buffer(), 0);
    HostLink::SessionEnd end = HostLink::SessionEnd::Ended;
    bool bufferAfter = false;
    for (const auto& e : link.takeEvents())
    {
        if (e.type == Type::SessionEnded) end = e.sessionEnd;
        if (e.type == Type::Buffer) bufferAfter = true;
    }
    CHECK(end == HostLink::SessionEnd::Left);
    CHECK(!bufferAfter); // BUFFER 0 after DISCONNECTED is the host listening again

    // No answer.
    CHECK(link.call("K1ABC", 700));
    link.takeEvents();
    say(link, "OK\rDISCONNECTED\r", 800);
    for (const auto& e : link.takeEvents())
    {
        if (e.type == Type::SessionEnded) CHECK(e.sessionEnd == HostLink::SessionEnd::NoAnswer && e.peer == "K1ABC");
    }

    // Called by another station, and that station leaves.
    say(link, "CONNECTED VK3ABC AG7EW\r", 900);
    CHECK(link.sessionIncoming());
    CHECK_EQ(link.sessionPeer(), std::string("VK3ABC"));
    link.takeEvents();
    say(link, "DISCONNECTED\r", 1000);
    for (const auto& e : link.takeEvents())
    {
        if (e.type == Type::SessionEnded) CHECK(e.sessionEnd == HostLink::SessionEnd::Ended);
    }

    // Nothing to leave.
    CHECK(!link.leave(1100));
    CHECK(!link.leaveNow(1100));
    // Leave while calling stops the call.
    CHECK(link.call("W1AW", 1200));
    CHECK(link.leave(1300));
    std::vector<std::string> lines = linesOf(link.takeCommandOutput());
    CHECK(!lines.empty() && lines.back() == "ABORT");
}

void testAbortTransmit()
{
    // The SWR trip with no session: ABORT, and the DISCONNECTED the host
    // answers it with ends nothing.
    HostLink link = opened(0);
    link.takeEvents();
    CHECK(link.abortTransmit(100));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"ABORT"}));
    say(link, "OK\rDISCONNECTED\r", 200);
    CHECK(link.sessionState() == HostLink::SessionState::None);
    CHECK(!hasEvent(link.takeEvents(), Type::SessionEnded));
    // In a session: Leave now.
    CHECK(link.call("W1AW", 300));
    say(link, "OK\rCONNECTED AG7EW W1AW 500\r", 400);
    link.takeCommandOutput();
    CHECK(link.abortTransmit(500));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"ABORT"}));
    CHECK(link.sessionState() == HostLink::SessionState::Leaving);
    say(link, "OK\rDISCONNECTED\r", 600);
    CHECK(link.sessionState() == HostLink::SessionState::None);
}

void testCuts()
{
    // A host from before Data2G PR #118 answers DATA2G ON with WRONG: not
    // refused, and no cuts.
    HostLink old = opened(0);
    CHECK(!old.data2g());
    CHECK(!hasEvent(old.takeEvents(), Type::Refused));

    HostLink link({"ag7ew", true, false});
    link.commandUp(0);
    link.takeCommandOutput();
    say(link, "OK\rOK\rOK\rOK\rOK\rBCAST PORT 2\rOK\rTIMEOUT 300.0\rTXPOWER 0.0\r", 10);
    CHECK(link.data2g());
    CHECK(!link.canCut());
    CHECK(!link.cleanTxBuffer(100, 20)); // no session
    CHECK(link.call("W1AW", 30));
    say(link, "OK\rCONNECTED AG7EW W1AW 2300\r", 40);
    link.takeCommandOutput();
    link.takeEvents();
    CHECK(link.canCut());
    CHECK(link.cleanTxBuffer(4096, 50));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"CLEANTXBUFFER 4096"}));
    say(link, "CLEANTXBUFFER OK 1200 4096 980 1200\rCLEANTXBUFFER SETTLED 980 1100 1200\r", 60);
    std::vector<HostLink::Event> events = link.takeEvents();
    CHECK_EQ(events.size(), (size_t)2);
    if (events.size() == 2)
    {
        CHECK(events[0].type == Type::Clean && events[0].clean.result == CommandEvent::Clean::Result::Ok
              && events[0].clean.offsets.size() == 4);
        CHECK(events[1].type == Type::Clean && events[1].clean.result == CommandEvent::Clean::Result::Settled);
    }
    // The answer was awaited: an OK after it answers the next command.
    link.setNarrow(true, 70);
    link.takeCommandOutput();
    say(link, "WRONG\rOK\r", 80);
    CHECK(hasEvent(link.takeEvents(), Type::Refused));

    // A host that answers WRONG can't cut after all.
    CHECK(link.cleanTxBuffer(5000, 90));
    say(link, "WRONG\r", 100);
    events = link.takeEvents();
    CHECK(events.size() == 1 && events[0].type == Type::Clean
          && events[0].clean.result == CommandEvent::Clean::Result::Failed);
    CHECK(!link.data2g() && !link.canCut());
}

void testTimeoutAndTxPower()
{
    // A host from before Data2G PR #118 isn't asked.
    HostLink old = opened(0);
    CHECK(!old.idleTimeout() && !old.txPower());
    CHECK(!old.setTxPower(-6.0, 10));
    CHECK(old.takeCommandOutput().empty());

    // Since PR #122: once DATA2G ON is taken, the host's own idle timeout
    // is read, ours set and read back, and the output volume read.
    HostLink::Settings settings{"ag7ew", true, false};
    settings.idleTimeoutS = HostLink::RAGCHEW_IDLE_TIMEOUT_S;
    HostLink link(settings);
    link.commandUp(0);
    link.takeCommandOutput();
    say(link, "OK\rOK\rOK\r", 10);
    CHECK(linesOf(link.takeCommandOutput())
          == std::vector<std::string>({"TIMEOUT", "TIMEOUT 86400.0", "TIMEOUT", "TXPOWER"}));
    say(link, "OK\rOK\rBCAST PORT 2\rOK\rTIMEOUT 300.0\rOK\rTIMEOUT 86400.0\rTXPOWER -3.0\r", 20);
    std::vector<HostLink::Event> events = link.takeEvents();
    CHECK(!hasEvent(events, Type::Refused));
    CHECK(link.idleTimeout() && *link.idleTimeout() == 86400.0);
    CHECK(link.txPower() && *link.txPower() == -3.0);
    int timeouts = 0;
    for (const auto& e : events)
    {
        if (e.type == Type::IdleTimeout) timeouts++;
        if (e.type == Type::TxPower) CHECK(e.value == -3.0);
    }
    CHECK_EQ(timeouts, 2);

    // The output volume: set, then read back.
    CHECK(link.setTxPower(-12.54, 30));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"TXPOWER -12.5", "TXPOWER"}));
    say(link, "OK\rTXPOWER -12.5\r", 40);
    events = link.takeEvents();
    CHECK(events.size() == 1 && events[0].type == Type::TxPower && events[0].value == -12.5);
    CHECK(!link.setTxPower(-60.5, 50) && !link.setTxPower(20.5, 50));
    CHECK(link.setTxPower(20.0, 50));
    link.takeCommandOutput();
    say(link, "WRONG\rTXPOWER -12.5\r", 60);
    events = link.takeEvents();
    CHECK(events.size() == 1 && events[0].type == Type::Refused && events[0].text == "TXPOWER 20.0");

    // Back to the host's own idle timeout, as first read.
    link.setIdleTimeout(0.0, 70);
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"TIMEOUT 300.0", "TIMEOUT"}));
    say(link, "OK\rTIMEOUT 300.0\r", 80);
    CHECK(*link.idleTimeout() == 300.0);
    link.setIdleTimeout(0.0, 90);
    CHECK(link.takeCommandOutput().empty());

    // Gone with the command port.
    link.commandDown(100);
    CHECK(!link.idleTimeout() && !link.txPower());

    // A host between PRs #118 and #122 answers WRONG: nothing refused.
    HostLink between({"ag7ew", true, false});
    between.commandUp(0);
    between.takeCommandOutput();
    say(between, "OK\rOK\rOK\rOK\rOK\rBCAST PORT 2\rOK\rWRONG\rWRONG\r", 10);
    CHECK(between.data2g() && !between.idleTimeout() && !between.txPower());
    CHECK(!hasEvent(between.takeEvents(), Type::Refused));
}

void testDisconnectReasons()
{
    // The host's reason comes with SessionEnded.
    HostLink link = opened(0);
    CHECK(link.call("W1AW", 10));
    say(link, "OK\rDISCONNECTED BUSY\r", 20);
    std::vector<HostLink::Event> events = link.takeEvents();
    bool busy = false;
    for (const auto& e : events)
    {
        busy = busy || (e.type == Type::SessionEnded && e.sessionEnd == HostLink::SessionEnd::NoAnswer
                        && e.disconnect.reason == "BUSY");
    }
    CHECK(busy);

    CHECK(link.call("W1AW", 30));
    say(link, "OK\rCONNECTED AG7EW W1AW 2300\rDISCONNECTED LINK_FAILED no progress\r", 40);
    events = link.takeEvents();
    CHECK(!events.empty() && events.back().type == Type::SessionEnded);
    if (!events.empty())
    {
        CHECK(events.back().sessionEnd == HostLink::SessionEnd::Ended);
        CHECK(events.back().disconnect.reason == "LINK_FAILED" && events.back().disconnect.detail == "no progress");
    }

    CHECK(link.call("W1AW", 50));
    say(link, "OK\rCONNECTED AG7EW W1AW 2300\r", 60);
    CHECK(link.leave(70));
    say(link, "OK\rDISCONNECTED CLIENT unconfirmed\r", 80);
    events = link.takeEvents();
    CHECK(!events.empty() && events.back().type == Type::SessionEnded);
    if (!events.empty())
    {
        CHECK(events.back().sessionEnd == HostLink::SessionEnd::Left);
        CHECK(events.back().disconnect.reason == "CLIENT" && events.back().disconnect.unconfirmed);
    }
}

void testTheLastModeReported()
{
    // MODE comes only when the mode changes, so the last one is kept.
    HostLink link = opened(0);
    CHECK(link.lastMode().empty());
    say(link, "PTT ON\rMODE qpsk-r1/3\rPTT OFF\r", 10);
    CHECK_EQ(link.lastMode(), std::string("qpsk-r1/3"));
    link.commandDown(20);
    CHECK(link.lastMode().empty());
}

void testAnUnansweredCommandIsNotWaitedOnForEver()
{
    // A host that never answers BW2300, say: the burst's BCAST MODE still
    // goes once the wait runs out.
    HostLink link({"AG7EW", true, false});
    link.commandUp(0);
    link.takeCommandOutput();
    say(link, "OK\rOK\rWRONG\rOK\rBCAST PORT 2\r", 10); // one OK short, and no MODES answer
    uint64_t id = link.broadcast({{0xA0, 1}}, "qpsk-r1/3", 20);
    CHECK(link.takeCommandOutput().empty());
    link.tick(HostLink::ANSWER_MS + 20);
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"BCAST MODE 2 qpsk-r1/3"}));
    say(link, "OK\r", HostLink::ANSWER_MS + 30);
    CHECK(link.burstState(id) == BurstState::Sending);
}

void testNothingWithoutTheHost()
{
    HostLink link({"AG7EW", true, false});
    uint64_t id = link.broadcast({{0xA0, 1}}, "", 0);
    CHECK(link.burstState(id) == BurstState::Queued);
    CHECK(link.takeKissOutput().empty());
    CHECK(!link.call("W1AW", 0));
    CHECK(link.broadcast({}, "", 0) == 0);
}

} // namespace

// The status bar's first field says what of ours is going out (Jeff,
// 2026-10-23), and its tooltip lists it all.
void testWhatIsGoingOut()
{
    HostLink link = opened(0);
    CHECK_EQ(channelLine(link), std::string("Channel clear"));
    say(link, "BUSY ON\r", 5);
    CHECK_EQ(channelLine(link), std::string("Channel busy"));
    say(link, "BUSY OFF\r", 6);

    uint64_t seal = link.broadcast({{0xA0, 1}}, "", 10, "seal");
    link.broadcast({{0xA0, 2}}, "", 10, snippet("CQ CQ de AG7EW"));
    std::vector<uint16_t> tags = tagsWritten(link, 2);
    CHECK_EQ(link.outgoing().size(), (size_t)2);
    CHECK(link.outgoing().front().id == seal && link.outgoing().front().state == BurstState::Sending);
    CHECK_EQ(channelLine(link), std::string("Sending: seal (+1)"));
    say(link, "BUSY ON\r", 20);
    CHECK_EQ(channelLine(link), std::string("Busy, next: seal (+1)"));
    CHECK_EQ(channelTip(link), std::string("Channel busy: data2g-host hears a signal\nWaiting for a clear channel: seal\n"
                                           "Queued: \xe2\x80\x9c" "CQ CQ de AG7EW\xe2\x80\x9d"));
    say(link, "BUSY OFF\r", 30);
    ackTags(link, 2, tags, 40);
    CHECK_EQ(channelLine(link), std::string("Sending: \xe2\x80\x9c" "CQ CQ de AG7EW\xe2\x80\x9d"));
    ackTags(link, 2, tagsWritten(link, 2), 50);
    CHECK(link.outgoing().empty());
    CHECK_EQ(channelLine(link), std::string("Channel clear"));

    // Paused, and held for a session; one with no label still shows.
    link.setPaused(true, 60);
    link.broadcast({{0xA0, 3}}, "", 60, "QRV");
    CHECK_EQ(channelLine(link), std::string("Paused: QRV"));
    link.setPaused(false, 70);
    tagsWritten(link, 2);
    say(link, "CONNECTED W1AW AG7EW 2300\rBUFFER 56\r", 80);
    link.broadcast({{0xA0, 4}}, "", 90);
    CHECK_EQ(channelLine(link), std::string("Held: QRV (+1)"));
    CHECK_EQ(channelTip(link), std::string("Channel clear\nHeld while the session is busy: QRV\nQueued: a burst\n"
                                           "Session: 56 bytes not yet acknowledged by W1AW"));
}

// A message's start: on one line, cut between words, UTF-8 kept whole.
void testSnippets()
{
    CHECK_EQ(snippet("hi"), std::string("\xe2\x80\x9chi\xe2\x80\x9d"));
    CHECK_EQ(snippet("  two\nlines  "), std::string("\xe2\x80\x9ctwo lines\xe2\x80\x9d"));
    CHECK_EQ(snippet("CQ CQ CQ de AG7EW AG7EW K", 12),
             std::string("\xe2\x80\x9c" "CQ CQ CQ de\xe2\x80\xa6\xe2\x80\x9d"));
    // No space in the second half: cut where the count ends.
    CHECK_EQ(snippet("abcdefghijklmnop", 5), std::string("\xe2\x80\x9c" "abcde\xe2\x80\xa6\xe2\x80\x9d"));
    // Five characters, the last two of them two bytes each.
    CHECK_EQ(snippet("abc\xc3\xa9\xc3\xa9xyz", 5),
             std::string("\xe2\x80\x9c" "abc\xc3\xa9\xc3\xa9\xe2\x80\xa6\xe2\x80\x9d"));
}

void testBurstsThatYield()
{
    const uint64_t gap = HostLink::YIELD_GAP_MS;
    HostLink link = opened(0);
    // A chat line goes, and is sent at 1000.
    uint64_t chat = link.broadcast({{0xA0, 1}}, "", 100, "hi");
    ackTags(link, 2, tagsWritten(link, 2), 1000);
    CHECK(link.burstState(chat) == BurstState::Sent);
    CHECK_EQ(link.lastSentMs(), (uint64_t)1000);

    // A file's bursts right behind it wait the gap, Queued and cancellable.
    uint64_t piece = link.broadcast({{0xA0, 2}}, "", 1100, "piece 1", true);
    uint64_t piece2 = link.broadcast({{0xA0, 3}}, "", 1100, "piece 2", true);
    CHECK(link.takeKissOutput().empty());
    CHECK(link.burstState(piece) == BurstState::Queued);

    // A message typed meanwhile goes ahead of them, and its own Sent
    // starts the gap again.
    uint64_t reply = link.broadcast({{0xA0, 4}}, "", 2000, "reply");
    std::vector<uint16_t> tags = tagsWritten(link, 2);
    CHECK_EQ(tags.size(), (size_t)1);
    CHECK(link.burstState(reply) == BurstState::Sending);
    ackTags(link, 2, tags, 3000);
    link.tick(1000 + gap);
    CHECK(link.takeKissOutput().empty());
    CHECK(link.burstState(piece) == BurstState::Queued);
    link.tick(3000 + gap - 1);
    CHECK(link.takeKissOutput().empty());

    // Then the first piece, in its turn; the second waits behind it.
    link.tick(3000 + gap);
    std::vector<std::vector<uint8_t>> frames;
    tags = tagsWritten(link, 2, &frames);
    CHECK(frames == std::vector<std::vector<uint8_t>>({{0xA0, 2}}));
    CHECK(link.burstState(piece2) == BurstState::Queued);
    ackTags(link, 2, tags, 20000);
    CHECK(link.burstState(piece) == BurstState::Sent);
    CHECK(link.cancel(piece2));

    // A hold keeps them waiting past the gap, and ends when lifted.
    uint64_t piece3 = link.broadcast({{0xA0, 5}}, "", 20000, "piece 3", true);
    link.holdYielding(40000, 20000);
    link.tick(20000 + gap);
    CHECK(link.takeKissOutput().empty());
    CHECK(link.yieldHold() == 40000);
    link.holdYielding(0, 30000);
    tags = tagsWritten(link, 2);
    CHECK_EQ(tags.size(), (size_t)1);
    CHECK(link.burstState(piece3) == BurstState::Sending);

    // Bursts that don't yield never wait the gap.
    ackTags(link, 2, tags, 31000);
    uint64_t chat2 = link.broadcast({{0xA0, 6}}, "", 31100, "hi again");
    CHECK_EQ(tagsWritten(link, 2).size(), (size_t)1);
    CHECK(link.burstState(chat2) == BurstState::Sending);
}

// Hands (Data2G PR #146): references, a Hand sent, net control's window.
void testHands()
{
    HostLink link = opened(0);
    // BW2300: all 40 channels; BW500: 8.
    CHECK_EQ(link.handChannels(), 40);
    CHECK_EQ(opened(0, true).handChannels(), 8);

    // A REF line on the command port, and a REF frame on the KISS port
    // ahead of its burst's frames: those frames carry it. Either shows
    // the host does Hands.
    CHECK(!link.handsSeen());
    say(link, "REF 7 123.45 -3.5 -8.0\r", 10);
    CHECK(link.handsSeen());
    {
        HostLink other = opened(0);
        std::string t = "REF 1 1.00 0.0 0.0";
        std::vector<uint8_t> r{KISS_REF};
        r.insert(r.end(), t.begin(), t.end());
        std::vector<uint8_t> k = kissEncodeSetHardware(0, r);
        other.kissBytes(k.data(), k.size(), 10);
        CHECK(other.handsSeen());
        other.commandDown(20);
        CHECK(!other.handsSeen());
    }
    std::vector<HostLink::Event> events = link.takeEvents();
    CHECK(events.size() == 1 && events[0].type == Type::HandRef && events[0].handRef
          && events[0].handRef->id == 7 && events[0].handRef->endS == 123.45 && events[0].handRef->offsetHz == -3.5
          && events[0].handRef->snr == -8.0);
    std::string text = "REF 8 130.00 2.0 4.5";
    std::vector<uint8_t> ref{KISS_REF};
    ref.insert(ref.end(), text.begin(), text.end());
    std::vector<uint8_t> in = kissEncodeSetHardware(0, ref);
    for (const std::vector<uint8_t>& frame : {std::vector<uint8_t>{1, 2}, std::vector<uint8_t>{3}})
    {
        std::vector<uint8_t> k = kissEncode(2, frame);
        in.insert(in.end(), k.begin(), k.end());
    }
    link.kissBytes(in.data(), in.size(), 20);
    events = link.takeEvents();
    CHECK_EQ(events.size(), (size_t)2);
    for (const auto& e : events) CHECK(e.type == Type::Heard && e.handRef && e.handRef->id == 8);

    // HAND SEND: checked here, then written.
    CHECK(!link.handSend(8, 256, 3.0, 2, 1, 30));
    CHECK(!link.handSend(8, 9, 30.0, 2, 1, 30));
    CHECK(!link.handSend(8, 9, 3.0, 40, 1, 30));
    CHECK(!link.handSend(0, 9, 3.0, 2, 1, 30));
    CHECK(link.takeCommandOutput().empty());
    CHECK(link.handSend(8, 9, 3.0, 2, 0xFEDCBA9876543210ull, 30));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"HAND SEND 8 9 3.000 2 7edcba9876543210"}));
    say(link, "OK\rHAND SENT 2\r", 40);
    events = link.takeEvents();
    CHECK(events.size() == 1 && events[0].type == Type::HandSent && events[0].number == 2);
    CHECK(link.handSend(8, 9, 3.0, 2, 1, 50));
    link.takeCommandOutput();
    say(link, "WRONG\r", 60);
    events = link.takeEvents();
    CHECK(events.size() == 1 && events[0].type == Type::Refused && events[0].text.compare(0, 10, "HAND SEND ") == 0);

    // Net control: the window is asked for, named by the burst's first
    // tag, and the burst waits for the OK.
    uint64_t id = link.broadcast({{1, 2, 3}, {4}}, "", 100);
    CHECK(id != 0);
    std::vector<uint16_t> early = tagsWritten(link, 2);
    CHECK(early.size() == 2);   // written at once: it had no window
    ackTags(link, 2, early, 110);
    link.takeEvents();
    CHECK(!link.handListen(id, 3.0, 9, {0, 1}, 120));  // gone already
    // A burst waits behind a held one, so it is still queued to ask for.
    link.holdYielding(0, 120);
    link.setPaused(true, 120);
    id = link.broadcast({{5, 6}, {7}}, "", 130);
    CHECK(!link.handListen(id, 0.5, 9, {0}, 130));     // under HAND_LISTEN_MIN_S
    CHECK(!link.handListen(id, 3.0, 9, {}, 130));
    CHECK(!link.handListen(id, 3.0, 9, {40}, 130));
    CHECK(link.handListen(id, 3.0, 9, {0, 1, 2}, 130));
    CHECK(!link.handListen(id, 3.0, 9, {0}, 130));     // one window at a time
    link.setPaused(false, 140);
    std::vector<std::string> lines = linesOf(link.takeCommandOutput());
    CHECK(lines.size() == 1 && lines[0].compare(0, 12, "HAND LISTEN ") == 0);
    CHECK(link.takeKissOutput().empty());
    CHECK(link.burstState(id) == BurstState::Queued);
    char expected[64];
    std::snprintf(expected, sizeof expected, "HAND LISTEN 3.000 9 %04x 0 1 2", (unsigned)(early.back() + 1));
    CHECK_EQ(lines[0], std::string(expected));
    say(link, "OK\r", 150);
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint16_t> tags = tagsWritten(link, 2, &frames);
    CHECK(tags.size() == 2 && tags[0] == early.back() + 1);
    CHECK(frames.size() == 2 && frames[0] == std::vector<uint8_t>({5, 6}));
    events = link.takeEvents();
    CHECK(hasEvent(events, Type::HandArmed));
    ackTags(link, 2, tags, 160);
    link.takeEvents();

    // Its Hands, then the end.
    say(link, "HAND 2 000000000000002a -12.5 140\rHAND SHORT 300\rHAND END\r", 5000);
    events = link.takeEvents();
    CHECK_EQ(events.size(), (size_t)3);
    CHECK(events[0].type == Type::HandHeard && events[0].number == 2 && events[0].handPayload == 42 && events[0].snr
          && *events[0].snr == -12.5 && events[0].value == 140.0);
    CHECK(events[1].type == Type::HandShort && events[1].value == 300.0);
    CHECK(events[2].type == Type::HandEnd);

    // A window the host refuses: Refused, and the burst goes anyway.
    id = link.broadcast({{8}}, "", 6000);
    link.takeKissOutput();
    link.takeEvents();
    link.setPaused(true, 6000);
    id = link.broadcast({{9}}, "", 6000);
    CHECK(link.handListen(id, 2.0, 1, {3}, 6000));
    ackTags(link, 2, {(uint16_t)(tags.back() + 1)}, 6010);
    link.setPaused(false, 6020);
    CHECK(linesOf(link.takeCommandOutput()).size() == 1);
    say(link, "WRONG\r", 6030);
    std::vector<uint16_t> nine = tagsWritten(link, 2);
    CHECK(nine.size() == 1);
    events = link.takeEvents();
    CHECK(hasEvent(events, Type::Refused) && !hasEvent(events, Type::HandArmed));
    ackTags(link, 2, nine, 6040);

    // HAND CANCEL: a burst still waiting on its window's OK goes without it.
    link.takeKissOutput();
    link.setPaused(true, 7000);
    id = link.broadcast({{10}}, "", 7000);
    CHECK(link.handListen(id, 2.0, 1, {3}, 7000));
    link.setPaused(false, 7010);
    link.takeCommandOutput();
    CHECK(link.handCancel(7020));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"HAND CANCEL"}));
    std::vector<uint16_t> ten = tagsWritten(link, 2);
    CHECK(ten.size() == 1);
    say(link, "OK\rOK\r", 7030);
    events = link.takeEvents();
    CHECK(!hasEvent(events, Type::Refused) && !hasEvent(events, Type::HandArmed));

    // A burst with its window from the start: HAND LISTEN before any
    // frame, even with nothing ahead of it.
    ackTags(link, 2, ten, 7990);
    link.takeKissOutput();
    link.takeEvents();
    HostLink::HandWindow window;
    window.delayS = 3.0;
    window.tag = 77;
    window.channels = {0, 1};
    HostLink::HandWindow bad = window;
    bad.channels = {40};
    CHECK_EQ(link.broadcast({{11}}, "", 8000, "roll call", bad), (uint64_t)0);
    CHECK(link.takeEvents().empty());
    id = link.broadcast({{11}}, "", 8000, "roll call", window);
    CHECK(id != 0);
    lines = linesOf(link.takeCommandOutput());
    CHECK(lines.size() == 1 && lines[0].compare(0, 20, "HAND LISTEN 3.000 77") == 0);
    CHECK(link.takeKissOutput().empty());
    CHECK_EQ(link.broadcast({{12}}, "", 8000, "another", window), (uint64_t)0);     // one window at a time
    say(link, "OK\r", 8010);
    CHECK(tagsWritten(link, 2).size() == 1);
    CHECK(hasEvent(link.takeEvents(), Type::HandArmed));

    // Nothing without the host.
    HostLink down({"ag7ew", true, false});
    CHECK(!down.handSend(1, 1, 3.0, 0, 1, 0) && !down.handCancel(0));
}

int main()
{
    testTheOpening();
    testReports();
    testSnrReports();
    testABurstGoesInItsMode();
    testAModeTheHostRefuses();
    testTheModeWaitsForEarlierAnswers();
    testNotSentAfterTwoMinutes();
    testASessionHoldsTheGroup();
    testDroppedAndPortLost();
    testPauseSending();
    testBurstsThatYield();
    testCallingAndLeaving();
    testAbortTransmit();
    testCuts();
    testTimeoutAndTxPower();
    testDisconnectReasons();
    testTheLastModeReported();
    testAnUnansweredCommandIsNotWaitedOnForEver();
    testNothingWithoutTheHost();
    testWhatIsGoingOut();
    testSnippets();
    testHands();
    return ragtime_test::finish();
}
