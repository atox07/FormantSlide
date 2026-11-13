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
    // Rilascio progressivo preservazione formanti (letti live a ogni frame).
    std::atomic<float>    relaxMax       {0.30f};   // 0.30 = max 30%
    std::atomic<float>    relaxShiftStart{12.0f};   // semitoni di shift
    std::atomic<float>    relaxShiftEnd  {24.0f};
    std::atomic<float>    relaxHzStart   {900.0f};  // f0 in uscita
    std::atomic<float>    relaxHzEnd     {1700.0f};
    std::atomic<unsigned> version        {1};   // +1 a ogni modifica

    static StftDevParams& instance() {
        static StftDevParams p;
        return p;
    }
};
