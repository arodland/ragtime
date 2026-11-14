// KISS, command-line and mode formats for talking to data2g-host. Ported
// from Glissando's Data2GLink; see the header.
#include "ragtime/link/Data2GLink.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace ragtime
{
namespace link
{

namespace
{

constexpr uint8_t FEND = 0xC0;
constexpr uint8_t FESC = 0xDB;
constexpr uint8_t TFEND = 0xDC;
constexpr uint8_t TFESC = 0xDD;

std::string upper(std::string text)
{
    for (char& c : text) c = (char)std::toupper((unsigned char)c);
    return text;
}

std::vector<std::string> words(const std::string& line)
{
    std::istringstream in(line);
    std::vector<std::string> out;
    std::string word;
    while (in >> word) out.push_back(word);
    return out;
}

bool toInt(const std::string& text, int& out)
{
    if (text.empty()) return false;
    char* end = nullptr;
    long value = std::strtol(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    out = (int)value;
    return true;
}

bool toOffset(const std::string& text, uint64_t& out)
{
    if (text.empty() || text.size() > 18) return false;
    for (char c : text)
    {
        if (c < '0' || c > '9') return false;
    }
    out = std::strtoull(text.c_str(), nullptr, 10);
    return true;
}

bool toDouble(const std::string& text, double& out)
{
    if (text.empty()) return false;
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end != nullptr && *end == '\0';
}

// An SNR at the end of a report: a finite number with no letter in it, so
// never a callsign (a callsign always has a letter) nor "inf" or "nan".
std::optional<double> trailingSnr(const std::string& word)
{
    if (std::any_of(word.begin(), word.end(), [](char c) { return std::isalpha((unsigned char)c) != 0; }))
        return std::nullopt;
    double dB = 0.0;
    if (!toDouble(word, dB) || !std::isfinite(dB)) return std::nullopt;
    return dB;
}

void appendEscaped(std::vector<uint8_t>& out, uint8_t b)
{
    if (b == FEND)
    {
        out.push_back(FESC);
        out.push_back(TFEND);
    }
    else if (b == FESC)
    {
        out.push_back(FESC);
        out.push_back(TFESC);
    }
    else
    {
        out.push_back(b);
    }
}

std::vector<uint8_t> kissFrame(uint8_t commandByte, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> out;
    out.reserve(body.size() + 8);
    out.push_back(FEND);
    appendEscaped(out, commandByte);
    for (uint8_t b : body) appendEscaped(out, b);
    out.push_back(FEND);
    return out;
}

} // namespace

std::vector<uint8_t> kissEncode(int port, const std::vector<uint8_t>& data)
{
    return kissFrame((uint8_t)(((port & 0x0F) << 4) | KISS_DATA), data);
}

std::vector<uint8_t> kissEncodeAckMode(int port, uint16_t tag, const std::vector<uint8_t>& data)
{
    // Sized once and filled in place: GCC 14 at -O3 takes reserve() then
    // push_back() for a free of a pointer off the heap (-Wfree-nonheap-object).
    std::vector<uint8_t> body(data.size() + 2);
    body[0] = (uint8_t)(tag >> 8);
    body[1] = (uint8_t)(tag & 0xFF);
    std::copy(data.begin(), data.end(), body.begin() + 2);
    return kissFrame((uint8_t)(((port & 0x0F) << 4) | KISS_ACKMODE), body);
}

std::vector<uint8_t> kissEncodeCommand(int port, uint8_t command, uint8_t value)
{
    return kissFrame((uint8_t)(((port & 0x0F) << 4) | (command & 0x0F)), {value});
}

std::vector<uint8_t> kissEncodeSetHardware(int port, const std::vector<uint8_t>& payload)
{
    return kissFrame((uint8_t)(((port & 0x0F) << 4) | KISS_SETHARDWARE), payload);
}

std::vector<uint8_t> kissEncodeRefs(bool on)
{
    std::string text = on ? "REFS ON" : "REFS OFF";
    return kissEncodeSetHardware(0, std::vector<uint8_t>(text.begin(), text.end()));
}

void KissDecoder::feed(const uint8_t* bytes, size_t length, std::vector<KissFrame>& framesOut)
{
    for (size_t i = 0; i < length; i++)
    {
        uint8_t b = bytes[i];
        if (b == FEND)
        {
            if (inFrame_ && current_.size() > 1)
            {
                KissFrame frame;
                frame.port = current_[0] >> 4;
                frame.command = current_[0] & 0x0F;
                frame.payload.assign(current_.begin() + 1, current_.end());
                if (frame.command == KISS_DATA || frame.command == KISS_ACKMODE || frame.command == KISS_SETHARDWARE)
                    framesOut.push_back(frame);
            }
            current_.clear();
            escaped_ = false;
            inFrame_ = true;
            continue;
        }
        if (!inFrame_) continue;

        if (escaped_)
        {
            escaped_ = false;
            if (b == TFEND) current_.push_back(FEND);
            else if (b == TFESC) current_.push_back(FESC);
            else current_.push_back(b); // a protocol error; keep the byte as sent
            continue;
        }
        if (b == FESC)
        {
            escaped_ = true;
            continue;
        }
        current_.push_back(b);
    }
}

void KissDecoder::reset()
{
    current_.clear();
    escaped_ = false;
    inFrame_ = false;
}

int burstDataBytes(const std::vector<int>& frameBytes, int bytesPerCodeword)
{
    int bytes = 0;
    for (int length : frameBytes)
    {
        if (bytesPerCodeword > 0 && bytes % bytesPerCodeword == 0) bytes++;
        bytes += framedBytes(length);
    }
    return bytes;
}

double ModeInfo::burstSeconds(const std::vector<int>& frameBytes) const
{
    if (!valid()) return 0.0;

    int bytes = burstDataBytes(frameBytes, bytesPerCodeword);
    int codewords = 1 + (bytes + bytesPerCodeword - 1) / bytesPerCodeword;
    codewords = std::min(codewords, maxCodewords);
    if (maxCodewords <= 1) return secondsAtOne;
    return secondsAtOne + (secondsAtMax - secondsAtOne) * (codewords - 1) / (maxCodewords - 1);
}

bool isAckMode(const std::string& mode)
{
    return mode.compare(0, 4, "ack-") == 0;
}

const std::vector<ModeInfo>& knownModes()
{
    // From a 2400 Hz data2g-host's MODES (Data2G 8ffe15c, 2026-10-07, as
    // Glissando measured them).
    static const std::vector<ModeInfo> modes = {
        {"n4-qpsk-r1/5", 200, 22, 32, 4.14, 111.27, {}},
        {"fsk16r25-r1/2", 500, 58, 9, 4.98, 89.22, {}},
        {"fsk16r25-r1/3", 500, 38, 9, 4.98, 89.22, {}},
        {"fsk8r50-r1/2", 500, 58, 9, 3.62, 59.62, {}},
        {"fsk8r50-r1/3", 500, 38, 9, 3.62, 59.62, {}},
        {"n10-16qam-r1/2", 500, 121, 32, 2.12, 46.76, {}},
        {"n10-qpsk-r1/2", 500, 60, 32, 2.12, 46.76, {}},
        {"n10-qpsk-r1/3", 500, 40, 32, 2.12, 46.76, {}},
        {"n10-qpsk-r1/5", 500, 23, 32, 2.12, 46.76, {}},
        {"qpsk-r1/3", 1200, 76, 64, 1.78, 74.36, {}},
        {"fsk32r62-r1/3", 2300, 38, 9, 2.42, 29.30, {}},
        {"w48-16qam-r1/2", 2400, 236, 64, 1.21, 37.50, {}},
        {"w48-qpsk-r1/2", 2400, 116, 64, 1.21, 37.50, {}},
    };
    return modes;
}

std::vector<ModeInfo> narrowModes(const std::vector<ModeInfo>& offered)
{
    const std::vector<ModeInfo>& modes = offered.empty() ? knownModes() : offered;
    std::vector<ModeInfo> narrow;
    for (const ModeInfo& mode : modes)
    {
        if (mode.bandwidthHz > 0 && mode.bandwidthHz <= NARROW_MODE_MAX_HZ) narrow.push_back(mode);
    }
    return narrow;
}

double handChannelHz(int channel)
{
    double off = 30.0 + 60.0 * (channel / 2);
    return channel % 2 ? 1500.0 + off : 1500.0 - off;
}

int handChannelsWithin(int widthHz)
{
    // A Hand's 8 tones span 50 Hz: it fits while its far edge is inside.
    int n = 0;
    for (int c = 0; c < HAND_CHANNELS; c++)
    {
        if (std::fabs(handChannelHz(c) - 1500.0) + 25.0 <= widthHz / 2.0) n++;
    }
    return n;
}

namespace
{

bool hexDigits(const std::string& text, size_t most)
{
    if (text.empty() || text.size() > most) return false;
    return std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit((unsigned char)c) != 0; });
}

bool refWords(const std::vector<std::string>& w, HandRef& ref)
{
    if (w.size() != 5 || upper(w[0]) != "REF" || !toInt(w[1], ref.id) || ref.id < 1) return false;
    return toDouble(w[2], ref.endS) && toDouble(w[3], ref.offsetHz) && toDouble(w[4], ref.snr)
           && std::isfinite(ref.endS) && std::isfinite(ref.offsetHz) && std::isfinite(ref.snr);
}

} // namespace

std::optional<HandRef> parseHandRef(const std::string& text)
{
    HandRef ref;
    if (!refWords(words(text), ref)) return std::nullopt;
    return ref;
}

std::string handDelayText(double seconds)
{
    long long ms = std::llround(std::max(0.0, std::min(seconds, 1e6)) * 1000.0);
    std::string frac = std::to_string(ms % 1000);
    return std::to_string(ms / 1000) + "." + std::string(3 - frac.size(), '0') + frac;
}

std::string handSendLine(int ref, int tag, double delayS, int channel, uint64_t payload)
{
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)(payload & HAND_PAYLOAD_MASK));
    return "HAND SEND " + std::to_string(ref) + " " + std::to_string(tag) + " " + handDelayText(delayS) + " "
           + std::to_string(channel) + " " + hex;
}

