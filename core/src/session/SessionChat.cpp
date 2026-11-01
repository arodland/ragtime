#include "ragtime/session/SessionChat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ragtime/broadcast/Stations.h"
#include "ragtime/session/SessionLog.h"

namespace ragtime
{
namespace session
{

namespace
{

using LinkEvent = link::HostLink::Event;
using State = Message::State;

const char* const DOT = " \xc2\xb7 ";    // " · "

uint32_t seconds(uint64_t nowMs)
{
    return (uint32_t)(nowMs / 1000);
}

bool blank(const std::string& text)
{
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

std::string twoDigits(uint64_t n)
{
    return (n < 10 ? "0" : "") + std::to_string(n);
}

// An idle timeout in words: "5 minutes", "24 hours".
std::string idleLength(uint64_t ms)
{
    auto count = [](uint64_t n, const char* unit) { return std::to_string(n) + " " + unit + (n == 1 ? "" : "s"); };
    if (ms >= 3600000 && ms % 3600000 == 0) return count(ms / 3600000, "hour");
    if (ms >= 60000 && ms % 60000 == 0) return count(ms / 60000, "minute");
    if (ms < 60000) return count(ms / 1000, "second");
    return sessionLength(ms);
}

// Quiet for about a host's idle timeout: from a little short of it to the
// hosts' goodbyes after it.
bool nearIdle(uint64_t silentMs, uint64_t idleMs, uint64_t earlyMs, uint64_t lateMs)
{
    return silentMs + earlyMs >= idleMs && silentMs < idleMs + lateMs;
}

} // namespace

//-------------------------------------------------------------------------
// The stream's account
//-------------------------------------------------------------------------

void StreamAccount::wrote(size_t bytes, uint64_t nowMs)
{
    if (bytes == 0) return;
    written_ += bytes;
    writes_.push_back({written_, nowMs});
}

void StreamAccount::buffer(int count, uint64_t nowMs)
{
    if (count < 0) count = 0;
    // A rise is reading: acknowledgements only lower the count.
    if (count > count_) read_ = std::min(written_, read_ + (uint64_t)(count - count_));
    count_ = count;
    if (count == 0)
    {
        acked_ = std::max(acked_, read_);
        // A 0 this long after a write is from after the host read it.
        for (const Write& w : writes_)
        {
            if (w.atMs + SETTLE_MS <= nowMs) acked_ = std::max(acked_, w.end);
        }
        read_ = std::max(read_, acked_);
    }
    else if (read_ >= (uint64_t)count)
    {
        acked_ = std::max(acked_, read_ - (uint64_t)count);
    }
    while (!writes_.empty() && writes_.front().end <= acked_) writes_.pop_front();
}

void StreamAccount::dropped(uint64_t at, uint64_t bytes, uint64_t unread, uint64_t nowMs)
{
    // Never on air, so never acknowledged: at is past acked_.
    at = std::max(at, acked_);
    bytes = std::min(bytes, written_ - std::min(written_, at));
    auto shift = [&](uint64_t x) { return x <= at ? x : at + (x > at + bytes ? x - at - bytes : 0); };
    written_ -= bytes;
    read_ = std::min(shift(read_), written_ - std::min(unread, written_));
    count_ = (int)std::max<int64_t>(0, (int64_t)count_ - (int64_t)bytes);
    // Bytes the host has yet to read are settled by counts, not by a
    // BUFFER 0 that comes before it reads them: their writes start again.
    uint64_t readTo = written_ - std::min(unread, written_);
    for (Write& w : writes_)
    {
        w.end = shift(w.end);
        if (w.end > readTo) w.atMs = std::max(w.atMs, nowMs);
    }
}

void StreamAccount::reset()
{
    *this = StreamAccount();
}

//-------------------------------------------------------------------------

const char* stateLabel(Message::State state)
{
    switch (state)
    {
        case State::Heard:
            return "";
        case State::Queued:
            return "Queued";
        case State::Sent:
            return "Sent";
        case State::Delivered:
            return "Delivered";
        case State::NotDelivered:
            return "Not delivered";
    }
    return "";
}

std::string sessionLength(uint64_t ms)
{
    uint64_t min = ms / 60000;
    if (min == 0) return "less than a minute";
    if (min < 60) return std::to_string(min) + " min";
    std::string out = std::to_string(min / 60) + " h";
    if (min % 60 != 0) out += " " + std::to_string(min % 60) + " min";
    return out;
}

std::string clockLength(uint64_t ms)
{
    uint64_t s = ms / 1000;
    if (s < 3600) return std::to_string(s / 60) + ":" + twoDigits(s % 60);
    return std::to_string(s / 3600) + ":" + twoDigits(s / 60 % 60) + ":" + twoDigits(s % 60);
}

std::string clockTime(uint64_t unixMs, long utcOffsetS)
{
    long long day = 86400;
    long long t = (long long)(unixMs / 1000) + utcOffsetS;
    long long inDay = ((t % day) + day) % day;
    return twoDigits((uint64_t)(inDay / 3600)) + ":" + twoDigits((uint64_t)(inDay / 60 % 60));
}

SessionChat::SessionChat(link::HostLink& link) : link_(link)
{
}

void SessionChat::setSettings(const Settings& settings)
{
    settings_ = settings;
    settings_.callsign = protocol::normalCallsign(settings.callsign);
    changed();
}

//-------------------------------------------------------------------------
// The operator
//-------------------------------------------------------------------------

bool SessionChat::call(const std::string& peer, uint64_t nowMs)
{
    std::string them = protocol::normalCallsign(peer);
    if (active() || !dataUp_ || them.empty() || them == settings_.callsign) return false;
    if (!link_.call(them, nowMs)) return false;
    endAfterglow();
    phase_ = Phase::Calling;
    peer_ = them;
    incoming_ = false;
    tries_ = 0;
    sinceMs_ = nowMs;
    connectedMs_ = 0;
    cancelling_ = gaveUp_ = dataLost_ = false;
    // A new session starts in the tab, below what was said with them before.
    showHistory(them);
    SessionChat::Event calling;
    calling.type = SessionChat::Event::Type::Calling;
    calling.peer = them;
    emit(calling);
    changed();
    return true;
}

bool SessionChat::leave(uint64_t nowMs)
{
    if (phase_ == Phase::Calling) return leaveNow(nowMs);
    if (phase_ != Phase::Connected) return false;
    sinceMs_ = nowMs;
    if (dataUp_)
    {
        // File bytes the host still holds needn't hold up the farewell: a
        // cut drops them first.
        bool cut = cutting_ != Cutting::None
                   || (canCut() && traffic_ != nullptr && traffic_->busy(peer_) && account_.waiting() > 0);
        phase_ = Phase::Leaving;
        if (cut)
        {
            leaveAfterCut_ = true;
            startCut(nowMs);
        }
        else
        {
            writeFarewell(nowMs);
        }
    }
    else
    {
        phase_ = Phase::Leaving;
        link_.leave(nowMs);
    }
    changed();
    return true;
}

// The last word of whatever rides in the session, then the farewell: our
// QRV, so the far end knows we left on purpose.
void SessionChat::writeFarewell(uint64_t nowMs)
{
    std::vector<uint8_t> out;
    if (traffic_ != nullptr) traffic_->leaving(peer_, numbering_, nowMs, out);
    protocol::Status qrv;
    qrv.state = protocol::Status::State::Qrv;
    protocol::encodeRecord(numbering_.record(qrv, seconds(nowMs)), out);
    write(out, nowMs);
    farewellPending_ = true;
    farewellEnd_ = account_.written() + out_.size();
    farewellMs_ = nowMs;
}

bool SessionChat::leaveNow(uint64_t nowMs)
{
    if (!active()) return false;
    if (phase_ == Phase::Calling) cancelling_ = true;
    farewellPending_ = false;
    phase_ = Phase::Leaving;
    sinceMs_ = nowMs;
    link_.leaveNow(nowMs);
    changed();
    return true;
}

uint64_t SessionChat::send(const std::string& typed, uint64_t nowMs)
{
    if (phase_ != Phase::Connected || settings_.callsign.empty() || blank(typed) || typed.size() > MAX_TYPED)
        return 0;
    protocol::Text t;
    t.text = typed;
    protocol::Record record = numbering_.record(t, seconds(nowMs));
    std::vector<uint8_t> bytes = protocol::encodeRecord(record);
    if (bytes.empty()) return 0;

    Message m;
    m.id = nextId_++;
    m.ours = true;
    m.from = settings_.callsign;
    m.text = typed;
    m.seq = record.header.seq;
    m.time = record.header.time;
    m.atMs = nowMs;
    m.state = State::Queued;
    queued_.push_back({m.id, std::move(bytes)});
    if (log_ != nullptr) log_->added(peer_, m);
    messages_.push_back(std::move(m));
    uint64_t id = messages_.back().id;
    trim();
    // More than a little of a file waiting at the host: a cut lets the
    // message in ahead of it, and the file carries on behind.
    if (!paused_ && cutting_ == Cutting::None && canCut() && traffic_ != nullptr && traffic_->busy(peer_)
        && account_.waiting() > CUT_FOR_CHAT_BYTES)
        startCut(nowMs);
    if (!paused_ && cutting_ == Cutting::None) writeQueued(nowMs);
    changed();
    return id;
}

uint64_t SessionChat::resend(uint64_t id, uint64_t nowMs)
{
    for (const Message& m : messages_)
    {
        if (m.id != id) continue;
        if (!canResend(m)) return 0;
        std::string text = m.text;   // send() may trim the list under m
        return send(text, nowMs);
    }
    return 0;
}

void SessionChat::setPaused(bool paused, uint64_t nowMs)
{
    paused_ = paused;
    if (!paused_ && phase_ == Phase::Connected && cutting_ == Cutting::None) writeQueued(nowMs);
    changed();
}

//-------------------------------------------------------------------------
// The ports
//-------------------------------------------------------------------------

void SessionChat::write(const std::vector<uint8_t>& bytes, uint64_t nowMs, uint64_t message)
{
    (void)nowMs;
    if (bytes.empty()) return;
    if (message != 0) outMessages_.push_back({out_.size(), message});
    out_.insert(out_.end(), bytes.begin(), bytes.end());
}

void SessionChat::writeQueued(uint64_t nowMs)
{
    if (!dataUp_) return;
    for (auto& q : queued_)
    {
        write(q.second, nowMs, q.first);
        for (Message& m : messages_)
        {
            if (m.id != q.first) continue;
            m.state = State::Sent;
            m.streamEnd = account_.written() + out_.size();
            logState(m);
        }
    }
    if (!queued_.empty()) changed();
    queued_.clear();
    writeReactions(nowMs);
}

bool SessionChat::canReact(const Message& m) const
{
    return phase_ == Phase::Connected && m.kind == Message::Kind::Text && !m.ours && !m.past && m.seq != 0
           && !settings_.callsign.empty() && reader_.greeted()
           && (reader_.hello().takes & protocol::TAKES_REACTIONS) != 0;
}

bool SessionChat::react(uint64_t id, uint8_t code, uint64_t nowMs)
{
    for (Message& m : messages_)
    {
        if (m.id != id) continue;
        if (!canReact(m) || protocol::reactionEntry(code) == nullptr) return false;
        if (code == (m.myWaiting != 0 ? m.myWaiting : m.myReaction)) return false;
        m.myWaiting = code;
        if (std::find(reactionsHeld_.begin(), reactionsHeld_.end(), id) == reactionsHeld_.end())
            reactionsHeld_.push_back(id);
        if (!paused_ && cutting_ == Cutting::None) writeReactions(nowMs);
        changed();
        return true;
    }
    return false;
}

void SessionChat::writeReactions(uint64_t nowMs)
{
    if (!dataUp_ || phase_ != Phase::Connected) return;
    for (uint64_t id : reactionsHeld_)
    {
        for (Message& m : messages_)
        {
            if (m.id != id || m.myWaiting == 0) continue;
            protocol::Reaction r;
            r.to = m.from;
            r.item = m.seq;
            r.code = m.myWaiting;
            write(protocol::encodeRecord(numbering_.record(r, seconds(nowMs))), nowMs);
            m.myReaction = m.myWaiting;
            m.myWaiting = 0;
            if (log_ != nullptr) log_->weReacted(peer_, m, m.myReaction);
            changed();
        }
    }
    reactionsHeld_.clear();
}

void SessionChat::onReaction(const protocol::Record& record, const protocol::Reaction& reaction)
{
    for (Message& m : messages_)
    {
        if (m.past || m.kind != Message::Kind::Text || m.from != reaction.to || m.seq != reaction.item) continue;
        if (!protocol::setReaction(m.reactions, record.header.callsign, reaction.code)) return;
        if (log_ != nullptr) log_->reacted(peer_, m, record.header.callsign, reaction.code);
        if (m.ours)
        {
            SessionChat::Event e;
            e.type = SessionChat::Event::Type::Reacted;
            e.peer = peer_;
            e.id = m.id;
            e.code = reaction.code;
            emit(e);
        }
        changed();
        return;
    }
}

std::vector<uint8_t> SessionChat::takeDataOutput(uint64_t nowMs)
{
    // Nothing goes while a cut is made: what follows it depends on where
    // it falls.
    stepCut(nowMs);
    if (cutting_ != Cutting::None) return {};
    if (phase_ == Phase::Connected && dataUp_ && traffic_ != nullptr)
    {
        if (canCut() && traffic_->wantsCut(peer_))
        {
            startCut(nowMs);
            if (cutting_ != Cutting::None) return {};
        }
        std::vector<uint8_t> more;
        traffic_->write(peer_, numbering_, account_.written() + out_.size(), nowMs, more);
        write(more, nowMs);
    }
    std::vector<uint8_t> out;
    out.swap(out_);
    std::vector<std::pair<size_t, uint64_t>> messages;
    messages.swap(outMessages_);
    if (!dataUp_ || out.empty()) return {};

    // Into the log, record by record, each message's where it now ends.
    size_t at = 0;
    for (size_t i = 0; i <= messages.size(); i++)
    {
        size_t next = i < messages.size() ? messages[i].first : out.size();
        if (next > at) sent_.wrote(out.data() + at, next - at);
        if (i == messages.size()) break;
        size_t to = i + 1 < messages.size() ? messages[i + 1].first : out.size();
        uint64_t start = sent_.end();
        sent_.wrote(out.data() + next, to - next, messages[i].second);
        for (const StreamLog::Span& span : sent_.spans())
        {
            if (span.start != start) continue;
            for (Message& m : messages_)
            {
                if (m.id == messages[i].second) m.streamEnd = span.end;
            }
        }
        at = to;
    }
    account_.wrote(out.size(), nowMs);
    hostWritten_ += out.size();
    lastWriteMs_ = nowMs;
    return out;
}

//-------------------------------------------------------------------------
// Cuts (docs/PROTOCOL.md, "Cuts")
//-------------------------------------------------------------------------

bool SessionChat::canCut() const
{
    bool up = phase_ == Phase::Connected || (phase_ == Phase::Leaving && leaveAfterCut_);
    return up && dataUp_ && connectedMs_ != 0 && link_.canCut();
}

void SessionChat::startCut(uint64_t nowMs)
{
    if (cutting_ != Cutting::None) return;
    cutting_ = Cutting::Quiet;
    cutRanges_.clear();
    cutBase_ = hostDropped_;
    limbo_ = false;
    cutTries_ = 0;
    changed();
    stepCut(nowMs);
}

void SessionChat::stepCut(uint64_t nowMs)
{
    switch (cutting_)
    {
        case Cutting::None:
        case Cutting::Settling:
            // The limbo settles with the far end's next acknowledgement, or
            // the session ends.
            return;
        case Cutting::Quiet:
            // The host reads its data port as bytes come: a moment after
            // our last write it has read all of it, so all of it is cut.
            if (!canCut())
            {
                mend(nowMs);
                return;
            }
            if (hostWritten_ != 0 && nowMs < lastWriteMs_ + CUT_QUIET_MS) return;
            if (!link_.cleanTxBuffer(hostWritten_, nowMs))
            {
                mend(nowMs);
                return;
            }
            cutting_ = Cutting::Asked;
            cutAskedMs_ = nowMs;
            cutTries_++;
            return;
        case Cutting::Asked:
            if (nowMs >= cutAskedMs_ + CUT_ANSWER_MS) mend(nowMs);
            return;
    }
}

void SessionChat::dropCut(uint64_t from, uint64_t to, uint64_t unread, uint64_t nowMs)
{
    if (to <= from) return;
    // Where it sits in our stream: less what was cut before it.
    uint64_t at = from - cutBase_;
    for (const auto& r : cutRanges_)
    {
        if (r.second <= from) at -= r.second - r.first;
    }
    cutRanges_.push_back({from, to});
    account_.dropped(at, to - from, unread, nowMs);
    hostDropped_ += to - from;
    cutBytes_ += to - from;
}

void SessionChat::onClean(const link::CommandEvent::Clean& clean, uint64_t nowMs)
{
    using Result = link::CommandEvent::Clean::Result;
    if (clean.result == Result::Settled)
    {
        // Ended first (UNKNOWN): the session is going, and the cut with it.
        if (clean.unknown || clean.offsets.size() != 3 || cutting_ == Cutting::None) return;
        // Delivered before split, dropped from it.
        dropCut(clean.offsets[1], clean.offsets[2], 0, nowMs);
        limbo_ = false;
        if (cutting_ == Cutting::Settling) mend(nowMs);
        return;
    }
    if (cutting_ != Cutting::Asked) return;
    if (clean.result == Result::Ok && clean.offsets.size() == 4)
    {
        // Past drop to, the host had yet to read what we wrote.
        uint64_t unread = hostWritten_ > clean.offsets[1] ? hostWritten_ - clean.offsets[1] : 0;
        dropCut(clean.offsets[0], clean.offsets[1], unread, nowMs);
        if (clean.offsets[3] > clean.offsets[2]) limbo_ = true;
        // Bytes still on their way to the host were kept, and follow the
        // cut: cut again.
        if (clean.offsets[1] < hostWritten_ && cutTries_ < 3 && link_.cleanTxBuffer(hostWritten_, nowMs))
        {
            cutAskedMs_ = nowMs;
            cutTries_++;
            return;
        }
    }
    if (limbo_)
    {
        cutting_ = Cutting::Settling;
        return;
    }
    mend(nowMs);
}

// The cut is made: what the far end has is our stream up to it. The record
// it fell in is finished, then come the messages waiting, the records cut
// that still matter (not file bytes: those go again from the file), and
// what was written meanwhile.
void SessionChat::mend(uint64_t nowMs)
{
    cutting_ = Cutting::None;
    std::vector<uint8_t> held;
    held.swap(out_);
    std::vector<std::pair<size_t, uint64_t>> heldMessages;
    heldMessages.swap(outMessages_);
    StreamLog::Cut cut;
    if (!cutRanges_.empty())
    {
        std::sort(cutRanges_.begin(), cutRanges_.end());
        uint64_t from = cutRanges_.front().first, to = from;
        bool joined = true;
        for (const auto& r : cutRanges_)
        {
            if (r.first > to) joined = false;
            to = std::max(to, r.second);
        }
        // Only a cut that takes the end of the stream can be mended.
        uint64_t at = from - cutBase_;
        if (!joined || to != hostWritten_ || account_.written() != at || !sent_.cut(at, cut))
        {
            cutRanges_.clear();
            leaveAfterCut_ = false;
            leaveNow(nowMs);
            return;
        }
        cuts_++;
        write(cut.tail, nowMs);
        if (traffic_ != nullptr) traffic_->cut(peer_, at);
    }
    cutRanges_.clear();
    if (!paused_) writeQueued(nowMs);
    for (size_t i = 0; i < cut.lost.size(); i++)
    {
        if (cut.lost[i].type == (uint8_t)protocol::RecordType::FileData) continue;
        write(cut.lostBytes[i], nowMs, cut.lost[i].message);
    }
    for (auto& m : heldMessages) outMessages_.push_back({m.first + out_.size(), m.second});
    out_.insert(out_.end(), held.begin(), held.end());
    if (leaveAfterCut_)
    {
        leaveAfterCut_ = false;
        writeFarewell(nowMs);
    }
    changed();
}

void SessionChat::endCut()
{
    cutting_ = Cutting::None;
    cutRanges_.clear();
    limbo_ = false;
    leaveAfterCut_ = false;
    sent_.reset();
    outMessages_.clear();
    hostWritten_ = hostDropped_ = cutBase_ = 0;
    lastWriteMs_ = 0;
}

std::vector<protocol::Record> SessionChat::dataBytes(const uint8_t* bytes, size_t length, uint64_t nowMs)
{
    if (afterglowUntilMs_ != 0 && nowMs >= afterglowUntilMs_) endAfterglow();
    std::vector<protocol::Record> records;
    bool wasForeign = reader_.foreign();
    reader_.feed(bytes, length, records);
    if (reader_.foreign() != wasForeign) changed();

    if (afterglowUntilMs_ != 0)
    {
        // The last of the session that just ended: its farewell, its last words.
        for (const protocol::Record& r : records)
        {
            if (r.as<protocol::Text>() != nullptr || r.as<protocol::Status>() != nullptr) onRecord(r, nowMs);
        }
        if (farewellHeard_ && (ending_ == Ending::LinkLost || ending_ == Ending::ClosedThere))
        {
            ending_ = Ending::LeftThere;
            changed();
        }
        return records;
    }

    bool up = phase_ == Phase::Connected || phase_ == Phase::Leaving;
    if (!up || connectedMs_ == 0)
    {
        // The far end's Hello can come before CONNECTED: it waits for it.
        early_.insert(early_.end(), records.begin(), records.end());
        return {};
    }
    if (length > 0) heard(nowMs);
    for (const protocol::Record& r : records) onRecord(r, nowMs);
    uint32_t offer = 0, offset = 0;
    const uint8_t* data = nullptr;
    size_t arrived = 0;
    if (traffic_ != nullptr && reader_.partialFileData(offer, offset, data, arrived))
        traffic_->arriving(peer_, offer, offset, data, arrived);
    return records;
}

void SessionChat::onRecord(const protocol::Record& record, uint64_t nowMs)
{
    if (const protocol::Hello* hello = record.as<protocol::Hello>())
    {
        if (!greetedTold_ && traffic_ != nullptr) traffic_->greeted(peer_, *hello);
        greetedTold_ = true;
        changed();
    }
    else if (const protocol::Text* text = record.as<protocol::Text>())
    {
        Message m;
        m.id = nextId_++;
        m.from = record.header.callsign;
        m.text = text->text;
        m.seq = record.header.seq;
        m.time = record.header.time;
        m.atMs = nowMs;
        if (log_ != nullptr) log_->added(peer_, m);
        messages_.push_back(m);
        trim();
        SessionChat::Event e;
        e.type = SessionChat::Event::Type::Heard;
        e.peer = peer_;
        e.id = m.id;
        emit(e);
        changed();
    }
    else if (const protocol::Status* status = record.as<protocol::Status>())
    {
        // The far end's farewell: it is leaving on purpose.
        if (status->state != protocol::Status::State::Qrl)
        {
            farewellHeard_ = true;
            changed();
        }
    }
    else if (const protocol::Reaction* reaction = record.as<protocol::Reaction>())
    {
        onReaction(record, *reaction);
    }
    else if (record.as<protocol::Ack>() == nullptr && traffic_ != nullptr)
    {
        traffic_->onRecord(peer_, record, nowMs);
    }
}

void SessionChat::heard(uint64_t nowMs)
{
    lastHeardMs_ = std::max(lastHeardMs_, nowMs);
    waitingSinceMs_ = std::max(waitingSinceMs_, nowMs);
}

void SessionChat::settle()
{
    uint64_t acked = account_.acknowledged();
    for (Message& m : messages_)
    {
        if (m.ours && m.state == State::Sent && m.streamEnd <= acked)
        {
            m.state = State::Delivered;
            logState(m);
            changed();
        }
    }
    sent_.acknowledged(acked);
    if (traffic_ != nullptr && connectedMs_ != 0) traffic_->acknowledged(peer_, acked);
}

void SessionChat::checkFarewell(uint64_t nowMs)
{
    if (!farewellPending_) return;
    if (account_.read() < farewellEnd_ && nowMs < farewellMs_ + FAREWELL_WAIT_MS) return;
    farewellPending_ = false;
    link_.leave(nowMs);
}

void SessionChat::onEvent(const link::HostLink::Event& event, uint64_t nowMs)
{
    using Type = LinkEvent::Type;
    switch (event.type)
    {
        case Type::SessionCalling:
            if (phase_ != Phase::Calling)
            {
                // Called through the link directly: show it all the same.
                phase_ = Phase::Calling;
                peer_ = event.peer;
                incoming_ = false;
                tries_ = 0;
                sinceMs_ = nowMs;
                connectedMs_ = 0;
                showHistory(peer_);
                changed();
            }
            break;
        case Type::SessionConnected:
            begin(event.peer, event.incoming, event.number, nowMs);
            break;
        case Type::SessionEnded:
        {
            using End = link::HostLink::SessionEnd;
            Ending ending = Ending::LinkLost;
            // A session ended while up: what we know first, then what happened.
            auto ended = [&](Ending otherwise) {
                return cancelling_     ? Ending::Cancelled
                       : gaveUp_        ? Ending::NoAnswer
                       : dataLost_      ? Ending::DataLost
                       : farewellHeard_ ? Ending::LeftThere
                       : phase_ == Phase::Leaving && connectedMs_ != 0 ? Ending::Left
                                                                       : otherwise;
            };
            // A host since Data2G PR #122 says why; CLIENT and ABORT are our
            // own Leave, which the flags above say more about.
            const std::string& why = event.disconnect.reason;
            bool hostSays = event.sessionEnd != End::HostLost && !why.empty() && why != "CLIENT" && why != "ABORT";
            if (hostSays)
            {
                if (why == "NO_ANSWER")
                    ending = cancelling_ ? Ending::Cancelled : Ending::NoAnswer;
                // The far end said no: that, even if we were stopping the call.
                else if (why == "BUSY")
                    ending = Ending::Busy;
                else if (why == "NOT_LISTENING")
                    ending = Ending::NotListening;
                else if (why == "VERSION" || why == "REFUSED")
                    ending = Ending::RefusedThere;
                else if (why == "IDLE")
                    ending = ended(Ending::IdleClosed);
                else if (why == "PEER")
                    ending = ended(Ending::ClosedThere);
                // LINK_LOST, LINK_FAILED, PEER_RECONNECTED, and any reason
                // newer than this.
                else
                    ending = ended(Ending::LinkLost);
            }
            else
            {
                switch (event.sessionEnd)
                {
                    case End::NoAnswer:
                        ending = cancelling_ ? Ending::Cancelled : Ending::NoAnswer;
                        break;
                    case End::Refused:
                        ending = Ending::Refused;
                        break;
                    case End::Left:
                        ending = gaveUp_      ? Ending::NoAnswer
                                 : cancelling_ ? Ending::Cancelled
                                 : dataLost_   ? Ending::DataLost
                                               : Ending::Left;
                        break;
                    case End::Ended:
                        ending = ended(Ending::LinkLost);
                        break;
                    case End::HostLost:
                        ending = Ending::HostLost;
                        break;
                }
            }
            if (phase_ == Phase::Calling && ending == Ending::LinkLost) ending = Ending::NoAnswer;
            why_ = event.sessionEnd == End::HostLost ? link::CommandEvent::Disconnect() : event.disconnect;
            idleCloseMs_ = HOST_IDLE_CLOSE_MS;
            if (std::optional<double> idle = link_.idleTimeout(); idle && *idle > 0.0)
                idleCloseMs_ = (uint64_t)std::llround(std::min(*idle, 1e9) * 1000.0);
            finish(ending, nowMs);
            break;
        }
        case Type::Buffer:
        {
            if (!(phase_ == Phase::Connected || phase_ == Phase::Leaving) || connectedMs_ == 0) break;
            int before = account_.waiting();
            account_.buffer(event.count, nowMs);
            // A fall is the far end's acknowledgement: we hear it.
            if (event.count < before) heard(nowMs);
            if (before == 0 && event.count > 0) waitingSinceMs_ = std::max(waitingSinceMs_, nowMs);
            settle();
            checkFarewell(nowMs);
            changed();
            break;
        }
        case Type::Ptt:
            // Each call keys the transmitter.
            if (phase_ == Phase::Calling && event.on && !pttOn_)
            {
                tries_++;
                changed();
            }
            pttOn_ = event.on;
            break;
        case Type::Clean:
            if ((phase_ == Phase::Connected || phase_ == Phase::Leaving) && connectedMs_ != 0)
                onClean(event.clean, nowMs);
            changed();
            break;
        case Type::ModeUsed:
            // While a broadcast of ours is with the host, the mode may be its;
            // an ACK burst's mode is never the gear the session's data goes in.
            if ((phase_ == Phase::Connected || phase_ == Phase::Leaving) && !link_.burstWithHost()
                && !link::isAckMode(event.text) && event.text != mode_)
            {
                mode_ = event.text;
                changed();
            }
            break;
        default:
            break;
    }
}

void SessionChat::begin(const std::string& peer, bool incoming, int bandwidth, uint64_t nowMs)
{
    bool stopping = cancelling_ || gaveUp_;
    endAfterglow();
    if (incoming || phase_ != Phase::Calling || peer != peer_) showHistory(peer);
    if (log_ != nullptr) log_->began(peer, nowMs);
    phase_ = Phase::Connected;
    peer_ = peer;
    incoming_ = incoming;
    bandwidthHz_ = bandwidth;
    connectedMs_ = sinceMs_ = nowMs;
    lastHeardMs_ = waitingSinceMs_ = nowMs;
    // The host reports a mode only when it changes: a session in the mode
    // of the burst before it hears none.
    mode_ = link_.lastDataMode();
    farewellHeard_ = farewellPending_ = false;
    greetedTold_ = false;
    if (!stopping) cancelling_ = gaveUp_ = false;
    dataLost_ = false;
    numbering_ = protocol::Numbering(settings_.callsign);
    account_.reset();
    out_.clear();
    queued_.clear();
    reactionsHeld_.clear();
    endCut();
    cuts_ = 0;
    cutBytes_ = 0;

    SessionChat::Event started;
    started.type = SessionChat::Event::Type::Started;
    started.peer = peer;
    started.incoming = incoming;
    emit(started);
    changed();

    if (stopping || !dataUp_)
    {
        // A call stopped as it was answered, or no data port to talk on.
        dataLost_ = !dataUp_ && !stopping;
        phase_ = Phase::Leaving;
        link_.leaveNow(nowMs);
        return;
    }

    protocol::Hello hello;
    hello.acceptsSessions = settings_.acceptsSessions;
    hello.locator = settings_.locator;
    hello.software = settings_.software;
    hello.takes = settings_.takes;
    write(protocol::streamOpening(numbering_.record(hello, seconds(nowMs))), nowMs);

    // Late bytes of the last session made the reader give up: start clean.
    if (reader_.foreign() && early_.empty()) reader_.reset();
    std::vector<protocol::Record> early;
    early.swap(early_);
    for (const protocol::Record& r : early) onRecord(r, nowMs);
}

void SessionChat::finish(Ending ending, uint64_t nowMs)
{
    bool wasUp = connectedMs_ != 0 && (phase_ == Phase::Connected || phase_ == Phase::Leaving);
    for (Message& m : messages_)
    {
        if (m.ours && (m.state == State::Sent || m.state == State::Queued)) m.state = State::NotDelivered;
        // A reaction still held never went.
        m.myWaiting = 0;
    }
    queued_.clear();
    reactionsHeld_.clear();
    if (wasUp && traffic_ != nullptr) traffic_->ended(peer_, nowMs);
    // The log's end mark waits out the afterglow, for the last words in it.
    if (wasUp && log_ != nullptr) logEndMs_ = std::max<uint64_t>(nowMs, 1);
    phase_ = Phase::Ended;
    ending_ = ending;
    sinceMs_ = nowMs;
    pttOn_ = false;
    farewellPending_ = false;
    // Ready for the next session's stream (docs/PROTOCOL.md, "Sessions"),
    // after a moment for a farewell still on its way.
    early_.clear();
    out_.clear();
    endCut();
    if (ending == Ending::LinkLost || ending == Ending::ClosedThere)
    {
        afterglowUntilMs_ = std::max<uint64_t>(nowMs + AFTERGLOW_MS, 1);
    }
    else
    {
        reader_.reset();
        endLog();
    }

    SessionChat::Event e;
    e.type = SessionChat::Event::Type::Ended;
    e.peer = peer_;
    e.ending = ending;
    emit(e);
    changed();
}

void SessionChat::endAfterglow()
{
    if (afterglowUntilMs_ == 0) return;
    afterglowUntilMs_ = 0;
    reader_.reset();
    endLog();
}

void SessionChat::endLog()
{
    if (logEndMs_ == 0) return;
    if (log_ != nullptr) log_->ended(peer_, logEndMs_);
    logEndMs_ = 0;
}

void SessionChat::showHistory(const std::string& peer)
{
    endLog();
    messages_.clear();
    if (log_ != nullptr)
    {
        for (Message& m : log_->load(peer, MAX_MESSAGES))
        {
            m.id = nextId_++;
            messages_.push_back(std::move(m));
        }
    }
    changed();
}

void SessionChat::logState(const Message& m)
{
    if (log_ != nullptr && !m.past) log_->settled(peer_, m);
}

void SessionChat::dataPort(bool up, uint64_t nowMs)
{
    if (up == dataUp_) return;
    dataUp_ = up;
    if (!up)
    {
        out_.clear();
        outMessages_.clear();
        if (active())
        {
            dataLost_ = true;
            farewellPending_ = false;
            if (phase_ == Phase::Calling) cancelling_ = false;
            phase_ = Phase::Leaving;
            link_.leaveNow(nowMs);
        }
        else
        {
            afterglowUntilMs_ = 0;
            reader_.reset();
            early_.clear();
        }
    }
    changed();
}

void SessionChat::tick(uint64_t nowMs)
{
    if (afterglowUntilMs_ != 0 && nowMs >= afterglowUntilMs_) endAfterglow();
    if (phase_ == Phase::Calling && nowMs >= sinceMs_ + CALL_GIVE_UP_MS)
    {
        gaveUp_ = true;
        phase_ = Phase::Leaving;
        link_.leaveNow(nowMs);
        changed();
    }
    stepCut(nowMs);
    checkFarewell(nowMs);
}

std::vector<SessionChat::Event> SessionChat::takeEvents()
{
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

//-------------------------------------------------------------------------
// What the tab shows
//-------------------------------------------------------------------------

int SessionChat::callTry() const
{
    return std::max(1, std::min(CALL_TRIES, tries_));
}

bool SessionChat::quiet(uint64_t nowMs) const
{
    bool up = phase_ == Phase::Connected || phase_ == Phase::Leaving;
    return up && connectedMs_ != 0 && account_.waiting() > 0 && nowMs >= waitingSinceMs_ + QUIET_MS;
}

uint64_t SessionChat::quietForMs(uint64_t nowMs) const
{
    return nowMs > waitingSinceMs_ ? nowMs - waitingSinceMs_ : 0;
}

bool SessionChat::trafficBusy() const
{
    return traffic_ != nullptr && active() && traffic_->busy(peer_);
}

const Message* SessionChat::find(uint64_t id) const
{
    for (const Message& m : messages_)
    {
        if (m.id == id) return &m;
    }
    return nullptr;
}

void SessionChat::trim()
{
    while (messages_.size() > MAX_MESSAGES) messages_.pop_front();
}

SessionChat::Strip SessionChat::strip(uint64_t nowMs, long utcOffsetS) const
{
    Strip s;
    s.live = active();
    std::string at = clockTime(sinceMs_, utcOffsetS);
    int waiting = account_.waiting();
    std::string bytes = std::to_string(waiting) + (waiting == 1 ? " byte" : " bytes");
    switch (phase_)
    {
        case Phase::None:
            s.text = "No session. Call a station from the Station menu or its right-click menu, "
                     "or wait for one to call you.";
            break;
        case Phase::Calling:
            s.text = "Calling " + peer_ + ", try " + std::to_string(callTry()) + " of " + std::to_string(CALL_TRIES);
            break;
        case Phase::Connected:
        {
            if (quiet(nowMs))
            {
                s.text = "Link quiet" + std::string(DOT) + clockLength(quietForMs(nowMs)) + DOT
                         + "data2g-host is still retrying" + DOT + bytes + " waiting";
                s.amber = true;
                break;
            }
            bool fresh = incoming_ && nowMs < connectedMs_ + 60000;
            s.text = fresh ? peer_ + " started a session with you" : "Connected to " + peer_;
            s.text += DOT + sessionLength(nowMs - std::min(nowMs, connectedMs_));
            if (bandwidthHz_ > 0) s.text += DOT + std::to_string(bandwidthHz_) + " Hz";
            if (!mode_.empty()) s.text += DOT + mode_;
            if (waiting > 0) s.text += DOT + bytes + " waiting";
            s.text += DOT + std::string("heard ") + broadcast::heardAgo(lastHeardMs_, nowMs);
            if (reader_.foreign()) s.text += DOT + peer_ + "'s end isn't speaking Ragtime: nothing more is read from it";
            break;
        }
        case Phase::Leaving:
            if (cancelling_ || gaveUp_ || connectedMs_ == 0)
                s.text = "Stopping the call to " + peer_;
            else if (waiting > 0)
                s.text = "Leaving, waiting for " + bytes + " to be acknowledged";
            else
                s.text = "Leaving the session with " + peer_;
            s.amber = quiet(nowMs);
            break;
        case Phase::Ended:
            switch (ending_)
            {
                case Ending::NoAnswer:
                    s.text = peer_ + " didn't answer (" + at + ")";
                    break;
                case Ending::Refused:
                    s.text = "data2g-host wouldn't call " + peer_ + " (" + at + ")";
                    break;
                case Ending::Cancelled:
                    s.text = "Call to " + peer_ + " cancelled at " + at;
                    break;
                case Ending::Left:
                    s.text = "You left the session with " + peer_ + " at " + at + ", after "
                             + sessionLength(sinceMs_ - std::min(sinceMs_, connectedMs_));
                    // The DISC went out, but no acknowledgement came back.
                    if (why_.unconfirmed) s.text += DOT + peer_ + " didn't confirm it";
                    break;
                case Ending::LeftThere:
                    s.text = "Session ended by " + peer_ + " at " + at;
                    break;
                case Ending::LinkLost:
                {
                    uint64_t silent = sinceMs_ > lastHeardMs_ ? sinceMs_ - lastHeardMs_ : 0;
                    const std::string& why = why_.reason;
                    if (why == "LINK_FAILED")
                        s.text = "Link failed at " + at + ": data2g-host gave up on it"
                                 + (why_.detail.empty() ? std::string() : " (" + why_.detail + ")");
                    else if (why == "PEER_RECONNECTED")
                        s.text = peer_ + " called again at " + at + ", which ended the session that was up";
                    // An older host gives no reason: the quiet may have been its idle timeout.
                    else if (why.empty() && nearIdle(silent, idleCloseMs_, HOST_IDLE_EARLY_MS, HOST_IDLE_LATE_MS))
                        s.text = "data2g-host closed the quiet session with " + peer_ + " at " + at
                                 + ": it hangs up after " + idleLength(idleCloseMs_) + " with nothing sent either way";
                    else if (silent >= 60000)
                        s.text = "Link lost at " + at + " after " + sessionLength(silent) + " without contact";
                    else if (why == "LINK_LOST")
                        s.text = "Link lost at " + at;
                    else
                        s.text = "The session with " + peer_ + " ended at " + at;
                    break;
                }
                case Ending::Busy:
                    s.text = peer_ + " is in another session (" + at + ")";
                    break;
                case Ending::NotListening:
                    s.text = peer_ + " isn't taking sessions (" + at + ")";
                    break;
                case Ending::RefusedThere:
                    if (why_.reason == "VERSION")
                        s.text = peer_ + "'s data2g-host can't make sessions with yours: another version (" + at + ")";
                    else
                        s.text = peer_ + "'s station refused the call ("
                                 + (why_.detail.empty() ? std::string() : why_.detail + ", ") + at + ")";
                    break;
                case Ending::IdleClosed:
                    s.text = "data2g-host closed the quiet session with " + peer_ + " at " + at + ": it hangs up after "
                             + idleLength(idleCloseMs_) + " with nothing sent either way";
                    break;
                case Ending::ClosedThere:
                {
                    // Its host's idle timeout is likely Data2G's own, or ours.
                    uint64_t silent = sinceMs_ > lastHeardMs_ ? sinceMs_ - lastHeardMs_ : 0;
                    if (nearIdle(silent, HOST_IDLE_CLOSE_MS, HOST_IDLE_EARLY_MS, HOST_IDLE_LATE_MS)
                        || nearIdle(silent, idleCloseMs_, HOST_IDLE_EARLY_MS, HOST_IDLE_LATE_MS))
                        s.text = peer_ + "'s data2g-host closed the quiet session at " + at
                                 + ": nothing was sent either way for " + sessionLength(silent);
                    else
                        s.text = peer_ + "'s end closed the session at " + at;
                    break;
                }
                case Ending::HostLost:
                    s.text = "data2g-host went away at " + at + ", and the session with " + peer_ + " with it";
                    break;
                case Ending::DataLost:
                    s.text = "The session data port closed at " + at + ", so the session with " + peer_ + " ended";
                    break;
            }
            break;
    }
    return s;
}

} // namespace session
} // namespace ragtime
