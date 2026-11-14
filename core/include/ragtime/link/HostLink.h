// HostLink: Ragtime's side of the conversation with data2g-host, with no
// sockets. The app connects the KISS and command ports, hands HostLink
// what arrives on them, and writes what it gives back; the session data
// port (the command port + 1) is the app's to read and write, through
// protocol::StreamReader.
//
// What it does:
// - Opening: MYCALL, CHAT ON, DATA2G ON, LISTEN, BW500 or BW2300, BCAST
//   OPEN D2G FROM <call>, then MODES. DATA2G ON (Data2G PR #118) asks for
//   host byte offsets in CLEANTXBUFFER's answers; a host from before it
//   answers WRONG, which is not refused, and sessions there go without
//   cuts. Once the host takes it, TIMEOUT and TXPOWER (Data2G PR #122)
//   are read, and TIMEOUT set when the settings ask; a host from before
//   that PR answers WRONG, and is not refused for it either.
// - The host's reports: PTT, BUSY, MODE, the MODES list, BCAST HEARD,
//   LOST, DROPPED and MISSED, CONNECTED, DISCONNECTED, BUFFER. HEARD and
//   MISSED carry an SNR from hosts since Data2G PR #98, and DISCONNECTED
//   a reason from hosts since PR #122 (passed on with SessionEnded).
// - The session partner's SNR: SN, asked when its bursts have likely
//   arrived (BUFFER going down, session bytes in), at most every
//   SN_GAP_MS. A host that answers WRONG (one before PR #98, or before the
//   partner's first burst) is not refused for it.
// - Broadcasts: one burst at a time to the D2G port, each frame with an
//   ACKMODE tag, after BCAST MODE when the burst wants another mode. A
//   burst is Sent once the host has acknowledged every tag, and Not sent
//   if it reports it dropped, the command port closes, or two minutes pass
//   without it going while no session is up (a session holds the group;
//   docs/PLAN.md, "Broadcast mode").
// - Sessions: Call (CONNECT), Leave (DISCONNECT) and Leave now (ABORT).
//   Nothing else opens or closes one. With DATA2G ON, CLEANTXBUFFER n
//   drops what the host holds of our session bytes before offset n and
//   hasn't sent (a cut), and its answers and SETTLED reports come back as
//   Clean events.
//
// The time is always passed in, so tests run it on a simulated clock.
#ifndef RAGTIME_LINK_HOST_LINK_H
#define RAGTIME_LINK_HOST_LINK_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ragtime/link/Data2GLink.h"

namespace ragtime
{
namespace link
{

class HostLink
{
public:
    struct Settings
    {
        std::string callsign;
        bool acceptSessions = true;     // LISTEN ON
        bool narrow = false;            // BW500 (the 500 Hz button), else BW2300
        // The host's link idle timeout, set with TIMEOUT: a session with no
        // data either way for this long is closed (IDLE). 0 leaves the
        // host's own (Data2G's is 5 minutes). Each host keeps its own, so
        // a longer one holds a quiet session only if both ends set it.
        double idleTimeoutS = 0.0;
    };

    enum class BurstState
    {
        Queued,     // still with us: it can be cancelled
        Sending,    // written to the host, which sends it when the channel is clear
        Held,       // written to the host, which holds it while the session is busy
        Sent,
        NotSent,
    };

    enum class SessionState
    {
        None,
        Calling,    // CONNECT sent; the host calls up to five times
        Connected,
        Leaving,    // DISCONNECT sent: the host waits for what is unacknowledged
    };

    enum class SessionEnd
    {
        NoAnswer,   // the call was never answered
        Refused,    // the host answered the CONNECT with WRONG
        Left,       // we left (Leave or Leave now)
        Ended,      // the far end left, or the host gave up the link
        HostLost,   // the command port closed
    };

    // Ragtime's TIMEOUT: a ragchew ends on Leave, never for being quiet
    // (docs/PLAN.md), so the host is told not to hang up for a day. The
    // link's keepalives still find a station that has gone (LINK_LOST, 90
    // s), and a far end on another client still hangs up after its own.
    static constexpr double RAGCHEW_IDLE_TIMEOUT_S = 24 * 3600.0;

    // TXPOWER's range, dB: 0 puts a burst's peak at full scale.
    static constexpr double TX_POWER_MIN_DB = -60.0;
    static constexpr double TX_POWER_MAX_DB = 20.0;

