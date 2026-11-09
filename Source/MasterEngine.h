#pragma once
#include <functional>
#include <string>
#include <vector>

namespace master
{
struct Stereo
{
    std::vector<float> l, r;
    double fs = 44100.0;
    int size() const { return (int) l.size(); }
    double seconds() const { return fs > 0 ? size() / fs : 0.0; }
};

struct Analysis
{
    bool valid = false;
    double lufs = -70.0, lra = 0.0, truePeakDb = -120.0, samplePeakDb = -120.0;
    float correlation = 1.0f;   // -1..1
    float sideMidDb = -60.0f;   // niveau Side relatif au Mid
    int clippedSamples = 0;
    double seconds = 0.0;
    std::vector<float> bands;   // énergie par tiers d'octave (dB relatif), kBands valeurs
    double plr() const { return truePeakDb - lufs; }
};

enum WarnCode { WarnAlreadyLoud, WarnClippedInput, WarnPhase, WarnNearMono, WarnHeavyLimiting, WarnTargetMissed, WarnShortFile, WarnSilent };
struct Warning { int code; float value; };

struct Style
{
    const char* nameFr; const char* nameEn;
    float loud, glue, punch, bright, width;   // valeurs par défaut des réglages
    float sat, bassDb, trebleDb, eqAmount, releaseMs;
};
constexpr int kNumStyles = 5;
const Style& style (int i);

struct Settings
{
    int style = 0;
    float targetLufs = -14.0f;
    float glue = 0.4f, punch = 0.5f, bright = 0.0f, width = 1.0f;  // glue/punch 0..1, bright -1..1, width 0..1.5
    float ceilingDb = -1.0f;
    int content = 0;                      // 0 = instrumental (beat), 1 = morceau avec voix
    const Analysis* reference = nullptr;  // si non nul : le spectre de référence remplace la courbe du style
};

struct Report
{
    Analysis before, after;
    std::vector<float> eqCurveDb;       // correction EQ appliquée (dB, par bande)
    std::vector<float> targetBands;     // courbe cible normalisée
    float bandGrDb[3] { 0, 0, 0 };      // réduction moyenne des 3 bandes
    float deEssGrDb = 0;                // réduction moyenne du de-esser (mode voix)
    float limiterAvgGrDb = 0, limiterMaxGrDb = 0;
    float gainIntoLimiterDb = 0;
    int iterations = 0;
    std::vector<Warning> warnings;
};

using ProgressFn = std::function<bool (float)>;   // renvoie false pour annuler

Settings settingsForStyle (int styleIndex);
std::vector<float> genericTargetNormalised();
std::vector<float> normaliseBandsPublic (const std::vector<float>& bands);
Analysis analyse (const Stereo& in, const ProgressFn& progress = nullptr);
bool masterAudio (const Stereo& in, const Analysis& before, const Settings& s, Stereo& out, Report& rep, const ProgressFn& progress = nullptr);
}
