// The Broadcast tab's engine: messages on the D2G group, both ways
// (docs/PLAN.md, "Broadcast mode"; docs/PROTOCOL.md, "Broadcast").
//
// Typed text becomes Text records, numbered by the station's broadcast
// Numbering (kept across restarts by the app), packed into frames and
// handed to HostLink as one burst. Each message then shows Queued,
// Sending, Waiting for the session to go idle, Sent or Not sent from the
// host's ACKMODE reports, as HostLink settles its burst.
//
// - "@W1AW hello" addresses the message to W1AW and asks for an Ack. W1AW
//   answers on the group by itself, and the message shows Acknowledged.
// - A message too long for one frame goes as several Texts, split between
//   words, one frame each, all in one burst.
// - Ping (ping()): a Ping to one station, which answers with a Pong by
//   itself, in the Ping's mode, saying the S/N it heard us at; we read the
//   S/N we hear the Pong at from the host. The row shows both. A Ping to
//   us is answered the same way, once the host has said how well it heard
//   it. Never retried by itself: Ping again is the operator's.
// - Reactions (react()): an emoji or ham word from the palette on a
//   station's line. Ours waits up to REACTION_RIDE_MS to ride in our next
//   burst (a message, Acks, a check-in, a ping), then goes alone, at most
//   one lone burst every REACTION_LONE_GAP_MS. One per station per line, a
//   later one replacing it. A reaction from the station an @message went
//   to counts as its Ack.
// - A Hello rides along in the first frame after the group opens, after
//   the station's settings change, and at most every 10 minutes after
//   that, so the station list learns locators without a burst of its own.
// - History: with a BroadcastLog, the tab opens with what was said before,
//   a rule where Ragtime last closed, and the log carries on below.
//
// No sockets and no clock: the time is passed in, and HostLink is the
// socket-free one the app feeds.
#ifndef RAGTIME_BROADCAST_BROADCAST_CHAT_H
#define RAGTIME_BROADCAST_BROADCAST_CHAT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Reactions.h"
#include "ragtime/protocol/Records.h"
#include "ragtime/protocol/Stream.h"
#include "ragtime/text/HamText.h"

namespace ragtime
{
namespace broadcast
{

struct Message
{
    enum class State
    {
        Heard,          // someone else's
        Queued,         // ours, still with us (Pause sending, or waiting its turn)
        Sending,        // with the host, waiting for a clear channel
        Held,           // with the host, held by a session until it goes idle
        Sent,
        NotSent,        // the host dropped it, the link closed, or two minutes passed
        Cancelled,      // taken back before it went
        Acknowledged,   // addressed, and the station's Ack came back
    };

    // A row is a message, a broadcast file (a live card, or one line from
    // the log), or the rule drawn where Ragtime last closed.
    enum class Kind
    {
        Text,
        File,
        Rule,
        Ping,           // a ping of ours and its pong, or a station's ping to us and our pong
    };

    uint64_t id = 0;            // this list's own number
    Kind kind = Kind::Text;
    bool ours = false;
    std::string from;
    std::string to;             // addressed to one station, or empty
    bool ackWanted = false;
    std::string text;           // the exact UTF-8
    uint32_t seq = 0;
    uint32_t time = 0;          // the record's, Unix seconds
    uint64_t atMs = 0;          // when we made or heard it
    State state = State::Heard;
    uint64_t burst = 0;         // ours: HostLink's burst id
    std::string mode;           // the mode it went (or is going) out in, when known
    // A broadcast file's card rather than a message: BroadcastFiles' id
    // for it, with text the file's name. 0 for a file line from the log.
    uint64_t file = 0;
    bool past = false;          // from before Ragtime last closed, read from the BroadcastLog
    std::string outcome;        // a file line from the log: how it ended, when known
    // A file line from the log: where it was saved or sent from ("" if not
    // known), and whether it is an inline picture or a voice message, shown
    // under its line from that file.
    std::string path;
    bool picture = false;
    bool voice = false;

    // A ping row (Kind::Ping): ours to `to`, or `from`'s to us. State is
    // its burst's (ours: the Ping's; theirs: our Pong's), and Acknowledged
    // once the Pong came back. Not kept in the log.
    uint16_t pingNumber = 0;
    std::optional<double> theyHear; // ours: the S/N its Pong says it heard our Ping at
    std::optional<double> weHear;   // the S/N our host heard its Pong (ours) or its Ping (theirs) at
    uint64_t sentMs = 0;            // ours: when the Ping went
    uint64_t dueMs = 0;             // ours: when it counts as unanswered
    uint64_t answeredMs = 0;        // ours: when the Pong came
    bool unanswered = false;        // ours: no Pong by dueMs (a late one still counts)