    struct Event
    {
        enum class Type
        {
            GroupOpen,          // the D2G port is open: number
            GroupClosed,
            ModesChanged,       // modes() has a new list
            Ptt,                // on
            Busy,               // on
            ModeUsed,           // text: the mode a burst went out in
            Heard,              // frame: a frame heard on the group
            HeardFrom,          // text: the call a host heard a group burst from, snr
            GroupLost,          // count: bursts the host heard but couldn't decode
            Missed,             // text: submode, count: codewords, snr: a burst no port could claim
            Burst,              // id, burstState
            SessionCalling,     // peer
            SessionConnected,   // peer, incoming, number: bandwidth when given
            SessionEnded,       // peer, sessionEnd, disconnect: the host's reason, if it gave one
            Buffer,             // count: our session bytes not yet acknowledged
            SessionSnr,         // peer, snr: the partner's last burst as the host heard it
            Refused,            // text: a command the host answered WRONG
            Clean,              // clean: CLEANTXBUFFER's answer (Failed for WRONG), or SETTLED
            TxPower,            // value: the host's output volume, dB
            IdleTimeout,        // value: the host's link idle timeout, seconds
            // Hands (Data2G PR #146):
            HandRef,            // handRef: a burst the host decoded (the command port's REF line)
            HandArmed,          // id: the burst a window waits on; the host took HAND LISTEN
            HandSent,           // number: our Hand's channel, keyed
            HandDropped,        // number: our Hand's channel, not sent (the radio busy past its slot)
            HandHeard,          // number: channel, handPayload, snr, value: its lateness, ms
            HandShort,          // value: ms of the window's head missed
            HandEnd,            // the window is over, or an armed one gave up
        } type = Type::GroupOpen;

        int number = 0;
        int count = 0;
        bool on = false;
        bool incoming = false;
        std::string text;
        std::string peer;
        uint64_t id = 0;
        BurstState burstState = BurstState::Queued;
        SessionEnd sessionEnd = SessionEnd::Ended;
        std::vector<uint8_t> frame;
        std::optional<double> snr;  // dB, average power in 2500 Hz
        double value = 0.0;
        CommandEvent::Clean clean;
        CommandEvent::Disconnect disconnect;
        // HandRef: the reference. Heard: the burst the frame came in, from
        // a host since Data2G PR #146 (the KISS port's REF frame before it).
        std::optional<HandRef> handRef;
        uint64_t handPayload = 0;   // HandHeard: its 63 bits
    };

    // A broadcast written to the host but not sent is given up after this
    // much time with no session up.
    static constexpr uint64_t NOT_SENT_MS = 2 * 60 * 1000;
    // A command the host hasn't answered in this long is taken as answered.
    static constexpr uint64_t ANSWER_MS = 10 * 1000;
    // An ABORT the host hasn't followed with DISCONNECTED in this long ends
    // the session here anyway, so Leave now always leaves.
    static constexpr uint64_t ABORT_WAIT_MS = 15 * 1000;
    // SN is asked at most this often.
    static constexpr uint64_t SN_GAP_MS = 5 * 1000;
    // A burst that yields (a file's pieces, a repair round) waits this long
    // after our last burst went, of any kind, so another station can answer
    // it. Counted from the host's Sent, about half a second before PTT OFF;
    // the far end's reply is seen here only once its header decodes, about
    // a second after it keys.
    static constexpr uint64_t YIELD_GAP_MS = 4000;
    // KISS P, sent as the ports connect: a burst of ours that waited on
    // BUSY (a reply typed while another station's burst was on air) takes
    // each 1 s slot after it with probability (P + 1) / 256. data2g-host's
    // own 63 gives 25%, so a reply often missed the gap between another
    // station's file bursts; 191 gives 75%, 94% within two slots (Jeff,
    // 2026-10-25). Not 255: two stations waiting under one burst would
    // always key together.
    static constexpr uint8_t KISS_P = 191;

    explicit HostLink(Settings settings);

    //---------------------------------------------------------------------
    // The ports
    //---------------------------------------------------------------------

    // The command port has connected: the opening lines are queued.
    void commandUp(uint64_t nowMs);
    // It has closed: the group, any session and any burst with the host end.
    void commandDown(uint64_t nowMs);
    void commandBytes(const char* bytes, size_t length, uint64_t nowMs);
    // Lines to write to the command port, each ending in CR.
    std::string takeCommandOutput();

    void kissBytes(const uint8_t* bytes, size_t length, uint64_t nowMs);
    std::vector<uint8_t> takeKissOutput();

