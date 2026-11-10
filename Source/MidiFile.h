#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct MidiNoteLength {
    uint32_t startTick = 0;
    uint32_t lengthTick = 0;
    int channel = 1;
    int note = 60;
};

class MidiFile {
public:
    bool load(const std::wstring& filePath, std::string& error);
    int durationTicks(int channel, int note, double hostStartTick,
                      int hostPpq, int offsetTicks) const;
    size_t noteCount() const { return notes_.size(); }
    uint16_t ppq() const { return division_; }
private:
    uint16_t division_ = 480;
    std::vector<MidiNoteLength> notes_;
};
