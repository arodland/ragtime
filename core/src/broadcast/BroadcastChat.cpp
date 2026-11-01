#include "ragtime/broadcast/BroadcastChat.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ragtime/broadcast/Airtime.h"
#include "ragtime/broadcast/BroadcastLog.h"
#include "ragtime/gearbox/Gearbox.h"

namespace ragtime
{
namespace broadcast
{

namespace
{

using State = Message::State;
using Event = link::HostLink::Event;
using BurstState = link::HostLink::BurstState;

uint32_t seconds(uint64_t nowMs)
{
    return (uint32_t)(nowMs / 1000);
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool blank(const std::string& text)
{
    return std::all_of(text.begin(), text.end(), isSpace);
}

// The start of the UTF-8 character at or before at.
size_t characterStart(const std::string& text, size_t at)
{
    while (at > 0 && at < text.size() && ((uint8_t)text[at] & 0xC0) == 0x80) at--;
    return at;
}

State fromBurst(BurstState state)
{
    switch (state)
    {
        case BurstState::Queued:
            return State::Queued;
        case BurstState::Sending:
            return State::Sending;
        case BurstState::Held:
            return State::Held;
        case BurstState::Sent:
            return State::Sent;
        case BurstState::NotSent:
            return State::NotSent;
    }
    return State::NotSent;
}

} // namespace

const char* stateLabel(Message::State state)
{
    switch (state)
    {
        case State::Heard:
            return "";
        case State::Queued:
            return "Queued";
        case State::Sending:
            return "Sending";
        case State::Held:
            return "Waiting for the session to go idle";
        case State::Sent:
            return "Sent";
        case State::NotSent:
            return "Not sent";
        case State::Cancelled:
            return "Cancelled";
        case State::Acknowledged:
            return "Acknowledged";
    }
    return "";
}

std::string pingLine(const Message& m)
{
    if (m.ours)
    {
        const std::string ping = "Ping " + m.to;
        switch (m.state)
        {
            case State::Queued:
                return ping + ", queued";
            case State::Sending:
                return ping + ", sending";
            case State::Held:
                return ping + ", held until the session goes idle";
            case State::NotSent:
                return ping + " not sent";
            case State::Cancelled:
                return ping + " cancelled";
            case State::Sent:
            case State::Heard:
                break;
            case State::Acknowledged:
            {
                uint64_t took = m.answeredMs > m.sentMs ? (m.answeredMs - m.sentMs + 500) / 1000 : 0;
                std::string line = "Pong from " + m.to + " in " + std::to_string(took) + " s: ";
                line += m.theyHear ? "it hears us at " + gearbox::formatDb(*m.theyHear) : "it gave no S/N for us";
                line += m.weHear ? ", we hear it at " + gearbox::formatDb(*m.weHear) : ", our host gave no S/N for it";
                return line;
            }
        }
        if (!m.unanswered) return ping + ", waiting for its pong";
        uint64_t waited = m.dueMs > m.sentMs ? (m.dueMs - m.sentMs + 500) / 1000 : 0;
        return "No pong from " + m.to + " in " + std::to_string(waited) + " s";
    }
    std::string line = m.from + " pinged us";
    if (m.weHear) line += "; we hear it at " + gearbox::formatDb(*m.weHear);
    switch (m.state)
    {
        case State::Sent:
            return line + ". Pong sent";
        case State::NotSent:
            return line + ". Pong not sent";
        case State::Cancelled:
            return line + ". Pong cancelled";
        case State::Held:
            return line + ". Pong held until the session goes idle";
        case State::Sending:
            return line + ". Pong sending";
        case State::Queued:
        case State::Heard:
        case State::Acknowledged:
            break;
    }
    return line + ". Pong queued";
}

uint64_t pongWaitMs(const link::ModeInfo& mode)
{
    return std::max(MIN_PONG_WAIT_MS, PONG_SNR_WAIT_MS + 2 * listeningWindowMs(mode));
}

BroadcastChat::BroadcastChat(link::HostLink& link) : link_(link)
{
}

void BroadcastChat::setSettings(const Settings& settings, uint32_t newestItem)
{
    Settings next = settings;
    next.callsign = protocol::normalCallsign(settings.callsign);
    bool newCall = next.callsign != settings_.callsign || numbering_.callsign() != next.callsign;
    bool changed = newCall || next.locator != settings_.locator || next.acceptsSessions != settings_.acceptsSessions
                   || next.software != settings_.software || next.takes != settings_.takes;
    settings_ = next;
    if (newCall) numbering_ = protocol::Numbering(settings_.callsign, newestItem);
    if (changed) helloSent_ = false;
    version_++;
}

bool BroadcastChat::addressee(const std::string& typed, std::string& call, std::string& rest)
{
    size_t at = 0;
    while (at < typed.size() && isSpace(typed[at])) at++;
    if (at >= typed.size() || typed[at] != '@') return false;
    size_t end = at + 1;
    while (end < typed.size() && !isSpace(typed[end]) && typed[end] != ',' && typed[end] != ':') end++;
    std::string normal = protocol::normalCallsign(typed.substr(at + 1, end - at - 1));
    if (normal.empty()) return false;
    while (end < typed.size() && (isSpace(typed[end]) || typed[end] == ',' || typed[end] == ':')) end++;
    if (end >= typed.size()) return false;
    call = normal;
    rest = typed.substr(end);
    return true;
}

std::vector<text::HamText::PhraseSpan> BroadcastChat::phrases(const std::string& typed)
{
    std::string call, rest;
    if (!addressee(typed, call, rest)) return text::HamText::phraseSpans(typed);
    std::vector<text::HamText::PhraseSpan> spans = text::HamText::phraseSpans(rest);
    for (text::HamText::PhraseSpan& span : spans) span.start += typed.size() - rest.size();
    return spans;
}

protocol::Record BroadcastChat::helloRecord(uint32_t time)
{
    protocol::Hello hello;
    hello.acceptsSessions = settings_.acceptsSessions;
    hello.locator = settings_.locator;
    hello.software = settings_.software;
    hello.takes = settings_.takes;
    return numbering_.record(hello, time);
}

bool BroadcastChat::helloDue(uint64_t nowMs) const
{
    return !helloSent_ || nowMs >= helloMs_ + HELLO_EVERY_MS;
}

std::vector<std::string> BroadcastChat::split(const std::string& text, const std::string& to, size_t firstRoom)
{
    // A Text of these bytes, with a header like ours: its seq and time are
    // fixed-size, so any will do for measuring.
    auto bytes = [&](const std::string& piece) {
        protocol::Record r;
        r.header.callsign = settings_.callsign;
        protocol::Text t;
        t.text = piece;
        t.to = to;
        t.ackWanted = !to.empty();
        r.body = t;
        return encodeRecord(r).size();
    };

    std::vector<std::string> out;
    size_t at = 0;
    size_t room = firstRoom;
    while (at < text.size())
    {
        size_t budget = protocol::MAX_BROADCAST_FRAME - 1 - room;
        std::string rest = text.substr(at);
        if (bytes(rest) <= budget)
        {
            out.push_back(rest);
            break;
        }
        // The longest start that fits: coded text grows with its length,
        // near enough, so halve the gap until it closes.
        size_t lo = 0, hi = rest.size();
        while (lo + 1 < hi)
        {
            size_t mid = characterStart(rest, (lo + hi) / 2);
            if (mid <= lo) mid = lo + 1;
            if (bytes(rest.substr(0, mid)) <= budget) lo = mid;
            else hi = mid;
        }
        size_t cut = characterStart(rest, lo);
        if (cut == 0) cut = std::min(rest.size(), (size_t)1);
        // Between words, if there is a space in the second half.
        size_t space = rest.rfind(' ', cut);
        if (space != std::string::npos && space > cut / 2 && space < cut)
        {
            out.push_back(rest.substr(0, space));
            at += space + 1;
        }
        else
        {
            out.push_back(rest.substr(0, cut));
            at += cut;
        }
        room = 0;
    }
    return out;
}

std::vector<uint64_t> BroadcastChat::send(const std::string& typed, const std::string& mode, uint64_t nowMs)
{
    std::vector<uint64_t> ids;
    if (settings_.callsign.empty() || blank(typed) || typed.size() > MAX_TYPED) return ids;

    std::string to, text = typed;
    std::string call, rest;
    if (addressee(typed, call, rest))
    {
        to = call;
        text = rest;
    }

    uint32_t time = seconds(nowMs);
    std::vector<protocol::Record> first;
    if (helloDue(nowMs)) first.push_back(helloRecord(time));
    size_t room = first.empty() ? 0 : protocol::encodeRecord(first.front()).size();

    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint64_t> made;
    std::vector<protocol::Record> items;
    for (const std::string& piece : split(text, to, room))
    {
        protocol::Text t;
        t.text = piece;
        t.to = to;
        t.ackWanted = !to.empty();
        protocol::Record record = numbering_.record(t, time);
        std::vector<protocol::Record> records = frames.empty() ? first : std::vector<protocol::Record>();
        records.push_back(record);
        std::vector<uint8_t> frame = protocol::encodeFrame(records);
        if (frame.empty()) continue;
        frames.push_back(std::move(frame));

        Message m;
        m.id = nextId_++;
        m.ours = true;
        m.from = settings_.callsign;
        m.to = to;
        m.ackWanted = t.ackWanted;
        m.text = piece;
        m.seq = record.header.seq;
        m.time = time;
        m.atMs = nowMs;
        m.state = State::Queued;
        m.mode = mode;
        records_[m.id] = record;
        items.push_back(record);
        messages_.push_back(m);
        made.push_back(m.id);
    }
    if (frames.empty()) return ids;

    if (!first.empty())
    {
        helloSent_ = true;
        helloMs_ = nowMs;
    }
    uint64_t burst = broadcast(frames, mode, nowMs, link::snippet(typed));
    for (Message& m : messages_)
    {
        if (std::find(made.begin(), made.end(), m.id) == made.end()) continue;
        m.burst = burst;
        if (log_ != nullptr) log_->added(m);
    }
    if (onItems_ && burst != 0) onItems_(items, burst, mode, nowMs);
    trim();
    version_++;
    return made;
}

bool BroadcastChat::checkIn(const std::string& mode, uint64_t nowMs)
{
    if (settings_.callsign.empty()) return false;
    std::vector<uint8_t> frame = protocol::encodeFrame({helloRecord(seconds(nowMs))});
    if (frame.empty()) return false;
    helloSent_ = true;
    helloMs_ = nowMs;
    return broadcast({frame}, mode, nowMs, "check-in") != 0;
}

bool BroadcastChat::announce(protocol::Status::State state, const std::string& partner, const std::string& mode,
                             uint64_t nowMs)
{
    using StatusState = protocol::Status::State;
    if (settings_.callsign.empty()) return false;
    if (state == StatusState::Qrv)
    {
        if (!qrlOut_) return false;
        qrlOut_ = false;
        // Nobody heard the QRL: nothing to take back on the air.
        if (link_.cancel(qrlBurst_)) return false;
    }
    protocol::Status status;
    status.state = state;
    if (state == StatusState::Qrl) status.partner = protocol::normalCallsign(partner);
    std::vector<uint8_t> frame = protocol::encodeFrame({numbering_.record(status, seconds(nowMs))});
    if (frame.empty()) return false;
    uint64_t id = link_.broadcast({frame}, mode, nowMs, state == StatusState::Qrl ? "QRL" : "QRV");
    if (state == StatusState::Qrl)
    {
        qrlBurst_ = id;
        qrlOut_ = id != 0;
    }
    return id != 0;
}

uint64_t BroadcastChat::ping(const std::string& call, const std::string& mode, uint64_t nowMs)
{
    std::string to = protocol::normalCallsign(call);
    if (settings_.callsign.empty() || to.empty() || to == settings_.callsign) return 0;
    if (++pingNumber_ == 0) pingNumber_ = 1;
    protocol::Ping p;
    p.to = to;
    p.number = pingNumber_;
    // The mode it goes in, so the Pong comes back in it: the gear is
    // tried both ways.
    p.mode = mode.empty() ? link_.groupMode() : mode;
    std::vector<uint8_t> frame = protocol::encodeFrame({numbering_.record(p, seconds(nowMs))});
    if (frame.empty()) return 0;

    Message m;
    m.id = nextId_++;
    m.kind = Message::Kind::Ping;
    m.ours = true;
    m.from = settings_.callsign;
    m.to = to;
    m.time = seconds(nowMs);
    m.atMs = nowMs;
    m.state = State::Queued;
    m.mode = mode;
    m.pingNumber = p.number;
    m.burst = broadcast({frame}, mode, nowMs, "ping " + to);
    if (m.burst == 0) m.state = State::NotSent;
    messages_.push_back(std::move(m));
    trim();
    version_++;
    return messages_.back().id;
}

bool BroadcastChat::pinging(const std::string& call) const
{
    for (const Message& m : messages_)
    {
        if (m.kind != Message::Kind::Ping || !m.ours || m.to != call) continue;
        if (m.state == State::Queued || m.state == State::Sending || m.state == State::Held) return true;
        if (m.state == State::Sent && !m.unanswered) return true;
    }
    return false;
}

void BroadcastChat::tick(uint64_t nowMs)
{
    for (size_t i = 0; i < pongsDue_.size();)
    {
        if (pongsDue_[i].untilMs > nowMs)
        {
            i++;
            continue;
        }
        PongDue due = pongsDue_[i];
        pongsDue_.erase(pongsDue_.begin() + (long)i);
        sendPong(due, std::nullopt, nowMs);
    }
    // Reactions that found nothing to ride with go alone.
    if (!reactionsDue_.empty() && !settings_.callsign.empty() && nowMs >= reactionsSinceMs_ + REACTION_RIDE_MS
        && (loneReactionsMs_ == 0 || nowMs >= loneReactionsMs_ + REACTION_LONE_GAP_MS))
    {
        if (broadcast({}, std::string(), nowMs, std::string()) != 0) loneReactionsMs_ = nowMs;
    }
    for (Message& m : messages_)
    {
        if (m.kind != Message::Kind::Ping || !m.ours || m.state != State::Sent || m.unanswered) continue;
        if (nowMs < m.dueMs) continue;
        m.unanswered = true;
        ackHeard(m.id, nowMs);
        version_++;
    }
}

void BroadcastChat::onPing(const protocol::Record& record, const protocol::Ping& ping, uint64_t nowMs)
{
    const std::string& from = record.header.callsign;
    if (settings_.callsign.empty() || ping.to != settings_.callsign) return;
    // A station pinging again and again gets one Pong in PONG_GAP_MS.
    auto last = pongedMs_.find(from);
    if (last != pongedMs_.end() && nowMs < last->second + PONG_GAP_MS) return;
    for (const PongDue& due : pongsDue_)
    {
        if (due.to == from) return;
    }
    pongedMs_[from] = nowMs;

    PongDue due;
    due.to = from;
    due.number = ping.number;
    due.mode = ping.mode;
    due.untilMs = nowMs + PONG_SNR_WAIT_MS;

    Message m;
    m.id = nextId_++;
    m.kind = Message::Kind::Ping;
    m.from = from;
    m.to = settings_.callsign;
    m.seq = record.header.seq;
    m.time = record.header.time;
    m.atMs = nowMs;
    m.state = State::Queued;
    m.pingNumber = ping.number;
    m.weHear = recentSnr(from, nowMs);
    due.row = m.id;
    messages_.push_back(std::move(m));
    trim();
    version_++;

    // The host's figure for the Ping's burst may be here already, or come
    // in a moment.
    if (std::optional<double> snr = recentSnr(from, nowMs))
        sendPong(due, snr, nowMs);
    else
        pongsDue_.push_back(due);
}

void BroadcastChat::sendPong(const PongDue& due, std::optional<double> snr, uint64_t nowMs)
{
    protocol::Pong p;
    p.to = due.to;
    p.number = due.number;
    p.snr = snr;
    // Back in the Ping's mode, so a Pong says the gear works both ways;
    // the group's when our host doesn't offer it.
    std::string mode = findMode(link_.modes(), due.mode).valid() ? due.mode : std::string();
    std::vector<uint8_t> frame = protocol::encodeFrame({numbering_.record(p, seconds(nowMs))});
    uint64_t burst = frame.empty() ? 0 : broadcast({frame}, mode, nowMs, "pong " + due.to);
    for (Message& m : messages_)
    {
        if (m.id != due.row) continue;
        m.burst = burst;
        m.mode = mode.empty() ? link_.groupMode() : mode;
        if (burst == 0) m.state = State::NotSent;
        version_++;
    }
}

void BroadcastChat::onPong(const protocol::Record& record, const protocol::Pong& pong, uint64_t nowMs)
{
    const std::string& from = record.header.callsign;
    if (settings_.callsign.empty() || pong.to != settings_.callsign) return;
    for (Message& m : messages_)
    {
        if (m.kind != Message::Kind::Ping || !m.ours || m.to != from || m.pingNumber != pong.number) continue;
        if (m.state == State::Acknowledged || m.state == State::Cancelled) continue;
        // Its own Sent may be on its way still: the Pong says it went.
        if (m.sentMs == 0) m.sentMs = nowMs;
        m.state = State::Acknowledged;
        m.unanswered = false;
        m.answeredMs = nowMs;
        m.theyHear = pong.snr;
        m.weHear = recentSnr(from, nowMs);
        ackHeard(m.id, nowMs);
        version_++;
    }
}

void BroadcastChat::heardSnr(const std::string& call, double dB, uint64_t nowMs)
{
    snrs_[call] = {dB, nowMs};
    for (size_t i = 0; i < pongsDue_.size();)
    {
        if (pongsDue_[i].to != call)
        {
            i++;
            continue;
        }
        PongDue due = pongsDue_[i];
        pongsDue_.erase(pongsDue_.begin() + (long)i);
        sendPong(due, dB, nowMs);
    }
    // A figure that came just after its Pong (or its Ping) was taken.
    for (Message& m : messages_)
    {
        if (m.kind != Message::Kind::Ping || m.weHear) continue;
        bool pong = m.ours && m.to == call && m.state == State::Acknowledged
                    && nowMs <= m.answeredMs + PONG_SNR_WAIT_MS;
        bool ping = !m.ours && m.from == call && nowMs <= m.atMs + PONG_SNR_WAIT_MS;
        if (!pong && !ping) continue;
        m.weHear = dB;
        version_++;
    }
}

std::optional<double> BroadcastChat::recentSnr(const std::string& call, uint64_t nowMs) const
{
    auto it = snrs_.find(call);
    if (it == snrs_.end() || nowMs > it->second.second + SNR_SAME_BURST_MS) return std::nullopt;
    return it->second.first;
}

Message* BroadcastChat::findMine(uint64_t id)
{
    for (Message& m : messages_)
    {
        if (m.id == id) return m.ours ? &m : nullptr;
    }
    return nullptr;
}

Message* BroadcastChat::findMessage(uint64_t id)
{
    for (Message& m : messages_)
    {
        if (m.id == id) return &m;
    }
    return nullptr;
}

const Message* BroadcastChat::find(uint64_t id) const
{
    for (const Message& m : messages_)
    {
        if (m.id == id) return &m;
    }
    return nullptr;
}

bool BroadcastChat::cancel(uint64_t id)
{
    Message* m = findMine(id);
    if (m == nullptr || m->state != State::Queued) return false;
    uint64_t burst = m->burst;
    if (!link_.cancel(burst)) return false;
    reactionsSettled(burst, false, 0);
    for (Message& other : messages_)
    {
        if (!other.ours || other.burst != burst) continue;
        other.state = State::Cancelled;
        logState(other);
    }
    version_++;
    return true;
}

bool BroadcastChat::canResend(const Message& message) const
{
    if (message.kind != Message::Kind::Text || !message.ours) return false;
    // One from the log is rebuilt under its own callsign and number.
    if (message.past && (message.from != settings_.callsign || message.seq == 0)) return false;
    return message.state == State::Sent || message.state == State::NotSent || message.state == State::Cancelled
           || message.state == State::Acknowledged;
}

bool BroadcastChat::resend(uint64_t id, const std::string& mode, uint64_t nowMs)
{
    Message* m = findMine(id);
    if (m == nullptr || !canResend(*m)) return false;
    auto record = records_.find(id);
    if (record == records_.end())
    {
        if (!m->past) return false;
        // The same record as the first time: the same callsign, number,
        // time and text, so a station that heard it lists it once.
        protocol::Text t;
        t.text = m->text;
        t.to = m->to;
        t.ackWanted = m->ackWanted;
        protocol::Record r;
        r.body = t;
        r.header.callsign = m->from;
        r.header.seq = m->seq;
        r.header.time = m->time;
        record = records_.emplace(id, std::move(r)).first;
    }
    std::vector<uint8_t> frame = protocol::encodeFrame({record->second});
    if (frame.empty()) return false;
    m->state = State::Queued;
    m->mode = mode;
    m->burst = broadcast({frame}, mode, nowMs, link::snippet(m->to.empty() ? m->text : "@" + m->to + " " + m->text) + " again");
    if (onItems_ && m->burst != 0) onItems_({record->second}, m->burst, mode, nowMs);
    if (m->past)
    {
        // It goes again in this run, so this run's log has it.
        m->past = false;
        if (log_ != nullptr) log_->added(*m);
    }
    else
    {
        logState(*m);
    }
    version_++;
    return true;
}

protocol::Record BroadcastChat::record(protocol::Body body, uint32_t time, uint32_t item)
{
    return numbering_.record(std::move(body), time, item);
}

uint64_t BroadcastChat::addFile(uint64_t file, bool ours, const std::string& from, const std::string& name,
                                uint64_t nowMs)
{
    Message m;
    m.id = nextId_++;
    m.ours = ours;
    m.from = from;
    m.text = name;
    m.time = seconds(nowMs);
    m.atMs = nowMs;
    m.state = State::Heard;
    m.kind = Message::Kind::File;
    m.file = file;
    if (log_ != nullptr) log_->added(m);
    messages_.push_back(std::move(m));
    trim();
    version_++;
    return messages_.back().id;
}

void BroadcastChat::fileEnded(uint64_t file, const std::string& outcome, const std::string& path, bool picture,
                              bool voice)
{
    if (file == 0) return;
    for (Message& m : messages_)
    {
        if (m.file != file) continue;
        m.picture = picture;
        m.voice = voice;
        if (log_ != nullptr) log_->fileEnded(m, outcome, path);
    }
}

std::vector<protocol::Record> BroadcastChat::onEvent(const Event& event, uint64_t nowMs)
{
    std::vector<protocol::Record> heard;
    switch (event.type)
    {
        case Event::Type::GroupOpen:
            // A fresh start with the host: the next message says hello.
            helloSent_ = false;
            break;
        case Event::Type::Burst:
        {
            if (event.burstState == BurstState::Sent || event.burstState == BurstState::NotSent)
                reactionsSettled(event.id, event.burstState == BurstState::Sent, nowMs);
            if (ackBursts_.count(event.id) != 0)
            {
                if (event.burstState == BurstState::Sent || event.burstState == BurstState::NotSent)
                    ackBursts_.erase(event.id);
                break;
            }
            for (Message& m : messages_)
            {
                // Ours, and the Pongs we answer pings with.
                if (m.burst != event.id || (!m.ours && m.kind != Message::Kind::Ping)) continue;
                if (m.state == State::Cancelled || m.state == State::Acknowledged) continue;
                State state = fromBurst(event.burstState);
                if (state == m.state) continue;
                m.state = state;
                if (state == State::Sent && m.ackWanted) holdForAck(m, nowMs);
                if (state == State::Sent && m.ours && m.kind == Message::Kind::Ping)
                {
                    m.sentMs = nowMs;
                    m.dueMs = nowMs + pongWaitMs(findMode(link_.modes(), m.mode, link_.groupMode()));
                    hold(m.id, m.dueMs, nowMs);
                }
                logState(m);
                version_++;
            }
            break;
        }
        case Event::Type::ModeUsed:
            for (Message& m : messages_)
            {
                if (m.ours && m.state == State::Sending)
                {
                    m.mode = event.text;
                    version_++;
                }
            }
            break;
        case Event::Type::HeardFrom:
            if (event.snr) heardSnr(event.text, *event.snr, nowMs);
            break;
        case Event::Type::Heard:
        {
            if (!protocol::decodeFrame(event.frame, heard) && heard.empty()) foreign_++;
            for (const protocol::Record& r : heard) onRecord(r, nowMs);
            flushAcks(std::string(), nowMs);
            break;
        }
        default:
            break;
    }
    return heard;
}

void BroadcastChat::onRebuilt(const protocol::Record& record, uint64_t nowMs)
{
    onRecord(record, nowMs);
    flushAcks(std::string(), nowMs);
}

void BroadcastChat::onRecord(const protocol::Record& record, uint64_t nowMs)
{
    const std::string& from = record.header.callsign;
    if (from.empty() || from == settings_.callsign) return;

    if (const protocol::Text* text = record.as<protocol::Text>())
    {
        bool toMe = !settings_.callsign.empty() && text->to == settings_.callsign;
        // Asked again: our Ack went missing, so it goes again.
        if (toMe && text->ackWanted)
        {
            protocol::Ack ack;
            ack.to = from;
            ack.item = record.header.seq;
            acksDue_.push_back(numbering_.record(ack, seconds(nowMs)));
        }
        ItemKey key{from, record.header.seq, record.header.time};
        if (!seen_.insert(key).second) return;
        Message m;
        m.id = nextId_++;
        m.from = from;
        m.to = text->to;
        m.ackWanted = text->ackWanted;
        m.text = text->text;
        m.seq = record.header.seq;
        m.time = record.header.time;
        m.atMs = nowMs;
        m.state = State::Heard;
        auto at = placeFor(m);
        if (log_ != nullptr) log_->added(m, at != messages_.end() ? at->id : 0);
        Message& line = *messages_.insert(at, std::move(m));
        // Reactions that came before it.
        for (auto e = early_.begin(); e != early_.end();)
        {
            if (e->first.first != from || e->first.second != line.seq)
            {
                ++e;
                continue;
            }
            if (protocol::setReaction(line.reactions, e->second.call, e->second.code) && log_ != nullptr)
                log_->reacted(line, e->second.call, e->second.code);
            e = early_.erase(e);
        }
        trim();
        version_++;
    }
    else if (const protocol::Reaction* reaction = record.as<protocol::Reaction>())
    {
        onReaction(from, *reaction, nowMs);
    }
    else if (const protocol::Ping* ping = record.as<protocol::Ping>())
    {
        onPing(record, *ping, nowMs);
    }
    else if (const protocol::Pong* pong = record.as<protocol::Pong>())
    {
        onPong(record, *pong, nowMs);
    }
    else if (const protocol::Ack* ack = record.as<protocol::Ack>())
    {
        if (ack->to != settings_.callsign) return;
        for (Message& m : messages_)
        {
            if (m.ours && !m.past && m.to == from && m.seq == ack->item && m.state != State::Acknowledged)
            {
                m.state = State::Acknowledged;
                ackHeard(m.id, nowMs);
                logState(m);
                version_++;
            }
        }
    }
}

bool BroadcastChat::canReact(const Message& m) const
{
    return m.kind == Message::Kind::Text && !m.ours && m.seq != 0 && !m.from.empty() && !settings_.callsign.empty()
           && m.from != settings_.callsign;
}

bool BroadcastChat::react(uint64_t id, uint8_t code, uint64_t nowMs)
{
    Message* m = findMessage(id);
    if (m == nullptr || !canReact(*m) || protocol::reactionEntry(code) == nullptr) return false;
    if (code == (m->myWaiting != 0 ? m->myWaiting : m->myReaction)) return false;
    auto out = reactionsOut_.find(id);
    if (out != reactionsOut_.end() ? out->second == code : code == m->myReaction)
    {
        // Back to the one on the air, or going: nothing more need go.
        reactionsDue_.erase(id);
        m->myWaiting = out != reactionsOut_.end() ? code : 0;
        version_++;
        return true;
    }
    m->myWaiting = code;
    if (reactionsDue_.empty()) reactionsSinceMs_ = nowMs;
    reactionsDue_.insert(id);
    version_++;
    return true;
}

bool BroadcastChat::unreact(uint64_t id)
{
    Message* m = findMessage(id);
    if (m == nullptr || reactionsDue_.erase(id) == 0) return false;
    // One already with the host still goes.
    auto out = reactionsOut_.find(id);
    m->myWaiting = out != reactionsOut_.end() ? out->second : 0;
    version_++;
    return true;
}

std::vector<BroadcastChat::ReactionNote> BroadcastChat::takeReactionNotes()
{
    std::vector<ReactionNote> notes;
    notes.swap(reactionNotes_);
    return notes;
}

uint64_t BroadcastChat::broadcast(std::vector<std::vector<uint8_t>> frames, const std::string& mode, uint64_t nowMs,
                                  const std::string& label)
{
    // Our reactions waiting, in frames of their own after the burst's: a
    // frame is cut where a codeword ends anyway, so they cost what they
    // weigh, about 21 bytes each.
    std::vector<std::pair<uint64_t, uint8_t>> taken;
    std::vector<protocol::Record> frame;
    size_t bytes = 1;
    uint32_t time = seconds(nowMs);
    for (uint64_t id : reactionsDue_)
    {
        const Message* m = findMessage(id);
        if (m == nullptr || m->myWaiting == 0) continue;
        protocol::Reaction r;
        r.to = m->from;
        r.item = m->seq;
        r.code = m->myWaiting;
        protocol::Record record = numbering_.record(r, time);
        size_t size = protocol::encodeRecord(record).size();
        if (size == 0) continue;
        if (!frame.empty() && bytes + size > protocol::MAX_BROADCAST_FRAME)
        {
            frames.push_back(protocol::encodeFrame(frame));
            frame.clear();
            bytes = 1;
        }
        frame.push_back(record);
        bytes += size;
        taken.emplace_back(id, r.code);
    }
    if (!frame.empty()) frames.push_back(protocol::encodeFrame(frame));
    frames.erase(std::remove_if(frames.begin(), frames.end(), [](const std::vector<uint8_t>& f) { return f.empty(); }),
                 frames.end());
    // Any left were for lines gone from the list.
    if (taken.empty()) reactionsDue_.clear();
    if (frames.empty()) return 0;

    std::string reactions = taken.size() == 1 ? std::string("reaction") : std::to_string(taken.size()) + " reactions";
    std::string what = taken.empty() ? label : label.empty() ? reactions : label + " + " + reactions;
    uint64_t burst = link_.broadcast(frames, mode, nowMs, what);
    if (burst == 0 || taken.empty()) return burst;
    reactionBursts_[burst] = taken;
    for (const auto& t : taken) reactionsOut_[t.first] = t.second;
    reactionsDue_.clear();
    return burst;
}

void BroadcastChat::reactionsSettled(uint64_t burst, bool sent, uint64_t nowMs)
{
    auto it = reactionBursts_.find(burst);
    if (it == reactionBursts_.end()) return;
    std::vector<std::pair<uint64_t, uint8_t>> taken = std::move(it->second);
    reactionBursts_.erase(it);
    for (const auto& t : taken)
    {
        auto out = reactionsOut_.find(t.first);
        if (out != reactionsOut_.end() && out->second == t.second) reactionsOut_.erase(out);
        Message* m = findMessage(t.first);
        if (m == nullptr) continue;
        if (sent)
        {
            m->myReaction = t.second;
            if (m->myWaiting == t.second && reactionsDue_.count(t.first) == 0) m->myWaiting = 0;
            if (log_ != nullptr) log_->weReacted(*m, t.second);
        }
        else if (m->myWaiting == t.second && reactionsDue_.count(t.first) == 0)
        {
            // Not sent, or taken back with what it rode with: it waits
            // again, to ride with the next or go alone.
            if (reactionsDue_.empty()) reactionsSinceMs_ = nowMs;
            reactionsDue_.insert(t.first);
        }
    }
    version_++;
}

void BroadcastChat::onReaction(const std::string& from, const protocol::Reaction& reaction, uint64_t nowMs)
{
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
    {
        Message& m = *it;
        if (m.kind != Message::Kind::Text || m.seq == 0 || m.from != reaction.to || m.seq != reaction.item) continue;
        if (protocol::setReaction(m.reactions, from, reaction.code))
        {
            if (log_ != nullptr) log_->reacted(m, from, reaction.code);
            if (!settings_.callsign.empty() && m.from == settings_.callsign)
                reactionNotes_.push_back(ReactionNote{from, reaction.code, m.id});
            version_++;
        }
        // The station an @message went to reacting to it: it has the line,
        // as an Ack would say.
        if (m.ours && !m.past && m.ackWanted && m.to == from && m.state != State::Acknowledged
            && m.state != State::Cancelled)
        {
            m.state = State::Acknowledged;
            ackHeard(m.id, nowMs);
            logState(m);
            version_++;
        }
        return;
    }
    // Its line not heard yet (it may come repaired): kept for it.
    early_.push_back({{reaction.to, reaction.item}, protocol::Reacted{from, reaction.code}});
    while (early_.size() > MAX_EARLY_REACTIONS) early_.pop_front();
}

void BroadcastChat::flushAcks(const std::string& mode, uint64_t nowMs)
{
    if (acksDue_.empty()) return;
    std::vector<std::vector<uint8_t>> frames;
    std::vector<protocol::Record> frame;
    size_t bytes = 1;
    for (const protocol::Record& ack : acksDue_)
    {
        size_t size = protocol::encodeRecord(ack).size();
        if (!frame.empty() && bytes + size > protocol::MAX_BROADCAST_FRAME)
        {
            frames.push_back(protocol::encodeFrame(frame));
            frame.clear();
            bytes = 1;
        }
        frame.push_back(ack);
        bytes += size;
    }
    frames.push_back(protocol::encodeFrame(frame));
    const std::string label = acksDue_.size() == 1 ? std::string("ack") : std::to_string(acksDue_.size()) + " acks";
    acksDue_.clear();
    frames.erase(std::remove_if(frames.begin(), frames.end(), [](const std::vector<uint8_t>& f) { return f.empty(); }),
                 frames.end());
    uint64_t id = broadcast(frames, mode, nowMs, label);
    if (id != 0) ackBursts_.insert(id);
}

// Its Ack goes as soon as the station decodes it, in a burst of one
// codeword or so: a listening window's worth (docs/PLAN.md, "Windows in
// Data2G terms") lets it in before a file's next burst. A mode we can't
// time holds nothing beyond the usual gap.
void BroadcastChat::holdForAck(const Message& m, uint64_t nowMs)
{
    link::ModeInfo info = findMode(link_.modes(), m.mode, link_.groupMode());
    if (!info.valid()) return;
    hold(m.id, nowMs + listeningWindowMs(info), nowMs);
}

void BroadcastChat::hold(uint64_t id, uint64_t untilMs, uint64_t nowMs)
{
    ackHolds_[id] = untilMs;
    uint64_t until = 0;
    for (auto it = ackHolds_.begin(); it != ackHolds_.end();)
    {
        if (it->second <= nowMs)
        {
            it = ackHolds_.erase(it);
            continue;
        }
        until = std::max(until, it->second);
        ++it;
    }
    link_.holdYielding(until, nowMs);
}

void BroadcastChat::ackHeard(uint64_t id, uint64_t nowMs)
{
    if (ackHolds_.erase(id) == 0) return;
    uint64_t until = 0;
    for (const auto& h : ackHolds_)
    {
        if (h.second > nowMs) until = std::max(until, h.second);
    }
    link_.holdYielding(until, nowMs);
}

void BroadcastChat::setLog(BroadcastLog* log, uint64_t nowMs)
{
    log_ = log;
    if (log_ == nullptr) return;
    std::deque<Message> rows;
    for (Message& m : log_->load(MAX_MESSAGES))
    {
        m.id = nextId_++;
        // Heard again (a station re-sending it), it isn't listed twice.
        if (m.kind == Message::Kind::Text && !m.ours) seen_.insert(ItemKey{m.from, m.seq, m.time});
        rows.push_back(std::move(m));
    }
    for (Message& m : messages_) rows.push_back(std::move(m));
    messages_ = std::move(rows);
    trim();
    log_->began(nowMs);
    for (const Message& m : messages_)
    {
        if (!m.past && m.kind != Message::Kind::Rule) log_->added(m);
    }
    version_++;
}

void BroadcastChat::closeLog(uint64_t nowMs)
{
    if (log_ == nullptr) return;
    log_->ended(nowMs);
    log_ = nullptr;
}

void BroadcastChat::logState(const Message& m)
{
    if (log_ != nullptr && !m.past) log_->settled(m);
}

std::deque<Message>::iterator BroadcastChat::placeFor(const Message& m)
{
    // Numbers wrap after 0xFFFFFF: later means up to half the range on.
    auto later = [&m](const Message& o) {
        uint32_t ahead = (o.seq - m.seq) & 0xFFFFFF;
        return o.kind == Message::Kind::Text && !o.past && o.from == m.from && ahead != 0 && ahead < 0x800000;
    };
    auto at = std::find_if(messages_.begin(), messages_.end(), later);
    if (at == messages_.end()) return at;
    // Up past the others' lines made after it, as far as the sender's
    // line before it, the rule, or the past.
    while (at != messages_.begin())
    {
        const Message& above = *std::prev(at);
        if (above.past || above.kind == Message::Kind::Rule || above.from == m.from || above.time <= m.time) break;
        --at;
    }
    return at;
}

void BroadcastChat::trim()
{
    while (messages_.size() > MAX_MESSAGES
           || (!messages_.empty() && messages_.front().kind == Message::Kind::Rule))
    {
        records_.erase(messages_.front().id);
        messages_.pop_front();
    }
}

} // namespace broadcast
} // namespace ragtime