    std::vector<Event> takeEvents();

    // Moves the timers on (not-sent, unanswered commands), and hands the
    // host the next burst when it can take one.
    void tick(uint64_t nowMs);

    //---------------------------------------------------------------------
    // Settings
    //---------------------------------------------------------------------

    // The callsign is sent when the command port next connects: reconnect
    // it to open the group under a new one.
    void setCallsign(const std::string& callsign) { settings_.callsign = callsign; }
    void setAcceptSessions(bool accept, uint64_t nowMs);
    void setNarrow(bool narrow, uint64_t nowMs);
    // TIMEOUT s now, if the host takes DATA2G ON, and on every connect; 0
    // puts back the host's own, as first read.
    void setIdleTimeout(double seconds, uint64_t nowMs);
    const Settings& settings() const { return settings_; }

    // TXPOWER dB: the host's output volume, from its next audio block. It
    // stays set on the host after Ragtime goes. False with no DATA2G ON
    // (a host from before Data2G PR #122 has no TXPOWER) or out of range;
    // the host's answer is read back as a TxPower event, or a WRONG comes
    // as Refused.
    bool setTxPower(double dB, uint64_t nowMs);

    //---------------------------------------------------------------------
    // Broadcasts
    //---------------------------------------------------------------------

    // Queues a burst of frames for the D2G group, in mode (empty: the
    // group's mode as it stands). Returns its id; 0 for no frames. The
    // label says what it is, for the status bar: "seal", "QRV", or the
    // start of a message (snippet()). A burst that yields waits YIELD_GAP_MS
    // after our last burst was sent, and while a yield hold runs; bursts
    // that don't yield go ahead of it, and those that do keep their order.
    // It is Queued while it waits.
    uint64_t broadcast(const std::vector<std::vector<uint8_t>>& frames, const std::string& mode, uint64_t nowMs,
                       const std::string& label = std::string(), bool yields = false);
    // A Hand window after a burst (HAND LISTEN): delayS after it ends
    // (HAND_LISTEN_MIN_S to HAND_DELAY_MAX_S), tag (0-255) seeding the
    // Hands' CRC, over channels inside handChannels().
    struct HandWindow
    {
        double delayS = 3.0;
        int tag = 0;
        std::vector<int> channels;
    };
    // The same, with a window armed before the burst can go (handListen).
    // 0, with nothing queued, if the window can't be had: no host, bad
    // numbers, or another window pending.
    uint64_t broadcast(const std::vector<std::vector<uint8_t>>& frames, const std::string& mode, uint64_t nowMs,
                       const std::string& label, const HandWindow& window);
    // Drops a burst still queued; false once the host has it.
    bool cancel(uint64_t id);
    BurstState burstState(uint64_t id) const;
    // A burst of ours is with the host (Sending or Held), so a MODE report
    // may be its.
    bool burstWithHost() const;

    // Our bursts not yet sent, the one with the host (if any) first.
    struct Outgoing
    {
        uint64_t id = 0;
        std::string label;
        BurstState state = BurstState::Queued;
        size_t frames = 0;
    };
    std::vector<Outgoing> outgoing() const;

    // Bursts that yield wait until untilMs too (0: no hold): an addressed
    // message waits for its Ack. Replaces the hold before it.
    void holdYielding(uint64_t untilMs, uint64_t nowMs);
    uint64_t yieldHold() const { return yieldHoldMs_; }
    // When our last burst was sent (0: none yet).
    uint64_t lastSentMs() const { return lastSentMs_; }

    // Pause sending: queued bursts stay queued. One the host has goes on.
    void setPaused(bool paused, uint64_t nowMs);
    bool paused() const { return paused_; }

    //---------------------------------------------------------------------
    // Hands (Data2G PR #146, docs/DATA2G.md "Hands")
    //---------------------------------------------------------------------