    // A text line's reactions (docs/PROTOCOL.md, "Reactions"): the
    // stations', one each, and ours, on the air or waiting to go.
    std::vector<protocol::Reacted> reactions;
    uint8_t myReaction = 0;         // ours, sent
    uint8_t myWaiting = 0;          // ours, chosen and not yet sent
};

// "Queued", "Sending", "Waiting for the session to go idle", "Sent",
// "Not sent", "Cancelled", "Acknowledged"; "" for one heard.
const char* stateLabel(Message::State state);

// The ping row's words: "Ping W1AW, waiting for its pong", "Pong from W1AW
// in 12 s: it hears us at −8.0 dB, we hear it at −3.5 dB", "W1AW pinged
// us; we hear it at −3.5 dB. Pong sent".
std::string pingLine(const Message& m);

// A Pong waits this long for the host to say how well it heard the Ping
// (its BCAST HEARD may come just before or just after the frame), then goes
// without a figure.
constexpr uint64_t PONG_SNR_WAIT_MS = 3000;
// A host's S/N for a station this recent is the burst just heard.
constexpr uint64_t SNR_SAME_BURST_MS = 2000;
// At most one Pong to a station this often, however often it pings.
constexpr uint64_t PONG_GAP_MS = 15 * 1000;
// A Ping is unanswered after this long, at least: twice the listening
// window in its mode, after the Pong's wait for its figure.
constexpr uint64_t MIN_PONG_WAIT_MS = 20 * 1000;
uint64_t pongWaitMs(const link::ModeInfo& mode);

// A reaction waits this long to ride with our next burst, then goes alone;
// lone ones go at most this often, all that wait in one burst.
constexpr uint64_t REACTION_RIDE_MS = 30 * 1000;
constexpr uint64_t REACTION_LONE_GAP_MS = 60 * 1000;
// Reactions heard before their line (it may come later, repaired), kept
// for so many lines.
constexpr size_t MAX_EARLY_REACTIONS = 200;

// How often a Hello rides along with our messages.
constexpr uint64_t HELLO_EVERY_MS = 10 * 60 * 1000;
// The longest message taken at once, in bytes: about a screenful, a few
// frames.
constexpr size_t MAX_TYPED = 2000;
// Messages kept in the list; the oldest go first.
constexpr size_t MAX_MESSAGES = 2000;

class BroadcastLog;

class BroadcastChat
{
public:
    struct Settings
    {
        std::string callsign;
        std::string locator;
        bool acceptsSessions = true;
        std::string software;       // "Ragtime 0.1.0"
        // What our Hello says we take (protocol::Takes): a client with fewer
        // features than Ragtime says so, and a peer offers it only those.
        uint8_t takes = protocol::TAKES_ALL_V0;
    };

    explicit BroadcastChat(link::HostLink& link);

    // A change makes the next message carry a Hello. A new callsign starts
    // its own numbering at newestItem.
    void setSettings(const Settings& settings, uint32_t newestItem);
    const Settings& settings() const { return settings_; }

    // Where the chat is kept: what it holds already goes in front of
    // what this run says, which is added as it happens. None: the tab
    // starts empty.
    void setLog(BroadcastLog* log, uint64_t nowMs);
    // Ragtime is closing: the log's rule goes here.
    void closeLog(uint64_t nowMs);

    // The newest broadcast item's number, for the app to keep.
    uint32_t newestItem() const { return numbering_.newestItem(); }

    // A record of ours for the group, numbered by the broadcast numbering:
    // a FileAnnounce takes the next item number, a FilePiece its file's
    // (item), and anything else the newest item's.
    protocol::Record record(protocol::Body body, uint32_t time, uint32_t item = 0);

    // Told of every burst of our items the chat hands HostLink, first time
    // or again, so they can be sealed (broadcast::Seals).
    using ItemsFn = std::function<void(const std::vector<protocol::Record>& items, uint64_t burst,
                                       const std::string& mode, uint64_t nowMs)>;
    void onItems(ItemsFn fn) { onItems_ = std::move(fn); }