std::string handListenLine(double delayS, int tag, uint16_t ack, const std::vector<int>& channels)
{
    char hex[5];
    std::snprintf(hex, sizeof hex, "%04x", (unsigned)ack);
    std::string line = "HAND LISTEN " + handDelayText(delayS) + " " + std::to_string(tag) + " " + hex;
    for (int c : channels) line += " " + std::to_string(c);
    return line;
}

CommandEvent parseCommandLine(const std::string& line)
{
    CommandEvent event;
    std::vector<std::string> w = words(line);
    if (w.empty()) return event;

    std::string verb = upper(w[0]);
    std::string arg1 = w.size() > 1 ? upper(w[1]) : std::string();

    if (verb == "OK" && w.size() == 1)
    {
        event.type = CommandEvent::Type::Ok;
    }
    else if (verb == "WRONG")
    {
        event.type = CommandEvent::Type::Wrong;
    }
    else if ((verb == "PTT" || verb == "BUSY") && w.size() == 2 && (arg1 == "ON" || arg1 == "OFF"))
    {
        event.type = verb == "PTT" ? CommandEvent::Type::Ptt : CommandEvent::Type::Busy;
        event.on = arg1 == "ON";
    }
    else if (verb == "MODE" && w.size() == 2)
    {
        event.type = CommandEvent::Type::Mode;
        event.text = w[1];
    }
    else if (verb == "MODE" && w.size() == 7)
    {
        ModeInfo mode;
        mode.name = w[1];
        double bandwidth = 0.0;
        if (toDouble(w[2], bandwidth) && toInt(w[3], mode.bytesPerCodeword) && toInt(w[4], mode.maxCodewords) &&
            toDouble(w[5], mode.secondsAtOne) && toDouble(w[6], mode.secondsAtMax))
        {
            mode.bandwidthHz = (int)std::lround(bandwidth);
            event.type = CommandEvent::Type::ModeLine;
            event.modeInfo = mode;
        }
    }
    else if (verb == "CHANNELS" && w.size() > 1)
    {
        event.type = CommandEvent::Type::Channels;
        event.words.assign(w.begin() + 1, w.end());
    }
    else if (verb == "THRESHOLD" && w.size() > 2)
    {
        event.text = w[1];
        for (size_t i = 2; i < w.size(); i++)
        {
            double dB = 0.0;
            if (w[i] == "-")
                event.dBs.push_back(std::nullopt);
            else if (toDouble(w[i], dB))
                event.dBs.push_back(dB);
            else
                return event;
        }
        event.type = CommandEvent::Type::ModeThreshold;
    }
    else if (verb == "BCAST" && w.size() == 3 && arg1 == "PORT" && toInt(w[2], event.number))
    {
        event.type = CommandEvent::Type::BcastPort;
    }
    else if (verb == "BCAST" && w.size() >= 3 && toInt(w[1], event.number))
    {
        std::string what = upper(w[2]);
        if (what == "HEARD")
        {
            // BCAST n HEARD [call] [snr]: the SNR, when there, is the last word.
            event.type = CommandEvent::Type::BcastHeard;
            size_t end = w.size();
            if (end > 3 && (event.snr = trailingSnr(w[end - 1]))) end--;
            if (end > 3) event.text = w[3];
        }
        else if ((what == "LOST" || what == "DROPPED") && w.size() == 4 && toInt(w[3], event.count))
        {
            event.type = what == "LOST" ? CommandEvent::Type::BcastLost : CommandEvent::Type::BcastDropped;
        }
    }
    else if (verb == "BCAST" && w.size() >= 5 && w[1] == "*" && upper(w[2]) == "MISSED" && toInt(w[4], event.count))
    {
        // BCAST * MISSED submode n_cw [snr]
        event.type = CommandEvent::Type::BcastMissed;
        event.text = w[3];
        if (w.size() > 5) event.snr = trailingSnr(w[5]);
    }
    else if (verb == "SN" && w.size() == 2 && (event.snr = trailingSnr(w[1])))
    {
        event.type = CommandEvent::Type::Sn;
    }
    else if (verb == "CONNECTED" && w.size() >= 3)
    {
        event.type = CommandEvent::Type::Connected;
        event.text = upper(w[1]);
        event.peer = upper(w[2]);
        if (w.size() > 3) toInt(w[3], event.number);
    }
    else if (verb == "DISCONNECTED")
    {
        event.type = CommandEvent::Type::Disconnected;
        CommandEvent::Disconnect& d = event.disconnect;
        size_t end = w.size();
        if (end > 2 && upper(w[end - 1]) == "UNCONFIRMED")
        {
            d.unconfirmed = true;
            end--;
        }
        if (end > 1) d.reason = arg1;
        for (size_t i = 2; i < end; i++) d.detail += (d.detail.empty() ? "" : " ") + w[i];
    }
    else if ((verb == "TXPOWER" || verb == "TIMEOUT") && w.size() == 2 && toDouble(w[1], event.value)
             && std::isfinite(event.value))
    {
        event.type = verb == "TXPOWER" ? CommandEvent::Type::TxPower : CommandEvent::Type::Timeout;
    }
    else if (verb == "REF" && refWords(w, event.handRef))
    {
        event.type = CommandEvent::Type::Ref;
    }
    else if (verb == "HAND" && w.size() == 3 && (arg1 == "SENT" || arg1 == "DROPPED") && toInt(w[2], event.number))
    {
        event.type = arg1 == "SENT" ? CommandEvent::Type::HandSent : CommandEvent::Type::HandDropped;
    }
    else if (verb == "HAND" && w.size() == 3 && arg1 == "SHORT" && toDouble(w[2], event.value)
             && std::isfinite(event.value))
    {
        event.type = CommandEvent::Type::HandShort;
    }
    else if (verb == "HAND" && w.size() == 2 && arg1 == "END")
    {
        event.type = CommandEvent::Type::HandEnd;
    }
    else if (verb == "HAND" && w.size() == 5 && toInt(w[1], event.number) && event.number >= 0
             && hexDigits(w[2], 16) && (event.snr = trailingSnr(w[3])) && toDouble(w[4], event.value)
             && std::isfinite(event.value))
    {
        // HAND c hex16 snr dt
        event.type = CommandEvent::Type::Hand;
        event.handPayload = std::strtoull(w[2].c_str(), nullptr, 16) & HAND_PAYLOAD_MASK;
    }
    else if (verb == "BUFFER" && w.size() == 2 && toInt(w[1], event.count))
    {
        event.type = CommandEvent::Type::Buffer;
    }
    else if (verb == "CQFRAME")
    {
        event.type = CommandEvent::Type::Cqframe;
    }
    else if (verb == "CLEANTXBUFFER" && w.size() >= 2)
    {
        using Result = CommandEvent::Clean::Result;
        CommandEvent::Clean& c = event.clean;
        size_t want = 0;
        if (arg1 == "OK")
        {
            c.result = Result::Ok;
            want = 4;
        }
        else if (arg1 == "SETTLED")
        {
            c.result = Result::Settled;
            want = 3;
        }
        else if (arg1 == "LATE" || arg1 == "BUFFEREMPTY" || arg1 == "FAILED")
        {
            c.result = arg1 == "LATE" ? Result::Late : arg1 == "FAILED" ? Result::Failed : Result::Empty;
        }
        else
        {
            return event;
        }
        // A bare OK is a host without DATA2G ON: no offsets.
        for (size_t i = 2; i < w.size() && c.offsets.size() < want; i++)
        {
            uint64_t value = 0;
            if (!toOffset(w[i], value)) break;
            c.offsets.push_back(value);
        }
        if (c.offsets.size() != want && !(c.result == Result::Ok && w.size() == 2)) return event;
        c.unknown = c.result == Result::Settled && w.size() > 5 && upper(w[5]) == "UNKNOWN";
        event.type = CommandEvent::Type::Clean;
    }
    return event;
}

void LineSplitter::feed(const char* bytes, size_t length, std::vector<std::string>& linesOut)
{
    for (size_t i = 0; i < length; i++)
    {
        char c = bytes[i];
        if (c == '\r' || c == '\n')
        {
            if (!current_.empty()) linesOut.push_back(current_);
            current_.clear();
        }
        else if (current_.size() < 1024)
        {
            current_.push_back(c);
        }
    }
}

std::string commandCallsign(const std::string& callsign)
{
    std::string out;
    for (char c : upper(callsign))
    {
        if (out.size() == 10) break;
        if (std::isalnum((unsigned char)c) || c == '/' || c == '-') out.push_back(c);
    }
    return out;
}

} // namespace link
} // namespace ragtime