    // The channels a Hand may use here: those inside our BW (BW500 with
    // the narrow setting, else BW2300), so 8 or 40. The host's --kiss-bw
    // doesn't narrow them.
    int handChannels() const;
    // The host has shown it does Hands: a Hand reference has come (a REF
    // line or frame, for any burst it decoded) since the link came up.
    bool handsSeen() const { return handsSeen_; }
    // HAND SEND: answer heard burst ref (a HandRef's or a Heard frame's
    // id) with a Hand in channel, delayS after the burst's end (Data2G's
    // advice: about 3 s), shifted by its offset; tag (0-255) and the
    // channel seed the Hand's CRC. False for bad numbers or no host; the
    // host's WRONG (a gone ref, a slot past, a Hand already waiting) comes
    // as Refused, and then HandSent or HandDropped.
    bool handSend(int ref, int tag, double delayS, int channel, uint64_t payload, uint64_t nowMs);
    // HAND LISTEN: net control's window over channels, delayS (at least
    // HAND_LISTEN_MIN_S) after the end of queued burst burstId, tag (0-255)
    // seeding the CRC. The burst carries the net's call in its first
    // frame, whose ACKMODE tag names it, and waits for the host's answer:
    // HandArmed on OK, else Refused and the burst goes without a window.
    // The decoded Hands come as HandHeard, then HandEnd. False if the
    // burst isn't queued, another window is pending, or the numbers are
    // bad. While a window is open the host sends no broadcasts.
    bool handListen(uint64_t burstId, double delayS, int tag, const std::vector<int>& channels, uint64_t nowMs);
    // Whether a window over these numbers could be armed now.
    bool canListen(double delayS, int tag, const std::vector<int>& channels) const;
    // HAND CANCEL: stops our waiting Hand and any window (a burst waiting
    // on its window's OK goes without one). Always OK at the host.
    bool handCancel(uint64_t nowMs);

    //---------------------------------------------------------------------
    // Sessions
    //---------------------------------------------------------------------

    // Call: false if the host isn't ready or a session is already up.
    bool call(const std::string& peer, uint64_t nowMs);
    // Leave: DISCONNECT, which waits for what is unacknowledged. Leave now:
    // ABORT, and the session ends here ABORT_WAIT_MS later even if the host
    // never says DISCONNECTED. While calling, either stops the call.
    bool leave(uint64_t nowMs);
    bool leaveNow(uint64_t nowMs);
    // The SWR trip: ABORT, which stops the host's transmitter at once and
    // drops the bursts it has queued (Data2G's Transmitter::abort), and
    // ends a session as Leave now does. The host answers DISCONNECTED even
    // with no session; that is let pass.
    bool abortTransmit(uint64_t nowMs);

    SessionState sessionState() const { return session_; }
    const std::string& sessionPeer() const { return peer_; }
    bool sessionIncoming() const { return incoming_; }
    // The host's latest BUFFER count.
    int buffer() const { return buffer_; }
    // Session bytes have arrived from the partner, so a burst of its has
    // decoded: SN is asked (now, or once SN_GAP_MS has passed).
    void sessionHeard(uint64_t nowMs);
    // The host took DATA2G ON, so CLEANTXBUFFER answers in host byte
    // offsets and a session's bytes can be cut.
    bool data2g() const { return data2g_; }
    bool canCut() const { return data2g_ && commandUp_ && session_ == SessionState::Connected; }
    // CLEANTXBUFFER upto: the host drops what it holds of our session
    // bytes before host offset upto that it hasn't sent. The answer comes
    // as a Clean event. False if no cut can be made.
    bool cleanTxBuffer(uint64_t upto, uint64_t nowMs);

    //---------------------------------------------------------------------
    // What the host has said
    //---------------------------------------------------------------------

    bool commandConnected() const { return commandUp_; }
    // The D2G group is open: broadcasts can go.
    bool groupOpen() const { return groupPort_ >= 0; }
    int groupPort() const { return groupPort_; }
    const std::string& groupMode() const { return groupMode_; }
    const std::vector<ModeInfo>& modes() const { return modes_; }
    bool ptt() const { return ptt_; }
    bool busy() const { return busy_; }
    // The mode of the host's last MODE report, ours or not. The host
    // reports a mode only when it changes, so a session in the mode of the
    // burst before it never hears one.
    const std::string& lastMode() const { return lastMode_; }
    // The last that wasn't an ACK burst's (isAckMode): the mode data went in.
    const std::string& lastDataMode() const { return lastDataMode_; }
    // The host's output volume (TXPOWER) and link idle timeout (TIMEOUT),
    // as last read; none from a host before Data2G PR #122.
    std::optional<double> txPower() const { return txPower_; }
    std::optional<double> idleTimeout() const { return idleTimeout_; }

private:
    struct Burst
    {
        uint64_t id = 0;
        std::vector<std::vector<uint8_t>> frames;
        std::string mode;
        std::string label;
        BurstState state = BurstState::Queued;
        std::vector<uint16_t> tags;     // written, not yet acknowledged
        uint64_t unsentMs = 0;          // time with the host, no session up
        bool yields = false;
        // A Hand window after it (handListen): asked before it is written.
        struct Listen
        {
            double delayS = 0.0;
            int tag = 0;
            std::vector<int> channels;
        };
        std::optional<Listen> listen;
    };