    // A broadcast file's card among the messages, ours or heard; file is
    // BroadcastFiles' id for it. Returns the card's message id.
    uint64_t addFile(uint64_t file, bool ours, const std::string& from, const std::string& name, uint64_t nowMs);
    // Something a file card shows has changed.
    void fileChanged() { version_++; }
    // A file card's file has ended, as outcome ("Saved", "Sent"), for the
    // log to show next time: with the file's path ("" if not kept) and, for
    // an inline picture or a voice message, which, so the log shows it
    // under its line from that file.
    void fileEnded(uint64_t file, const std::string& outcome, const std::string& path = std::string(),
                   bool picture = false, bool voice = false);

    // "@W1AW" at the start of typed text: the station, and the text after
    // it. False when it isn't addressed.
    static bool addressee(const std::string& typed, std::string& call, std::string& rest);
    // The ham phrases send() will code as one symbol each, as spans of
    // typed (an "@W1AW" in front goes in the record's To, not its text),
    // for the entry line to show as they are typed.
    static std::vector<text::HamText::PhraseSpan> phrases(const std::string& typed);

    // Sends typed text in mode (empty: the group's mode as it stands).
    // Returns the messages it became; none for blank text, text over
    // MAX_TYPED, or no callsign set.
    std::vector<uint64_t> send(const std::string& typed, const std::string& mode, uint64_t nowMs);

    // A Hello of its own: "check in".
    bool checkIn(const std::string& mode, uint64_t nowMs);

    // Pings call in mode (empty: the group's mode as it stands). Returns
    // the ping's row; 0 with no callsign set, or call not a callsign or
    // our own.
    uint64_t ping(const std::string& call, const std::string& mode, uint64_t nowMs);
    // A ping of ours to call is still going or waiting for its Pong.
    bool pinging(const std::string& call) const;
    // Moves the pings on: Pongs that waited long enough for their figure
    // go, and Pings with no Pong by their time show as unanswered.
    void tick(uint64_t nowMs);

    // Our session state on the group (docs/PLAN.md, "Busy stations"): a
    // QRL naming the partner on entering a session, a QRV on leaving it.
    // A QRV goes only after a QRL; a QRL still queued when the session ends
    // is taken back instead, and then no QRV is needed. False when nothing
    // went.
    bool announce(protocol::Status::State state, const std::string& partner, const std::string& mode,
                  uint64_t nowMs);

    // Takes back a message HostLink hasn't handed to the host yet (with the
    // rest of its burst). False once the host has it.
    bool cancel(uint64_t id);
    // Re-send: one of our messages goes again, once it has settled (Sent,
    // Not sent, Cancelled or Acknowledged). It goes as the same record, so
    // a station that heard it the first time doesn't list it twice, and an
    // addressed one is acknowledged again. False while it is still going.
    // One from the log goes again too, if it was sent under the callsign
    // set now.
    bool resend(uint64_t id, const std::string& mode, uint64_t nowMs);
    bool canResend(const Message& message) const;

    // Reactions. A text line of another station's, with a number, takes
    // one: of ours and the rule, pings and files, none.
    bool canReact(const Message& message) const;
    // Our reaction to the line: it waits to ride with our next burst
    // (REACTION_RIDE_MS). A code not in the palette, or the one we gave
    // already, does nothing (false). Choosing the one on the air while
    // another waits takes the waiting one back.
    bool react(uint64_t id, uint8_t code, uint64_t nowMs);
    // Takes back our reaction still waiting to go; false once it has gone,
    // as what is on the air has been said.
    bool unreact(uint64_t id);
    // Ours waiting to go, not yet given to the host: all of them, and the
    // line's, which unreact() can take back.
    size_t reactionsWaiting() const { return reactionsDue_.size(); }
    bool reactionWaiting(uint64_t id) const { return reactionsDue_.count(id) != 0; }
    // A station's reaction to one of our lines, for the status bar.
    struct ReactionNote
    {
        std::string from;
        uint8_t code = 0;
        uint64_t message = 0;
    };
    std::vector<ReactionNote> takeReactionNotes();

    // Every HostLink event goes here: bursts settling, frames heard, the
    // group opening.
    // Returns the records heard in a frame (for the station list, and the
    // broadcast files to come), every one of them, ours to answer or not.
    std::vector<protocol::Record> onEvent(const link::HostLink::Event& event, uint64_t nowMs);
    // A record rebuilt from parity (Seals::takeRebuilt), taken as if heard.
    void onRebuilt(const protocol::Record& record, uint64_t nowMs);

    const std::deque<Message>& messages() const { return messages_; }
    const Message* find(uint64_t id) const;
    // Goes up with every change, so a view knows when to redraw.
    uint64_t version() const { return version_; }
    // Frames heard that weren't Ragtime's, or broke off.
    size_t foreignFrames() const { return foreign_; }

private:
    using ItemKey = std::tuple<std::string, uint32_t, uint32_t>;  // call, seq, time

