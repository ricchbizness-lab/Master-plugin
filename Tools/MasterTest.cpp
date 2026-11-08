// Outil de test en ligne de commande : masterise un WAV et affiche le rapport.
//   MasterTest in.wav out.wav [style 0-4] [lufs] [ref.wav]
#include "../Source/MasterEngine.h"
#include "../Source/Dsp.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include <random>
#include <juce_dsp/juce_dsp.h>

using namespace master;

static bool loadWav (const juce::File& f, Stereo& s)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r) return false;
    juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
    s.fs = r->sampleRate;
    s.l.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
    s.r.assign (b.getReadPointer (b.getNumChannels() > 1 ? 1 : 0), b.getReadPointer (b.getNumChannels() > 1 ? 1 : 0) + b.getNumSamples());
    return true;
}

static void print (const char* tag, const Analysis& a)
{
    std::printf ("%-7s LUFS %6.2f  LRA %4.1f  TP %6.2f dBTP  peak %6.2f  PLR %5.1f  corr %5.2f  S/M %6.1f dB  clip %d\n",
                 tag, a.lufs, a.lra, a.truePeakDb, a.samplePeakDb, a.plr(), a.correlation, a.sideMidDb, a.clippedSamples);
}


// ---- Auto-tests des briques DSP
static int selfTest()
{
    int fails = 0;
    const auto check = [&] (const char* name, bool ok, double v) { std::printf ("  [%s] %s (%.3f)\n", ok ? "OK " : "KO!", name, v); if (! ok) ++fails; };
    const double fs = 44100.0; const int n = (int) (fs * 20);
    std::mt19937 rng (7); std::normal_distribution<float> nd (0.0f, 0.1f);
    std::vector<float> a ((size_t) n), b ((size_t) n);
    for (int i = 0; i < n; ++i) { a[(size_t) i] = nd (rng); b[(size_t) i] = nd (rng); }

    // 1. LUFS : sinus 1 kHz stéréo à -20 dBFS crête => -20 + 0 - 0.691+... référence EBU : -23 dBFS RMS sinus => -23 LUFS
    { std::vector<float> s ((size_t) n); for (int i = 0; i < n; ++i) s[(size_t) i] = 0.1f * (float) std::sin (2 * kPi * 1000.0 * i / fs) * 1.41421356f * 0.7071f * 1.0f;
      // amplitude crête 0.1 => RMS -23.01 dBFS par canal => LUFS ~ -23.0 (K-weighting ~0 dB à 1 kHz, 2 canaux)
      const auto l = measureLoudness (s.data(), s.data(), n, fs); check ("LUFS sinus 1 kHz crête 0.1 sur 2 canaux = -20.0 (réf. BS.1770)", std::abs (l.integrated + 20.0) < 0.1, l.integrated); }
    // 2. true-peak : sinus fs/4 déphasé, crête réelle 1.0 mais échantillons à 0.7071
    { std::vector<float> s ((size_t) n); for (int i = 0; i < n; ++i) s[(size_t) i] = (float) std::sin (2 * kPi * (fs / 4) * i / fs + kPi / 4);
      float sp = 0; for (float v : s) sp = std::max (sp, std::abs (v));
      const float tp = truePeakLinear (s.data(), s.data(), n); check ("true-peak détecte la crête inter-échantillon (0.707 -> ~1.0)", tp > 0.98f && tp < 1.02f && sp < 0.72f, tp); }
    // 3. FIR identité (noyau delta) => signal inchangé
    { std::vector<float> h (8191, 0.0f); h[4095] = 1.0f; auto x = a, y = b; firZeroLatencyStereo (x.data(), y.data(), n, h);
      double e = 0; for (int i = 0; i < n; ++i) e = std::max (e, (double) std::abs (x[(size_t) i] - a[(size_t) i])); check ("convolution FIR identité (erreur max)", e < 1e-5, e); }
    // 4. EQ : courbe +6 dB à 1 kHz (autour), 0 ailleurs => mesure sur bruit blanc
    { std::vector<float> curve ((size_t) kBands, 0.0f); for (int k = 0; k < kBands; ++k) { const double f = bandCentre (k); curve[(size_t) k] = (float) (6.0 * std::exp (-0.5 * std::pow (std::log2 (f / 1000.0) / 0.7, 2))); }
      const auto h = designKernel (curve, fs, 22.0); auto x = a, y = a; const auto before = bandSpectrum (x.data(), y.data(), n, fs);
      firZeroLatencyStereo (x.data(), y.data(), n, h); const auto after = bandSpectrum (x.data(), y.data(), n, fs);
      double worst = 0; for (int k = 0; k < kBands; ++k) { if (bandCentre (k) < 60 || bandCentre (k) > 16000) continue; worst = std::max (worst, (double) std::abs ((after[(size_t) k] - before[(size_t) k]) - curve[(size_t) k])); }
      check ("EQ phase linéaire : écart max à la courbe demandée (dB, 60 Hz-16 kHz)", worst < 0.6, worst);
      int k1k = 0; for (int k = 0; k < kBands; ++k) if (std::abs (bandCentre (k) - 1000.0) < std::abs (bandCentre (k1k) - 1000.0)) k1k = k;
      check ("EQ : gain mesuré à 1 kHz ~ +6 dB", std::abs ((after[(size_t) k1k] - before[(size_t) k1k]) - 6.0) < 0.5, after[(size_t) k1k] - before[(size_t) k1k]); }
    // 5. Crossover LR4 3 voies avec compensation de phase : somme plate
    { juce::dsp::ProcessSpec spec { fs, 512, 2 }; juce::dsp::LinkwitzRileyFilter<float> x1, x2, x2b;
      for (auto* f : { &x1, &x2, &x2b }) { f->prepare (spec); f->reset(); } x1.setCutoffFrequency (140.0f); x2.setCutoffFrequency (3000.0f); x2b.setCutoffFrequency (3000.0f);
      std::vector<float> o ((size_t) n);
      for (int i = 0; i < n; ++i) { float lo, rest, mid, hi, la, lb, m2, h2; x1.processSample (0, a[(size_t) i], lo, rest); x2.processSample (0, rest, mid, hi); x2b.processSample (0, lo, la, lb); o[(size_t) i] = la + lb + mid + hi; (void) m2; (void) h2; }
      const auto s0 = bandSpectrum (a.data(), a.data(), n, fs), s1 = bandSpectrum (o.data(), o.data(), n, fs);
      double worst = 0; for (int k = 3; k < 29; ++k) worst = std::max (worst, (double) std::abs (s1[(size_t) k] - s0[(size_t) k])); check ("crossover 3 voies : somme plate (écart max dB)", worst < 0.3, worst); }
    // 6. Gain fixe : LUFS suit exactement le gain
    { std::vector<float> x = a; for (auto& v : x) v *= 2.0f; const auto l0 = measureLoudness (a.data(), a.data(), n, fs), l1 = measureLoudness (x.data(), x.data(), n, fs);
      check ("LUFS +6.02 dB pour un gain x2", std::abs ((l1.integrated - l0.integrated) - 6.0206) < 0.01, l1.integrated - l0.integrated); }
    std::printf (fails ? "ECHECS: %d\n" : "tous les tests DSP passent\n", fails);
    return fails;
}