    void advance(uint64_t nowMs);
    void sendLine(const std::string& line, const std::string& answer, uint64_t nowMs);
    void onLine(const std::string& line, uint64_t nowMs);
    void answered(bool ok, uint64_t nowMs);
    void pump(uint64_t nowMs);
    bool mayGo(const Burst& burst, uint64_t nowMs) const;
    void writeBurst(Burst& burst);
    void assignTags(Burst& burst);
    void listenAnswered(bool ok);
    void setBurst(Burst& burst, BurstState state);
    void finishSession(SessionEnd end, const CommandEvent::Disconnect& why = CommandEvent::Disconnect());
    void sendIdleTimeout(uint64_t nowMs);
    void heldChanged();
    void askSn(uint64_t nowMs);
    Burst* current();
    void emit(Event event) { events_.push_back(std::move(event)); }

    Settings settings_;
    bool commandUp_ = false;
    std::string commandOut_;
    std::vector<uint8_t> kissOut_;
    LineSplitter lines_;
    KissDecoder kiss_;
    std::vector<Event> events_;

    // Answers we wait for, oldest first: "OK" (OK or WRONG), "BCAST PORT"
    // or "MODES" (MODE lines, CHANNELS and THRESHOLD lines from hosts since
    // Data2G PR #186, then OK).
    struct Awaiting
    {
        std::string command;
        std::string answer;
    };
    std::deque<Awaiting> awaiting_;
    uint64_t awaitingSinceMs_ = 0;
    bool modeAsked_ = false;            // BCAST MODE sent for the current burst, not yet answered
    bool listenAsked_ = false;          // HAND LISTEN sent for the current burst, not yet answered
    std::optional<HandRef> kissRef_;    // the burst the KISS port's next frames came in
    bool handsSeen_ = false;            // a REF since the command port came up
    std::vector<ModeInfo> modesComing_;
    std::vector<std::string> channelsComing_;  // the MODES answer's CHANNELS

    int groupPort_ = -1;
    std::string groupMode_;
    std::vector<ModeInfo> modes_;
    bool ptt_ = false;
    bool busy_ = false;
    bool data2g_ = false;
    std::string lastMode_;
    std::string lastDataMode_;
    std::optional<double> txPower_;
    std::optional<double> idleTimeout_;
    std::optional<double> hostIdleTimeout_;     // the host's own, read before we set ours

    std::deque<Burst> bursts_;          // the first may be with the host; the rest wait
    std::vector<std::pair<uint64_t, BurstState>> finished_;   // the latest outcomes, for burstState()
    uint64_t nextId_ = 1;
    uint16_t nextTag_ = 1;
    bool paused_ = false;
    uint64_t lastMs_ = 0;
    uint64_t lastSentMs_ = 0;           // our last burst Sent
    uint64_t yieldHoldMs_ = 0;          // yielding bursts wait until then

    SessionState session_ = SessionState::None;
    std::string peer_;
    bool incoming_ = false;
    int buffer_ = 0;
    uint64_t abortMs_ = 0;              // when ABORT went, while it waits for DISCONNECTED
    bool snWanted_ = false;             // the partner has likely sent since SN was last asked
    uint64_t snAskedMs_ = 0;
    std::optional<double> sessionSnr_;  // the last SN answer this session
};

//-------------------------------------------------------------------------
// What is going out, in the status bar's first field (Jeff, 2026-10-23:
// "indicate if we're sending a seal ... just a little snippet")
//-------------------------------------------------------------------------

// The start of text, in quotes, cut between words after at most chars
// characters (UTF-8 kept whole), with an ellipsis when cut.
std::string snippet(const std::string& text, size_t chars = 20);

// The field's line: "Channel clear" or "Channel busy" with nothing going
// out; else the first burst and how it stands ("Sending: seal",
// "Busy, next: seal", "Held: QRV" while the session holds the group,
// "Paused: “CQ de AG7EW…”"), and how many wait behind it.
std::string channelLine(const HostLink& link);
// Its tooltip: every burst going out, one a line.
std::string channelTip(const HostLink& link);

} // namespace link
} // namespace ragtime

#endif
