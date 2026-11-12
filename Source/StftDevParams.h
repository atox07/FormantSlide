#pragma once
#include <atomic>

// Parametri "dev" condivisi tra editor (thread GUI) e motore STFT (thread audio).
// Nessun limite imposto: l'editor scrive, StftVoice::render() li legge.
// quefrencyMs e' in MILLISECONDI (2.0 = 0.002 s).
struct StftDevParams {
    std::atomic<float>    envSmooth      {0.30f};
    std::atomic<float>    envSmoothSlide {0.05f};
    std::atomic<float>    maxGain        {6.0f};
    std::atomic<double>   quefrencyMs    {2.0};
    // Letti SOLO quando nasce una voce (nuova nota): ricreano le bande.
    std::atomic<int>      fftLow         {2048};  // base a <=50 kHz, scala col sample rate
    std::atomic<int>      fftHigh        {512};
    std::atomic<double>   crossLowHz     {2200.0};
    std::atomic<double>   crossHighHz    {2800.0};
    std::atomic<unsigned> version        {1};   // +1 a ogni modifica

    static StftDevParams& instance() {
        static StftDevParams p;
        return p;
    }
};
