#pragma once
#include <atomic>

// Parametri del motore STFT (valori finali, tarati a orecchio).
// Nessuna UI li modifica piu': per cambiarli si cambiano qui e si ricompila.
// quefrencyMs e' in MILLISECONDI (2.0 = 0.002 s).
struct StftDevParams {
    std::atomic<float>    envSmooth      {0.30f};
    std::atomic<float>    envSmoothSlide {0.566f};
    std::atomic<float>    maxGain        {9.118f};
    std::atomic<double>   quefrencyMs    {2.0};
    // Letti SOLO quando nasce una voce (nuova nota): ricreano le bande.
    std::atomic<int>      fftLow         {2048};  // base a <=50 kHz, scala col sample rate
    std::atomic<int>      fftHigh        {4096};
    std::atomic<double>   crossLowHz     {2200.0};
    std::atomic<double>   crossHighHz    {2800.0};
    // Slider della GUI "FORMANT RELEASE" (scritti dall'editor, letti dal render).
    std::atomic<float>    formantGuard     {1.0f};     // 0..2
    std::atomic<float>    highNoteLimitHz  {2000.0f};  // 500..4000
    std::atomic<float>    highNoteRelaxMax {0.75f};    // 0..1
    std::atomic<unsigned> version        {1};   // +1 a ogni modifica

    static StftDevParams& instance() {
        static StftDevParams p;
        return p;
    }
};

// Rilascio progressivo della preservazione formanti: costanti fisse.
// Soglia in Hz e rilascio massimo oltre soglia arrivano dagli slider della GUI
// (Settings -> StftVoice::setHighNoteLimit / setHighNoteRelaxMax).
struct StftRelax {
    static constexpr double kLow      = 0.10;  // rilascio base sotto la soglia di pericolo
    static constexpr double kShiftOn0 = 6.0;   // |shift| in semitoni: inizio rilascio
    static constexpr double kShiftOn1 = 12.0;  // |shift| in semitoni: rilascio pieno
};