int main (int argc, char** argv)
{
    if (argc > 1 && std::string (argv[1]) == "selftest") return selfTest();
    if (argc < 3) { std::printf ("usage: MasterTest in.wav out.wav [style] [lufs] [ref.wav]\n"); return 1; }
    Stereo in;
    if (! loadWav (juce::File (argv[1]), in)) { std::printf ("lecture impossible\n"); return 2; }
    Settings s = settingsForStyle (argc > 3 ? std::atoi (argv[3]) : 0);
    if (argc > 4) s.targetLufs = (float) std::atof (argv[4]);
    Analysis ref;
    if (argc > 5) { Stereo r; if (loadWav (juce::File (argv[5]), r)) { ref = analyse (r); s.reference = &ref; } }

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto before = analyse (in);
    Stereo out; Report rep;
    const bool ok = masterAudio (in, before, s, out, rep);
    const auto t1 = juce::Time::getMillisecondCounterHiRes();
    std::printf ("ok=%d  %.1f s de calcul pour %.1f s d'audio (%.0f Hz)  itérations limiteur: %d\n", (int) ok, (t1 - t0) / 1000.0, in.seconds(), in.fs, rep.iterations);
    if (! ok) return 3;
    print ("avant", rep.before); print ("après", rep.after);
    std::printf ("GR multibande moy.: %.2f / %.2f / %.2f dB   limiteur moy. %.2f dB max %.2f dB   gain avant limiteur %.2f dB\n",
                 rep.bandGrDb[0], rep.bandGrDb[1], rep.bandGrDb[2], rep.limiterAvgGrDb, rep.limiterMaxGrDb, rep.gainIntoLimiterDb);
    std::printf ("EQ (dB) :"); for (int b = 0; b < kBands; b += 3) std::printf (" %.0fHz:%+.1f", bandCentre (b), rep.eqCurveDb[(size_t) b]); std::printf ("\n");
    for (auto& w : rep.warnings) std::printf ("warning %d (%.2f)\n", w.code, w.value);

    juce::File of (argv[2]); of.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os (of.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> wr (wav.createWriterFor (os.get(), out.fs, 2, 24, {}, 0));
    if (wr) { os.release(); const float* ch[2] = { out.l.data(), out.r.data() }; juce::AudioBuffer<float> b (const_cast<float**> (ch), 2, out.size()); wr->writeFromAudioSampleBuffer (b, 0, out.size()); }
    return 0;
}
