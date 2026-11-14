// The byte formats Ragtime uses to talk to a separately running
// data2g-host: KISS framing (with ACKMODE), the command port's lines, and
// the modes it offers. No sockets here, so all of it is tested on its own.
//
// Ported from Glissando's Data2GLink (written there for Glissando from the
// public KISS and VARA protocol descriptions and Data2G's
// docs/broadcast.md) and relicensed MIT by Jeff. No Data2G code is used.
// The session stream's framing is Ragtime's own: see
// ragtime/protocol/Stream.h.
#ifndef RAGTIME_LINK_DATA2G_LINK_H
#define RAGTIME_LINK_DATA2G_LINK_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ragtime
{
namespace link
{

constexpr int DEFAULT_KISS_PORT = 8100;
constexpr int DEFAULT_COMMAND_PORT = 8300; // the session data port is the next one

// The broadcast group Ragtime opens. Its bursts carry the name, and every
// codeword a CRC mask hashed from it, so only stations that opened D2G
// take them in. Never GLISS: Ragtime and Glissando don't read each other.
constexpr const char* GROUP = "D2G";

//-------------------------------------------------------------------------
// KISS
//-------------------------------------------------------------------------

constexpr uint8_t KISS_DATA = 0x00;
constexpr uint8_t KISS_ACKMODE = 0x0C;
constexpr uint8_t KISS_PERSIST = 0x02;      // KISS P: one byte, a slot taken with probability (P + 1) / 256
constexpr uint8_t KISS_SETHARDWARE = 0x06;  // data2g-host's subcommands: a first byte 0x20-0x7E is text
constexpr uint8_t KISS_REF = 0x11;          // SETHARDWARE subcommand, host to app: a heard burst's REF line

// A data frame for a KISS port (0 to 15).
std::vector<uint8_t> kissEncode(int port, const std::vector<uint8_t>& data);

// An ACKMODE frame: data2g-host answers with the same tag once the burst
// carrying it has been transmitted.
std::vector<uint8_t> kissEncodeAckMode(int port, uint16_t tag, const std::vector<uint8_t>& data);
// A KISS command with a one-byte value, such as KISS_PERSIST.
std::vector<uint8_t> kissEncodeCommand(int port, uint8_t command, uint8_t value);
// SETHARDWARE "REFS ON" (or OFF) on port 0: from then on, on this
// connection, the host sends each decoded burst's Hand reference as a
// REF frame (SETHARDWARE, subcommand KISS_REF, then the REF line's text)
// ahead of that burst's frames (Data2G PR #146, docs/DATA2G.md "Hands").
std::vector<uint8_t> kissEncodeRefs(bool on);
// A SETHARDWARE frame with this payload (a subcommand byte, or text).
std::vector<uint8_t> kissEncodeSetHardware(int port, const std::vector<uint8_t>& payload);

struct KissFrame
{
    int port = 0;
    uint8_t command = KISS_DATA;    // low nibble of the command byte
    // Data; for an ACKMODE ack the 2-byte tag; for SETHARDWARE the
    // subcommand byte and its text (a REF frame: KISS_REF, "REF ...").
    std::vector<uint8_t> payload;
};

// Splits a KISS byte stream into frames, however it arrives in pieces.
class KissDecoder
{
public:
    void feed(const uint8_t* bytes, size_t length, std::vector<KissFrame>& framesOut);
    void reset();

private:
    std::vector<uint8_t> current_;
    bool escaped_ = false;
    bool inFrame_ = false;
};

//-------------------------------------------------------------------------
// Modes
//-------------------------------------------------------------------------

// The most bytes an n-byte frame takes in a burst's data. Broadcast v3
// (Data2G PR #121) COBS-encodes each frame and ends it with a zero: n + 2
// up to 253 bytes, one more per 254. That is the 2-byte length of v2 up
// to 253 bytes, so it counts both.
constexpr int framedBytes(int n) { return n + 2 + n / 254; }

// The bytes these frames take in a burst's data, back to back: each
// framed, and in v3 a zero more before any frame that would start a
// codeword, the first included. At most a byte a frame over v2's count.
int burstDataBytes(const std::vector<int>& frameBytes, int bytesPerCodeword);

// One line of the MODES list: "MODE name bandwidth-Hz bytes-per-codeword
// max-codewords seconds-at-1 seconds-at-max".
struct ModeInfo
{
    std::string name;
    int bandwidthHz = 0;
    int bytesPerCodeword = 0;
    int maxCodewords = 0;
    double secondsAtOne = 0.0;
    double secondsAtMax = 0.0;
    // S/N thresholds from hosts since Data2G PR #186: per channel name
    // ("awgn", "mpg", "mpp", "mpd"), the 10% codeword failure S/N in dB,
    // none where the mode never got there. Empty from an older host.
    std::map<std::string, std::optional<double>> thresholds;

    bool valid() const { return !name.empty() && bytesPerCodeword > 0 && maxCodewords > 0; }

    // How long a burst carrying these frames takes: one control codeword,
    // then the frames back to back (burstDataBytes).
    double burstSeconds(const std::vector<int>& frameBytes) const;
};

// The modes a 2400 Hz data2g-host offered when this was written (Data2G
// 8ffe15c, as Glissando measured them); what the host's own MODES list
// says wins.
const std::vector<ModeInfo>& knownModes();

// One of Data2G's session acknowledgement modes ("ack-4f"): 4 bytes a
// codeword, for a session's ACK bursts, never a gear data goes in. The
// host's MODE report names the mode of whatever burst it keys, these
// included (docs/DATA2G.md, "Modes").
bool isAckMode(const std::string& mode);

// The operator's 500 Hz setting: of the modes offered (or, none listed
// yet, the known ones), those no wider than this.
constexpr int NARROW_MODE_MAX_HZ = 500;
std::vector<ModeInfo> narrowModes(const std::vector<ModeInfo>& offered);

//-------------------------------------------------------------------------
// Hands (Data2G PR #146, docs/DATA2G.md "Hands")
//-------------------------------------------------------------------------

// The show of hands: after one burst, every station that heard it can
// answer at once with a Hand, 63 bits in one of 40 narrow channels
// (6.25 Hz 8-FSK, 60 Hz apart round 1500 Hz), 10.56 s long. Only a
// station that knows when and where to look (net control) decodes them.
constexpr int HAND_CHANNELS = 40;
constexpr int HAND_PAYLOAD_BITS = 63;
constexpr uint64_t HAND_PAYLOAD_MASK = (uint64_t(1) << HAND_PAYLOAD_BITS) - 1;
constexpr double HAND_SECONDS = 10.56;
// Net control's window: the Hand's slot, give or take this.
constexpr double HAND_SEARCH_S = 0.6;
// HAND LISTEN's delay: at least this, and both commands' under the most.
constexpr double HAND_LISTEN_MIN_S = 1.0;
constexpr double HAND_DELAY_MAX_S = 30.0;

// Channel c's centre, Hz, numbered from the centre out: 0 at 1470, 1 at
// 1530, 2 at 1410 and so on, even below 1500 Hz and odd above.
double handChannelHz(int channel);
// The channels that fit a width: a Hand is 50 Hz wide, so 8 in 500 Hz,
// 20 in 1200 and all 40 in 2400. The host refuses the rest.
int handChannelsWithin(int widthHz);

// A heard burst as a Hand reference: "REF id end offset snr". The id
// counts up from 1 on each host (its last 64 stay usable); end is the
// burst's end in host seconds, on the sound card's clock; offset its
// measured frequency offset, Hz (0 for modes that don't measure one);
// snr its pilot SNR, dB.
struct HandRef
{
    int id = 0;
    double endS = 0.0;
    double offsetHz = 0.0;
    double snr = 0.0;
};
// A REF line's text, from the command port or a KISS REF frame.
std::optional<HandRef> parseHandRef(const std::string& text);

// HAND SEND ref tag d channel hex16: answer heard burst `ref` with a Hand
// d seconds after its end. The payload's top bit is not sent.
std::string handSendLine(int ref, int tag, double delayS, int channel, uint64_t payload);
// HAND LISTEN d tag ack channel...: net control's window, d seconds after
// the end of the burst carrying the frame sent with ACKMODE tag ack.
std::string handListenLine(double delayS, int tag, uint16_t ack, const std::vector<int>& channels);
// A delay as the host reads it: digits and a point, never an exponent or
// a locale's comma.
std::string handDelayText(double seconds);

//-------------------------------------------------------------------------
// The command port
//-------------------------------------------------------------------------

// What a line from the command port means to us.
struct CommandEvent
{
    enum class Type
    {
        Other,          // IAMALIVE, VERSION and anything Ragtime has no use for
        Ok,
        Wrong,
        Ptt,            // on
        Busy,           // on
        Mode,           // text: the submode a burst went out in
        ModeLine,       // modeInfo: one line of the MODES list
        Channels,       // words: the THRESHOLD lines' columns (Data2G PR #186)
        ModeThreshold,  // text: mode, dBs: one per channel, none for "-"
        BcastPort,      // number: the port BCAST OPEN gave
        BcastHeard,     // number, text: the sender's call when sent with FROM, snr
        BcastLost,      // number, count
        BcastDropped,   // number, count
        BcastMissed,    // text: the submode, count: its codewords, snr (any port's: "BCAST *")
        Sn,             // snr: the session partner's last burst (the answer to SN)
        Connected,      // text: caller, peer: called station, number: bandwidth when given
        Disconnected,   // disconnect: why, from hosts since Data2G PR #122
        Buffer,         // count: bytes still to send in the session
        Cqframe,
        Clean,          // clean: a CLEANTXBUFFER answer, or its SETTLED report
        TxPower,        // value: the output volume, dB (the answer to TXPOWER)
        Timeout,        // value: the link idle timeout, seconds (the answer to TIMEOUT)
        // Hands (Data2G PR #146), with DATA2G ON or after a HAND command:
        Ref,            // handRef: a burst this host decoded
        HandSent,       // number: the channel of our Hand, keyed
        HandDropped,    // number: our Hand's channel, not sent (the radio busy past its slot)
        Hand,           // number: channel, handPayload, snr, value: its lateness, ms
        HandShort,      // value: ms of the window's head missed
        HandEnd,        // the window is over (or never opened)
    } type = Type::Other;

    bool on = false;
    int number = 0;
    int count = 0;
    std::string text;
    std::string peer;
    ModeInfo modeInfo;
    // dB, average power in 2500 Hz, on the gearshift's scale. Hosts since
    // Data2G PR #98 end HEARD with the burst's pilot SNR and MISSED with an
    // energy reading (once they know the noise floor); older hosts send
    // neither, and a HEARD without one still parses.
    std::optional<double> snr;
    double value = 0.0;
    HandRef handRef;
    uint64_t handPayload = 0;
    std::vector<std::string> words;
    std::vector<std::optional<double>> dBs;

    // Why a session ended (Data2G PR #122, with DATA2G ON): "DISCONNECTED
    // reason [detail] [unconfirmed]". The reason is one word: CLIENT (our
    // DISCONNECT), IDLE (the host's idle timeout), PEER (the far end left),
    // ABORT, LINK_LOST, LINK_FAILED (detail: why), NO_ANSWER,
    // PEER_RECONNECTED, or the far host's refusal: BUSY, VERSION,
    // NOT_LISTENING, REFUSED (an unknown one: "REFUSED reason n"). CLIENT
    // and IDLE end in "unconfirmed" when the far end never acknowledged
    // the DISC. Empty from an older host, or without DATA2G ON.
    struct Disconnect
    {
        std::string reason;
        std::string detail;
        bool unconfirmed = false;
    } disconnect;

    // CLEANTXBUFFER (Data2G PR #118; with DATA2G ON, host byte offsets
    // since CONNECTED, ranges [from, to)):
    // - Ok: drop from, drop to, limbo from, limbo to: the bytes dropped,
    //   and those on air not yet acknowledged.
    // - Late: nothing dropped, the offset asked for is on air already.
    // - Empty (BUFFEREMPTY): nothing queued. Failed: no session.
    // - Settled: from, split, to: the limbo settled, delivered before split,
    //   dropped from it; unknown when the session ended first.
    struct Clean
    {
        enum class Result
        {
            Ok,
            Late,
            Empty,
            Failed,
            Settled,
        } result = Result::Ok;
        std::vector<uint64_t> offsets;
        bool unknown = false;
    } clean;
};

CommandEvent parseCommandLine(const std::string& line);

// Splits the command port's CR (or LF) terminated lines.
class LineSplitter
{
public:
    void feed(const char* bytes, size_t length, std::vector<std::string>& linesOut);
    void reset() { current_.clear(); }

private:
    std::string current_;
};

// The callsign as the command port and a BCAST FROM can carry it: letters,
// digits, '/' and '-', at most 10 characters, upper case.
std::string commandCallsign(const std::string& callsign);

} // namespace link
} // namespace ragtime

#endif
