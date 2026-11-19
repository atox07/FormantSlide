#pragma once
#include <atomic>

struct StftDevParams {
    // Slider della GUI "FORMANT RELEASE" (scritti dall'editor, letti dal render).
    std::atomic<float>    formantGuard     {0.0f};     // 0..2
    std::atomic<float>    highNoteLimitHz  {2000.0f};  // 500..4000
    std::atomic<float>    highNoteRelaxMax {0.75f};    // 0..1

    static StftDevParams& instance() {
        static StftDevParams p;
        return p;
    }
};
