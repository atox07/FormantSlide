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
    std::atomic<unsigned> version        {1};   // +1 a ogni modifica

    static StftDevParams& instance() {
        static StftDevParams p;
        return p;
    }
};

// Rilascio progressivo della preservazione formanti (valori finali).
// r = cap * onset, con
//   onset : 0 -> 1 quando la nota sale (shift in semitoni OPPURE f0 in uscita in Hz)
//   cap   : sale da kLow (nota alta) a kHigh (nota estremamente alta)
// Le note basse restano a r = 0: suono invariato.
struct StftRelax {
    static constexpr double kLow  = 0.10;   // tetto per note alte "normali" (quasi impercettibile)
    static constexpr double kHigh = 0.75;   // tetto per note estremamente alte (0.70 - 0.80)

    // Inizio del rilascio (onset)
    static constexpr double kShiftOn0 = 6.0,    kShiftOn1 = 12.0;    // semitoni di shift
    static constexpr double kHzOn0    = 1100.0, kHzOn1    = 1900.0;  // f0 in uscita

    // Zona estrema: il tetto passa da kLow a kHigh
    static constexpr double kShiftX0  = 12.0,   kShiftX1  = 18.0;
    static constexpr double kHzX0     = 1900.0, kHzX1     = 3000.0;
};
