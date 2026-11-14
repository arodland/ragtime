// KISS ports and ACKMODE, the command port's lines and the modes list.
// The KISS and command-line checks are ported from Glissando's Data2GTest.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/link/Data2GLink.h"

using namespace ragtime::link;

namespace
{

void testKissPortsAndAckMode()
{
    std::vector<uint8_t> data = {0x01, 0xC0, 0xDB, 0x7F};
    std::vector<uint8_t> stream = kissEncode(3, data);
    std::vector<uint8_t> ack = kissEncodeAckMode(12, 0xC0DB, data); // 12: the command byte is FEND
    stream.insert(stream.end(), ack.begin(), ack.end());
    // A TXDELAY command, which is not passed on.
    std::vector<uint8_t> txdelay = {0xC0, 0x01, 0x32, 0xC0};
    stream.insert(stream.end(), txdelay.begin(), txdelay.end());

    KissDecoder decoder;
    std::vector<KissFrame> frames;
    for (uint8_t b : stream) decoder.feed(&b, 1, frames);
    CHECK_EQ(frames.size(), (size_t)2);
    if (frames.size() != 2) return;
    CHECK(frames[0].port == 3 && frames[0].command == KISS_DATA && frames[0].payload == data);
    CHECK(frames[1].port == 12 && frames[1].command == KISS_ACKMODE);
    std::vector<uint8_t> tagged = {0xC0, 0xDB};
    tagged.insert(tagged.end(), data.begin(), data.end());
    CHECK(frames[1].payload == tagged);

    // All at once, and after a reset mid-frame.
    frames.clear();
    decoder.reset();
    std::vector<uint8_t> broken = {0xC0, 0x00, 0x11};
    decoder.feed(broken.data(), broken.size(), frames);
    decoder.reset();
    decoder.feed(stream.data(), stream.size(), frames);
    CHECK_EQ(frames.size(), (size_t)2);
}

void testCommandLines()
{
    LineSplitter splitter;
    std::vector<std::string> lines;
    const char* part1 = "PTT ON\rBUSY O";
    const char* part2 = "FF\rMODE n10-qpsk-r1/5\rMODE qpsk-r1/3 1200 76 64 1.78 74.36\rBCAST PORT 2\r"
                        "BCAST 2 HEARD VK3ABC\rBCAST 2 DROPPED 3\rCONNECTED W1AW VK3ABC 2300\rBUFFER 120\rOK\r";
    splitter.feed(part1, std::strlen(part1), lines);
    splitter.feed(part2, std::strlen(part2), lines);
    CHECK_EQ(lines.size(), (size_t)10);
    if (lines.size() != 10) return;

    using Type = CommandEvent::Type;
    CommandEvent e = parseCommandLine(lines[0]);
    CHECK(e.type == Type::Ptt && e.on);
    e = parseCommandLine(lines[1]);
    CHECK(e.type == Type::Busy && !e.on);
    e = parseCommandLine(lines[2]);
    CHECK(e.type == Type::Mode && e.text == "n10-qpsk-r1/5");
    e = parseCommandLine(lines[3]);
    CHECK(e.type == Type::ModeLine && e.modeInfo.name == "qpsk-r1/3" && e.modeInfo.bandwidthHz == 1200 &&
          e.modeInfo.bytesPerCodeword == 76 && e.modeInfo.maxCodewords == 64 &&
          std::fabs(e.modeInfo.secondsAtMax - 74.36) < 1e-6);
    e = parseCommandLine(lines[4]);
    CHECK(e.type == Type::BcastPort && e.number == 2);
    e = parseCommandLine(lines[5]);
    CHECK(e.type == Type::BcastHeard && e.number == 2 && e.text == "VK3ABC");
    e = parseCommandLine(lines[6]);
    CHECK(e.type == Type::BcastDropped && e.count == 3);
    e = parseCommandLine(lines[7]);
    CHECK(e.type == Type::Connected && e.text == "W1AW" && e.peer == "VK3ABC" && e.number == 2300);
    e = parseCommandLine(lines[8]);
    CHECK(e.type == Type::Buffer && e.count == 120);
    CHECK(parseCommandLine(lines[9]).type == Type::Ok);
    CHECK(parseCommandLine("IAMALIVE").type == Type::Other);
    CHECK(parseCommandLine("BCAST 2 LOST 4").type == Type::BcastLost);
    CHECK(parseCommandLine("WRONG").type == Type::Wrong);
    CHECK(parseCommandLine("DISCONNECTED").type == Type::Disconnected);
    CHECK(parseCommandLine("connected k1abc w1aw").text == "K1ABC");
    // A MODE line that doesn't parse is not a mode.
    CHECK(parseCommandLine("MODE qpsk-r1/3 wide 76 64 1.78 74.36").type == Type::Other);
    CHECK(parseCommandLine("BUFFER lots").type == Type::Other);

    // Thresholds in MODES, from hosts since Data2G PR #186.
    e = parseCommandLine("CHANNELS awgn mpg mpp mpd");
    CHECK(e.type == Type::Channels && e.words == std::vector<std::string>({"awgn", "mpg", "mpp", "mpd"}));
    e = parseCommandLine("THRESHOLD qpsk-r1/3 -4.5 -1.25 - 3");
    CHECK(e.type == Type::ModeThreshold && e.text == "qpsk-r1/3" && e.dBs.size() == 4 && e.dBs[0] == -4.5 &&
          !e.dBs[2] && e.dBs[3] == 3.0);
    CHECK(parseCommandLine("THRESHOLD qpsk-r1/3 loud").type == Type::Other);

    // SNRs, from hosts since Data2G PR #98: the last word of HEARD (a
    // callsign always has a letter), and of MISSED once the noise floor
    // is known. A HEARD without one, as before, has none.
    CHECK(!e.snr);
    e = parseCommandLine("BCAST 2 HEARD W1AW 12.3");
    CHECK(e.type == Type::BcastHeard && e.number == 2 && e.text == "W1AW" && e.snr && *e.snr == 12.3);
    e = parseCommandLine("BCAST 2 HEARD -7.5");
    CHECK(e.type == Type::BcastHeard && e.text.empty() && e.snr && *e.snr == -7.5);
    e = parseCommandLine("BCAST 2 HEARD 4X1AB");
    CHECK(e.type == Type::BcastHeard && e.text == "4X1AB" && !e.snr);
    e = parseCommandLine("BCAST 2 HEARD");
    CHECK(e.type == Type::BcastHeard && e.text.empty() && !e.snr);
    e = parseCommandLine("BCAST 2 HEARD W1AW nan");
    CHECK(e.type == Type::BcastHeard && e.text == "W1AW" && !e.snr);
    e = parseCommandLine("BCAST * MISSED w48-qpsk-r1/2 6 -3.1");
    CHECK(e.type == Type::BcastMissed && e.text == "w48-qpsk-r1/2" && e.count == 6 && e.snr && *e.snr == -3.1);
    e = parseCommandLine("BCAST * MISSED qpsk-r1/3 2");
    CHECK(e.type == Type::BcastMissed && e.count == 2 && !e.snr);
    e = parseCommandLine("SN 8.4");
    CHECK(e.type == Type::Sn && e.snr && *e.snr == 8.4);
    CHECK(parseCommandLine("SN").type == Type::Other);
    CHECK(parseCommandLine("SN loud").type == Type::Other);

    // LF too, and blank lines are dropped.
    lines.clear();
    const char* part3 = "OK\n\r\nPTT OFF\n";
    splitter.feed(part3, std::strlen(part3), lines);
    CHECK_EQ(lines.size(), (size_t)2);
}

void testCleanTxBufferLines()
{
    // Data2G PR #118, with DATA2G ON.
    using Type = CommandEvent::Type;
    using Result = CommandEvent::Clean::Result;
    CommandEvent e = parseCommandLine("CLEANTXBUFFER OK 1200 4096 980 1200");
    CHECK(e.type == Type::Clean && e.clean.result == Result::Ok);
    CHECK(e.clean.offsets == std::vector<uint64_t>({1200, 4096, 980, 1200}));
    e = parseCommandLine("CLEANTXBUFFER SETTLED 980 1100 1200");
    CHECK(e.type == Type::Clean && e.clean.result == Result::Settled && !e.clean.unknown);
    CHECK(e.clean.offsets == std::vector<uint64_t>({980, 1100, 1200}));
    e = parseCommandLine("CLEANTXBUFFER SETTLED 980 1100 1200 UNKNOWN");
    CHECK(e.type == Type::Clean && e.clean.result == Result::Settled && e.clean.unknown);
    CHECK(parseCommandLine("CLEANTXBUFFER LATE").clean.result == Result::Late);
    CHECK(parseCommandLine("CLEANTXBUFFER BUFFEREMPTY").clean.result == Result::Empty);
    // VARA's FAILED ends in a space.
    e = parseCommandLine("CLEANTXBUFFER FAILED ");
    CHECK(e.type == Type::Clean && e.clean.result == Result::Failed);
    // VARA's bare OK, from a host without DATA2G ON: no offsets.
    e = parseCommandLine("CLEANTXBUFFER OK");
    CHECK(e.type == Type::Clean && e.clean.result == Result::Ok && e.clean.offsets.empty());
    // Short or odd ones aren't taken.
    CHECK(parseCommandLine("CLEANTXBUFFER OK 1 2 3").type == Type::Other);
    CHECK(parseCommandLine("CLEANTXBUFFER SETTLED 1 x 3").type == Type::Other);
    CHECK(parseCommandLine("CLEANTXBUFFER OK -1 2 3 4").type == Type::Other);
    CHECK(parseCommandLine("CLEANTXBUFFER MAYBE").type == Type::Other);
}

void testDisconnectReasons()
{
    using Type = CommandEvent::Type;
    // Data2G PR #122, with DATA2G ON: DISCONNECTED says why.
    CommandEvent e = parseCommandLine("DISCONNECTED");
    CHECK(e.type == Type::Disconnected && e.disconnect.reason.empty() && e.disconnect.detail.empty());
    CHECK(!e.disconnect.unconfirmed);
    e = parseCommandLine("DISCONNECTED PEER");
    CHECK(e.type == Type::Disconnected && e.disconnect.reason == "PEER" && e.disconnect.detail.empty());
    e = parseCommandLine("DISCONNECTED idle unconfirmed");
    CHECK(e.disconnect.reason == "IDLE" && e.disconnect.unconfirmed && e.disconnect.detail.empty());
    e = parseCommandLine("DISCONNECTED LINK_FAILED no progress");
    CHECK(e.disconnect.reason == "LINK_FAILED" && e.disconnect.detail == "no progress" && !e.disconnect.unconfirmed);
    e = parseCommandLine("DISCONNECTED LINK_FAILED protocol: bad frame");
    CHECK(e.disconnect.detail == "protocol: bad frame");
    e = parseCommandLine("DISCONNECTED REFUSED reason 7");
    CHECK(e.disconnect.reason == "REFUSED" && e.disconnect.detail == "reason 7");

    // TXPOWER and TIMEOUT answers.
    e = parseCommandLine("TXPOWER -1.2");
    CHECK(e.type == Type::TxPower && e.value == -1.2);
    e = parseCommandLine("TIMEOUT 300.0");
    CHECK(e.type == Type::Timeout && e.value == 300.0);
    CHECK(parseCommandLine("TIMEOUT").type == Type::Other);
    CHECK(parseCommandLine("TIMEOUT soon").type == Type::Other);
    CHECK(parseCommandLine("TXPOWER inf").type == Type::Other);
    CHECK(parseCommandLine("TXPOWER 1 2").type == Type::Other);
}

void testCommandCallsign()
{
    CHECK_EQ(commandCallsign("vk2abc/p"), std::string("VK2ABC/P"));
    CHECK_EQ(commandCallsign("W1AW-7 "), std::string("W1AW-7"));
    CHECK_EQ(commandCallsign("ABCDEFGHIJKL"), std::string("ABCDEFGHIJ"));
}

void testModes()
{
    const std::vector<ModeInfo>& known = knownModes();
    CHECK(!known.empty());
    for (const ModeInfo& mode : known) CHECK(mode.valid());

    // The 500 Hz setting keeps only the narrow ones, from a host's list or
    // the known one.
    for (const auto& offered : {known, std::vector<ModeInfo>()})
    {
        std::vector<ModeInfo> narrow = narrowModes(offered);
        CHECK(!narrow.empty());
        for (const ModeInfo& mode : narrow) CHECK(mode.bandwidthHz <= NARROW_MODE_MAX_HZ);
    }

    // Framing: COBS and its zero (Data2G broadcast v3), the 2-byte length
    // of v2 up to 253 bytes.
    CHECK_EQ(framedBytes(74), 76);
    CHECK_EQ(framedBytes(253), 255);
    CHECK_EQ(framedBytes(254), 257);
    CHECK_EQ(framedBytes(255), 258);
    // And a zero before a frame that would start a codeword: two 73-byte
    // frames fill two 76-byte codewords; a 74-byte one starts the second
    // codeword inside it, so the next frame needs none.
    CHECK_EQ(burstDataBytes({73, 73}, 76), 152);
    CHECK_EQ(burstDataBytes({74, 10}, 76), 89);

    // A burst is one control codeword and the frames, each framed:
    // qpsk-r1/3 carries 76 bytes a codeword.
    ModeInfo qpsk;
    for (const ModeInfo& mode : known)
    {
        if (mode.name == "qpsk-r1/3") qpsk = mode;
    }
    CHECK(qpsk.valid());
    CHECK(std::fabs(qpsk.burstSeconds({73}) - (qpsk.secondsAtOne + (qpsk.secondsAtMax - qpsk.secondsAtOne) / 63)) < 1e-9);
    CHECK(qpsk.burstSeconds({74}) > qpsk.burstSeconds({73}));
    // Never past the most codewords a burst takes.
    CHECK(std::fabs(qpsk.burstSeconds({100000}) - qpsk.secondsAtMax) < 1e-9);
    CHECK(ModeInfo().burstSeconds({10}) == 0.0);
}

// Hands (Data2G PR #146): the channels, REF and HAND lines, the
// commands and the KISS port's REF frames.
void testHands()
{
    // From the centre out, even below 1500 Hz and odd above.
    CHECK(handChannelHz(0) == 1470.0 && handChannelHz(1) == 1530.0 && handChannelHz(2) == 1410.0);
    CHECK(handChannelHz(39) == 1500.0 + 30.0 + 19 * 60.0);
    CHECK_EQ(handChannelsWithin(500), 8);
    CHECK_EQ(handChannelsWithin(1200), 20);
    CHECK_EQ(handChannelsWithin(2400), 40);

    using Type = CommandEvent::Type;
    CommandEvent e = parseCommandLine("REF 12 4071.25 -2.5 -14.0");
    CHECK(e.type == Type::Ref && e.handRef.id == 12 && e.handRef.endS == 4071.25 && e.handRef.offsetHz == -2.5
          && e.handRef.snr == -14.0);
    CHECK(parseCommandLine("REF 0 1.0 0.0 0.0").type == Type::Other);
    CHECK(parseCommandLine("REF 3 1.0 0.0").type == Type::Other);
    CHECK(parseHandRef("REF 4 10.00 0.0 3.5") && parseHandRef("REF 4 10.00 0.0 3.5")->id == 4);
    CHECK(!parseHandRef("REF 4 nan 0.0 3.5"));

    e = parseCommandLine("HAND SENT 5");
    CHECK(e.type == Type::HandSent && e.number == 5);
    e = parseCommandLine("HAND DROPPED 6");
    CHECK(e.type == Type::HandDropped && e.number == 6);
    e = parseCommandLine("HAND 3 7fffffffffffffff -18.5 -220");
    CHECK(e.type == Type::Hand && e.number == 3 && e.handPayload == HAND_PAYLOAD_MASK && e.snr && *e.snr == -18.5
          && e.value == -220.0);
    // The top bit is never a Hand's.
    CHECK(parseCommandLine("HAND 3 ffffffffffffffff 1.0 0").handPayload == HAND_PAYLOAD_MASK);
    CHECK(parseCommandLine("HAND 3 xyz 1.0 0").type == Type::Other);
    e = parseCommandLine("HAND SHORT 250");
    CHECK(e.type == Type::HandShort && e.value == 250.0);
    CHECK(parseCommandLine("HAND END").type == Type::HandEnd);

    // The commands, numbers as the host reads them: no exponent, no comma.
    CHECK_EQ(handSendLine(12, 200, 3.0, 7, 0x123), std::string("HAND SEND 12 200 3.000 7 0000000000000123"));
    CHECK_EQ(handSendLine(1, 0, 0.25, 0, ~uint64_t(0)), std::string("HAND SEND 1 0 0.250 0 7fffffffffffffff"));
    CHECK_EQ(handListenLine(1.5, 9, 0x00AB, {0, 1, 2}), std::string("HAND LISTEN 1.500 9 00ab 0 1 2"));
    CHECK_EQ(handDelayText(2.0004), std::string("2.000"));
    CHECK_EQ(handDelayText(12.3456), std::string("12.346"));

    // REFS ON, byte for byte as Data2G's docs/broadcast.md gives it.
    CHECK(kissEncodeRefs(true) == std::vector<uint8_t>({0xC0, 0x06, 'R', 'E', 'F', 'S', ' ', 'O', 'N', 0xC0}));
    // A REF frame comes through the decoder with its subcommand.
    std::string text = "REF 9 1.00 0.0 2.0";
    std::vector<uint8_t> payload{KISS_REF};
    payload.insert(payload.end(), text.begin(), text.end());
    std::vector<uint8_t> stream = kissEncodeSetHardware(0, payload);
    KissDecoder decoder;
    std::vector<KissFrame> frames;
    decoder.feed(stream.data(), stream.size(), frames);
    CHECK(frames.size() == 1 && frames[0].command == KISS_SETHARDWARE && frames[0].payload == payload);
}

} // namespace

int main()
{
    testKissPortsAndAckMode();
    testCommandLines();
    testCleanTxBufferLines();
    testDisconnectReasons();
    testCommandCallsign();
    testModes();
    testHands();
    return ragtime_test::finish();
}
