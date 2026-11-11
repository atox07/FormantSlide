#include "MidiFile.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace {
using Byte = uint8_t;
bool readBig16(const std::vector<Byte>& b, size_t& i, uint16_t& value) {
    if (i + 2 > b.size()) return false;
    value = static_cast<uint16_t>((b[i] << 8) | b[i + 1]); i += 2; return true;
}
bool readBig32(const std::vector<Byte>& b, size_t& i, uint32_t& value) {
    if (i + 4 > b.size()) return false;
    value = (uint32_t(b[i]) << 24) | (uint32_t(b[i + 1]) << 16) |
            (uint32_t(b[i + 2]) << 8) | uint32_t(b[i + 3]); i += 4; return true;
}
bool varlen(const std::vector<Byte>& b, size_t& i, size_t end, uint32_t& value) {
    value = 0;
    for (int x = 0; x < 4; ++x) {
        if (i >= end) return false;
        Byte c = b[i++]; value = (value << 7) | (c & 0x7f);
        if (!(c & 0x80)) return true;
    }
    return false;
}
struct RawEvent { uint32_t tick; int channel; int note; bool on; };
}

bool MidiFile::load(const std::wstring& path, std::string& error) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) { error = "Cannot open MIDI file"; return false; }
    const std::vector<Byte> data((std::istreambuf_iterator<char>(in)), {});
    if (data.size() < 14 || data.size() > 32 * 1024 * 1024 ||
        std::string(reinterpret_cast<const char*>(data.data()), 4) != "MThd") {
        error = "Invalid MIDI header"; return false;
    }
    size_t pos = 4;
    uint32_t headerSize = 0;
    uint16_t format = 0, numTracks = 0, division = 0;
    if (!readBig32(data, pos, headerSize) || headerSize < 6 ||
        !readBig16(data, pos, format) || !readBig16(data, pos, numTracks) ||
        !readBig16(data, pos, division) || (division & 0x8000) || division == 0 ||
        format > 1 || numTracks == 0 || numTracks > 512 || 8u + headerSize > data.size()) {
        error = "Unsupported MIDI file (need format 0/1 and PPQ timing)"; return false;
    }
    pos = 8 + headerSize;
    std::vector<RawEvent> events;
    for (uint16_t track = 0; track < numTracks; ++track) {
        if (pos + 8 > data.size() ||
            std::string(reinterpret_cast<const char*>(data.data() + pos), 4) != "MTrk") {
            error = "Invalid MIDI track"; return false;
        }
        pos += 4;
        uint32_t size = 0;
        if (!readBig32(data, pos, size) || size > data.size() - pos) {
            error = "Truncated MIDI track"; return false;
        }
        const size_t end = pos + size;
        uint32_t tick = 0;
        Byte running = 0;
        while (pos < end) {
            uint32_t dt = 0;
            if (!varlen(data, pos, end, dt) || dt > UINT32_MAX - tick || pos >= end) {
                error = "Invalid MIDI event"; return false;
            }
            tick += dt;
            Byte status = data[pos];
            if (status & 0x80) {
                ++pos;
                if (status < 0xf0) running = status;
                else running = 0;
            } else {
                if (!running) { error = "Invalid MIDI running status"; return false; }
                status = running;
            }
            if (status == 0xff) {
                if (pos >= end) return false;
                ++pos; // meta type
                uint32_t len = 0;
                if (!varlen(data, pos, end, len) || len > end - pos) return false;
                pos += len;
            } else if (status == 0xf0 || status == 0xf7) {
                uint32_t len = 0;
                if (!varlen(data, pos, end, len) || len > end - pos) return false;
                pos += len;
            } else if (status >= 0x80 && status <= 0xef) {
                const Byte kind = status & 0xf0;
                const int required = kind == 0xc0 || kind == 0xd0 ? 1 : 2;
                if (pos + required > end) return false;
                const int note = data[pos], velocity = required == 2 ? data[pos + 1] : 0;
                pos += required;
                if (kind == 0x90 || kind == 0x80)
                    events.push_back({tick, (status & 0x0f) + 1, note,
                                      kind == 0x90 && velocity != 0});
            } else { error = "Unsupported system MIDI event"; return false; }
        }
    }
    // Note-offs precede note-ons when two notes have the same tick.
    std::stable_sort(events.begin(), events.end(), [](const RawEvent& a, const RawEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.on < b.on;
    });
    std::array<std::array<std::vector<size_t>, 128>, 16> active;
    std::vector<MidiNoteLength> notes;
    for (const auto& e : events) {
        if (e.note < 0 || e.note > 127 || e.channel < 1 || e.channel > 16) continue;
        auto& stack = active[size_t(e.channel - 1)][size_t(e.note)];
        if (e.on) {
            stack.push_back(notes.size());
            notes.push_back({e.tick, 0, e.channel, e.note});
        } else if (!stack.empty()) {
            const size_t index = stack.front();
            stack.erase(stack.begin());
            notes[index].lengthTick = e.tick - notes[index].startTick;
        }
    }
    notes.erase(std::remove_if(notes.begin(), notes.end(), [](const MidiNoteLength& n) {
        return n.lengthTick == 0;
    }), notes.end());
    division_ = division;
    notes_ = std::move(notes);
    error.clear();
    return true;
}

int MidiFile::durationTicks(int channel, int note, double hostStartTick,
                            int hostPpq, int offsetTicks) const {
    if (hostPpq <= 0 || notes_.empty()) return -1;
    const double scale = double(hostPpq) / double(division_);
    const double tol = std::max(6.0, double(hostPpq) * 0.25); // 1/4 beat
    double closest = std::numeric_limits<double>::infinity();
    int result = -1;
    int totalMatches = 0;
    int uniqueLength = -1;
    for (const auto& n : notes_) {
        if (n.channel != channel || n.note != note) continue;
        const int len = std::max(1, int(std::lround(n.lengthTick * scale)));
        ++totalMatches;
        uniqueLength = len;
        const double distance = std::abs(double(n.startTick) * scale + offsetTicks - hostStartTick);
        if (distance < closest) { closest = distance; result = len; }
    }
    if (closest <= tol) return result;
    // A single matching slide can still be used for a preview that has no host position.
    return totalMatches == 1 ? uniqueLength : -1;
}