    void onRecord(const protocol::Record& record, uint64_t nowMs);
    Message* findMine(uint64_t id);
    protocol::Record helloRecord(uint32_t time);
    bool helloDue(uint64_t nowMs) const;
    // Splits text into pieces that each fit a frame beside `room` other
    // bytes, between words where it can.
    std::vector<std::string> split(const std::string& text, const std::string& to, size_t firstRoom);
    void trim();
    // Where a line heard goes: in its place among its sender's lines (one
    // repaired, or heard late, slots in above the sender's later ones), by
    // the record's time among the others' there; the end when it is the
    // sender's newest.
    std::deque<Message>::iterator placeFor(const Message& m);
    void flushAcks(const std::string& mode, uint64_t nowMs);
    // Hands frames to HostLink as one burst, with our reactions waiting
    // packed in after them.
    uint64_t broadcast(std::vector<std::vector<uint8_t>> frames, const std::string& mode, uint64_t nowMs,
                       const std::string& label);
    // A burst of ours settled or was taken back: its reactions are on the
    // air, or wait again.
    void reactionsSettled(uint64_t burst, bool sent, uint64_t nowMs);
    void onReaction(const std::string& from, const protocol::Reaction& reaction, uint64_t nowMs);
    Message* findMessage(uint64_t id);
    void onPing(const protocol::Record& record, const protocol::Ping& ping, uint64_t nowMs);
    void onPong(const protocol::Record& record, const protocol::Pong& pong, uint64_t nowMs);
    void heardSnr(const std::string& call, double dB, uint64_t nowMs);
    // The host's S/N for call's burst just heard, if it has said.
    std::optional<double> recentSnr(const std::string& call, uint64_t nowMs) const;
    struct PongDue
    {
        std::string to;
        uint16_t number = 0;
        std::string mode;           // the Ping's
        uint64_t untilMs = 0;       // goes without a figure then
        uint64_t row = 0;
    };
    void sendPong(const PongDue& due, std::optional<double> snr, uint64_t nowMs);
    void logState(const Message& m);
    // A file's bursts hold off while an addressed message waits for its
    // Ack, and a ping for its Pong.
    void holdForAck(const Message& m, uint64_t nowMs);
    void hold(uint64_t id, uint64_t untilMs, uint64_t nowMs);
    void ackHeard(uint64_t id, uint64_t nowMs);

    link::HostLink& link_;
    ItemsFn onItems_;
    BroadcastLog* log_ = nullptr;
    Settings settings_;
    protocol::Numbering numbering_;
    std::deque<Message> messages_;
    std::map<uint64_t, protocol::Record> records_;  // ours, by message id, for resending
    std::set<ItemKey> seen_;
    std::set<uint64_t> ackBursts_;
    std::vector<protocol::Record> acksDue_;
    std::map<uint64_t, uint64_t> ackHolds_;         // message id: until when its Ack (or Pong) holds files off

    std::set<uint64_t> reactionsDue_;               // message ids whose myWaiting goes with our next burst
    uint64_t reactionsSinceMs_ = 0;                 // when the oldest of them was chosen
    uint64_t loneReactionsMs_ = 0;                  // when reactions last went alone
    std::map<uint64_t, std::vector<std::pair<uint64_t, uint8_t>>> reactionBursts_;  // burst: (message, code)
    std::map<uint64_t, uint8_t> reactionsOut_;      // message id: its code with the host
    std::deque<std::pair<std::pair<std::string, uint32_t>, protocol::Reacted>> early_;  // (line's call, seq): reaction
    std::vector<ReactionNote> reactionNotes_;

    uint16_t pingNumber_ = 0;
    std::vector<PongDue> pongsDue_;                 // waiting for the host's figure
    std::map<std::string, uint64_t> pongedMs_;      // station: when we last answered its ping
    std::map<std::string, std::pair<double, uint64_t>> snrs_;   // station: its last burst's S/N, and when
    uint64_t nextId_ = 1;
    uint64_t version_ = 0;
    size_t foreign_ = 0;

    uint64_t qrlBurst_ = 0;         // our latest QRL's burst
    bool qrlOut_ = false;           // a QRL has gone (or is going) with no QRV since

    bool helloSent_ = false;        // since the group opened or the settings changed
    uint64_t helloMs_ = 0;
};

} // namespace broadcast
} // namespace ragtime

#endif
