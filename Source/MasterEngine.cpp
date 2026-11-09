#include "MasterEngine.h"
#include "Dsp.h"
#include <juce_dsp/juce_dsp.h>

namespace master
{
// ------------------------------------------------------------------ Styles
static const Style kStyleTable[kNumStyles] = {
    //  FR              EN               loud   glue  punch bright width  sat   bass  treble eq    rel
    { "Équilibré",   "Balanced",     -14.0f, 0.40f, 0.50f,  0.00f, 1.00f, 0.15f, 0.0f,  0.0f, 0.60f,  80.0f },
    { "Club",        "Club",          -9.0f, 0.60f, 0.60f,  0.15f, 1.10f, 0.25f, 1.5f,  0.5f, 0.60f,  60.0f },
    { "Chaud",       "Warm",         -12.0f, 0.50f, 0.40f, -0.15f, 1.00f, 0.45f, 1.0f, -1.0f, 0.50f, 120.0f },
    { "Transparent", "Transparent",  -14.0f, 0.25f, 0.60f,  0.00f, 1.00f, 0.00f, 0.0f,  0.0f, 0.30f, 100.0f },
    { "Rap / Trap",  "Rap / Trap",   -10.0f, 0.50f, 0.70f,  0.10f, 1.00f, 0.20f, 2.0f,  0.5f, 0.50f,  70.0f },
};
const Style& style (int i) { return kStyleTable[std::clamp (i, 0, kNumStyles - 1)]; }

Settings settingsForStyle (int i)
{
    const auto& st = style (i);
    Settings s;
    s.style = std::clamp (i, 0, kNumStyles - 1);
    s.targetLufs = st.loud; s.glue = st.glue; s.punch = st.punch; s.bright = st.bright; s.width = st.width;
    return s;
}

// Courbe cible générique (énergie par tiers d'octave, dB relatif à ~250 Hz), pente moyenne d'un master moderne.
static std::vector<float> genericTarget()
{
    static const double pts[][2] = {
        { 25, -16 }, { 31.5, -12 }, { 40, -8.5 }, { 50, -5.5 }, { 63, -3 }, { 80, -1 }, { 100, 0.5 }, { 125, 1 }, { 160, 1 }, { 200, 0.5 },
        { 250, 0 }, { 315, -0.5 }, { 400, -1 }, { 500, -1.5 }, { 630, -2.1 }, { 800, -2.6 }, { 1000, -3 }, { 1250, -3.6 }, { 1600, -4.2 },
        { 2000, -4.8 }, { 2500, -5.5 }, { 3150, -6.1 }, { 4000, -6.8 }, { 5000, -7.6 }, { 6300, -8.5 }, { 8000, -9.5 }, { 10000, -11.5 },
        { 12500, -14 }, { 16000, -18 }, { 20000, -25 } };
    const int np = (int) (sizeof (pts) / sizeof (pts[0]));
    std::vector<float> out ((size_t) kBands);
    for (int b = 0; b < kBands; ++b)
    {
        const double f = bandCentre (b);
        double v = pts[0][1];
        if (f >= pts[np - 1][0]) v = pts[np - 1][1];
        else for (int i = 0; i + 1 < np; ++i)
            if (f >= pts[i][0] && f <= pts[i + 1][0])
            {
                const double t = std::log (f / pts[i][0]) / std::log (pts[i + 1][0] / pts[i][0]);
                v = pts[i][1] + t * (pts[i + 1][1] - pts[i][1]);
                break;
            }
        out[(size_t) b] = (float) v;
    }
    return out;
}

std::vector<float> normaliseBandsPublic (const std::vector<float>& b) { return b.empty() ? b : normaliseBands (b); }
std::vector<float> genericTargetNormalised() { return normaliseBands (genericTarget()); }

// ------------------------------------------------------------------ Analyse
Analysis analyse (const Stereo& in, const ProgressFn& progress)
{
    Analysis a;
    const int n = in.size();
    a.seconds = in.seconds();
    if (n < 4096) return a;
    if (progress && ! progress (0.05f)) return a;

    const auto ld = measureLoudness (in.l.data(), in.r.data(), n, in.fs);
    a.lufs = ld.integrated; a.lra = ld.lra;
    if (progress && ! progress (0.4f)) return a;

    a.truePeakDb = linToDb (truePeakLinear (in.l.data(), in.r.data(), n));
    float sp = 0.0f; int clip = 0;
    double sLL = 0, sRR = 0, sLR = 0, sM = 0, sS = 0;
    for (int i = 0; i < n; ++i)
    {
        const float l = in.l[(size_t) i], r = in.r[(size_t) i];
        const float m = std::max (std::abs (l), std::abs (r));
        sp = std::max (sp, m);
        if (m >= 0.9995f) ++clip;
        sLL += (double) l * l; sRR += (double) r * r; sLR += (double) l * r;
        const double M = 0.5 * (l + r), S = 0.5 * (l - r);
        sM += M * M; sS += S * S;
    }
    a.samplePeakDb = linToDb (sp);
    a.clippedSamples = clip;
    a.correlation = (sLL > 0 && sRR > 0) ? (float) (sLR / std::sqrt (sLL * sRR)) : 1.0f;
    a.sideMidDb = (float) (10.0 * std::log10 ((sS + 1e-12) / (sM + 1e-12)));
    if (progress && ! progress (0.6f)) return a;

    a.bands = bandSpectrum (in.l.data(), in.r.data(), n, in.fs);
    a.valid = true;
    if (progress) progress (1.0f);
    return a;
}

// ------------------------------------------------------------------ Étages
static void eqStage (Stereo& w, const Analysis& before, const Settings& s, Report& rep)
{
    const auto& st = style (s.style);
    const double fs = w.fs;
    const bool useRef = s.reference && s.reference->valid;
    const float maxDb = useRef ? 4.0f : 3.0f;
    const bool voice = s.content == 1;
    const float amount = (useRef ? 0.8f : st.eqAmount) * (voice ? 0.75f : 1.0f);   // voix : on corrige moins, le mix est déjà équilibré

    const auto target = useRef ? normaliseBands (s.reference->bands) : normaliseBands (genericTarget());
    const auto meas = normaliseBands (before.bands);
    rep.targetBands = target;

    std::vector<float> d ((size_t) kBands, 0.0f);
    for (int b = 0; b < kBands; ++b)
        d[(size_t) b] = (bandCentre (b) < fs * 0.45) ? target[(size_t) b] - meas[(size_t) b] : 0.0f;

    // retire l'écart de niveau moyen (on ne corrige que la forme)
    double sm = 0.0; int c = 0;
    for (int b = 0; b < kBands; ++b)
        if (bandCentre (b) >= 100.0 && bandCentre (b) <= 4000.0) { sm += d[(size_t) b]; ++c; }
    const float off = (float) (sm / std::max (1, c));
    for (auto& x : d) x -= off;

    for (int pass = 0; pass < 2; ++pass)   // lissage : on ne corrige que les grandes lignes
    {
        const auto t = d;
        for (int b = 0; b < kBands; ++b)
        {
            const float a = t[(size_t) std::max (0, b - 1)], m = t[(size_t) b], z = t[(size_t) std::min (kBands - 1, b + 1)];
            d[(size_t) b] = 0.25f * a + 0.5f * m + 0.25f * z;
        }
    }
    std::vector<float> curve ((size_t) kBands, 0.0f);
    for (int b = 0; b < kBands; ++b)
    {
        const double f = bandCentre (b);
        float lim = (f < 40.0 || f > 14000.0) ? 1.5f : maxDb;
        if (voice && f >= 1000.0 && f <= 4500.0) lim = 1.5f;   // zone de présence de la voix : pas de grosse correction

        curve[(size_t) b] = std::clamp (d[(size_t) b], -lim, lim) * amount;
    }
    // Teintes du style et réglage Brillance
    for (int b = 0; b < kBands; ++b)
    {
        const double f = bandCentre (b);
        const double lowShelf = 1.0 / (1.0 + (f / 110.0) * (f / 110.0));
        const double hiShelf = 1.0 / (1.0 + (5000.0 / f) * (5000.0 / f));
        if (! useRef) curve[(size_t) b] += (float) (st.bassDb * (voice ? 0.5 : 1.0) * lowShelf + st.trebleDb * hiShelf);
        curve[(size_t) b] += (float) (s.bright * 3.0 * hiShelf);
        if (! voice && s.vocalRoom)   // « place pour la voix » : creux large (~1,5 octave) centré sur 2 kHz, -2 dB
        {
            const double o = std::log2 (f / 2000.0);
            curve[(size_t) b] -= (float) (2.0 * std::exp (-(o * o) / (2.0 * 0.75 * 0.75)));
        }
    }
    rep.eqCurveDb = curve;

    const auto h = designKernel (curve, fs, 22.0);
    firZeroLatencyStereo (w.l.data(), w.r.data(), w.size(), h);
}

static bool isActive (const std::vector<char>& a, int i, int hop)
{
    return a[std::min ((size_t) (i / hop), a.size() - 1)] != 0;
}

static bool multibandStage (Stereo& w, const std::vector<char>& active, int hop, const Settings& s, Report& rep, const ProgressFn& progress)
{
    const auto& st = style (s.style);
    const int n = w.size();
    const double fs = w.fs;
    const float f1 = 140.0f, f2 = 3000.0f;

    juce::dsp::ProcessSpec spec { fs, 512, 2 };
    juce::dsp::LinkwitzRileyFilter<float> x1, x2, x2b;
    const auto prep = [&]
    {
        for (auto* f : { &x1, &x2, &x2b }) { f->prepare (spec); f->reset(); }
        x1.setCutoffFrequency (f1); x2.setCutoffFrequency (f2); x2b.setCutoffFrequency (f2);
    };

    const auto split = [&] (float l, float r, float (&bd)[3][2])
    {
        const float in[2] = { l, r };
        for (int ch = 0; ch < 2; ++ch)
        {
            float lo, rest, mid, hi, la, lb;
            x1.processSample (ch, in[ch], lo, rest);
            x2.processSample (ch, rest, mid, hi);
            x2b.processSample (ch, lo, la, lb);
            bd[0][ch] = la + lb; bd[1][ch] = mid; bd[2][ch] = hi;
        }
    };

    // Passe 1 : niveau moyen de chaque bande (blocs actifs) -> seuils adaptés au morceau
    prep();
    double ms[3] = { 0, 0, 0 }; long cnt = 0;
    float bd[3][2];
    for (int i = 0; i < n; ++i)
    {
        split (w.l[(size_t) i], w.r[(size_t) i], bd);
        if (isActive (active, i, hop))
        {
            for (int b = 0; b < 3; ++b) ms[b] += 0.5 * ((double) bd[b][0] * bd[b][0] + (double) bd[b][1] * bd[b][1]);
            ++cnt;
        }
        if ((i & 0xFFFFF) == 0 && progress && ! progress ((float) i / n * 0.3f)) return false;
    }
    if (cnt == 0) return true;

    struct Band { float thr = 0, thrLin = 0, ratio = 2, atk = 0, rel = 0, env = 0; double grSum = 0.0; } band[3];
    const bool voice = s.content == 1;
    const float atkMs[3] = { 25.0f, voice ? 18.0f : 12.0f, 4.0f }, relMs[3] = { 220.0f, 140.0f, 90.0f };   // voix : attaque médium plus lente, transitoires préservés
    const float pf = 0.4f + 1.2f * s.punch, rs = st.releaseMs / 80.0f;
    for (int b = 0; b < 3; ++b)
    {
        const float rmsDb = (float) (10.0 * std::log10 (ms[b] / (double) cnt + 1e-14));
        band[b].thr = rmsDb + 2.0f - 5.0f * s.glue;
        band[b].thrLin = dbToLin (band[b].thr);
        band[b].ratio = std::max (1.05f, (1.3f + 1.7f * s.glue) * (b == 2 ? 0.8f : (voice && b == 1 ? 0.85f : 1.0f)));
        band[b].atk = std::exp (-1.0f / (atkMs[b] * pf * 0.001f * (float) fs));
        band[b].rel = std::exp (-1.0f / (relMs[b] * rs * 0.001f * (float) fs));
    }

    // Passe 2 : compression multibande (détection liée L/R)
    prep();
    const float knee = 6.0f;
    long act = 0;
    for (int i = 0; i < n; ++i)
    {
        split (w.l[(size_t) i], w.r[(size_t) i], bd);
        const bool isAct = isActive (active, i, hop);
        float outL = 0.0f, outR = 0.0f;
        for (int b = 0; b < 3; ++b)
        {
            auto& B = band[b];
            const float lvl = std::max (std::abs (bd[b][0]), std::abs (bd[b][1]));
            B.env = lvl > B.env ? lvl + (B.env - lvl) * B.atk : lvl + (B.env - lvl) * B.rel;
            float g = 1.0f;
            if (B.env > B.thrLin * 0.45f)
            {
                const float over = linToDb (B.env) - B.thr;
                float gr = 0.0f;
                if (2.0f * over < -knee) gr = 0.0f;
                else if (2.0f * std::abs (over) <= knee) { const float t = over + knee * 0.5f; gr = (1.0f / B.ratio - 1.0f) * t * t / (2.0f * knee); }
                else gr = (1.0f / B.ratio - 1.0f) * over;
                g = std::exp (gr * 0.11512925f);
                if (isAct) B.grSum += gr;
            }
            outL += bd[b][0] * g; outR += bd[b][1] * g;
        }
        if (isAct) ++act;
        w.l[(size_t) i] = outL; w.r[(size_t) i] = outR;
        if ((i & 0xFFFFF) == 0 && progress && ! progress (0.3f + (float) i / n * 0.7f)) return false;
    }
    for (int b = 0; b < 3; ++b) rep.bandGrDb[b] = act > 0 ? (float) (-band[b].grSum / (double) act) : 0.0f;
    return true;
}

// De-esser (mode voix) : réduit dynamiquement la bande > 5,5 kHz quand les sibilantes dépassent le niveau moyen du morceau.
static bool deEssStage (Stereo& w, const std::vector<char>& active, int hop, Report& rep, const ProgressFn& progress)
{
    const int n = w.size();
    juce::dsp::ProcessSpec spec { w.fs, 512, 2 };
    juce::dsp::LinkwitzRileyFilter<float> xo;
    const auto prep = [&] { xo.prepare (spec); xo.reset(); xo.setCutoffFrequency (5500.0f); };
    float lo[2], hi[2];
    const auto split = [&] (float l, float r)
    {
        const float in[2] = { l, r };
        for (int ch = 0; ch < 2; ++ch) xo.processSample (ch, in[ch], lo[ch], hi[ch]);
    };
    prep();
    double ms = 0.0; long cnt = 0;
    for (int i = 0; i < n; ++i)
    {
        split (w.l[(size_t) i], w.r[(size_t) i]);
        if (isActive (active, i, hop)) { const double m = 0.5 * ((double) hi[0] + hi[1]); ms += m * m; ++cnt; }
        if ((i & 0xFFFFF) == 0 && progress && ! progress ((float) i / n * 0.4f)) return false;
    }
    if (cnt == 0) return true;
    const float thr = (float) (10.0 * std::log10 (ms / (double) cnt + 1e-14)) + 3.5f;   // dB (puissance)
    const float atk = std::exp (-1.0f / (0.0005f * (float) w.fs)), rel = std::exp (-1.0f / (0.045f * (float) w.fs));
    const float ratio = 3.0f, maxGr = -6.0f;
    prep();
    float env = 0.0f; double grSum = 0.0; long act = 0;
    for (int i = 0; i < n; ++i)
    {
        split (w.l[(size_t) i], w.r[(size_t) i]);
        const float lvl = std::abs (0.5f * (hi[0] + hi[1]));
        env = lvl > env ? lvl + (env - lvl) * atk : lvl + (env - lvl) * rel;
        float gr = 0.0f;
        const float envDb = 20.0f * std::log10 (env + 1.0e-9f);
        if (envDb > thr) gr = std::max (maxGr, -(envDb - thr) * (1.0f - 1.0f / ratio));
        const float g = std::exp (gr * 0.11512925f);
        w.l[(size_t) i] = lo[0] + hi[0] * g; w.r[(size_t) i] = lo[1] + hi[1] * g;
        if (isActive (active, i, hop)) { grSum += gr; ++act; }
        if ((i & 0xFFFFF) == 0 && progress && ! progress (0.4f + (float) i / n * 0.6f)) return false;
    }
    rep.deEssGrDb = act > 0 ? (float) (-grSum / (double) act) : 0.0f;
    return true;
}

static void saturationStage (Stereo& w, float amount)
{
    if (amount <= 0.001f) return;
    const int block = 2048, n = w.size();
    juce::dsp::Oversampling<float> os (2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
    os.initProcessing ((size_t) block);
    os.reset();
    const int lat = (int) std::lround (os.getLatencyInSamples());
    juce::AudioBuffer<float> buf (2, block);
    const float drive = 2.5f;
    for (int pos = 0; pos < n + lat; pos += block)
    {
        for (int i = 0; i < block; ++i)
        {
            const int idx = pos + i;
            buf.setSample (0, i, idx < n ? w.l[(size_t) idx] : 0.0f);
            buf.setSample (1, i, idx < n ? w.r[(size_t) idx] : 0.0f);
        }
        juce::dsp::AudioBlock<float> ab (buf);
        auto up = os.processSamplesUp (ab);
        for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
        {
            float* p = up.getChannelPointer (ch);
            for (size_t i = 0; i < up.getNumSamples(); ++i) p[i] = std::tanh (drive * p[i]) / drive;
        }
        os.processSamplesDown (ab);
        for (int i = 0; i < block; ++i)
        {
            const int idx = pos + i - lat;
            if (idx < 0 || idx >= n) continue;
            w.l[(size_t) idx] = w.l[(size_t) idx] * (1.0f - amount) + amount * buf.getSample (0, i);
            w.r[(size_t) idx] = w.r[(size_t) idx] * (1.0f - amount) + amount * buf.getSample (1, i);
        }
    }
}

static void stereoStage (Stereo& w, float width)
{
    // (la limite de largeur du mode voix est appliquée par l'appelant)
    juce::dsp::LinkwitzRileyFilter<float> hp;
    juce::dsp::ProcessSpec spec { w.fs, 512, 1 };
    hp.prepare (spec); hp.reset(); hp.setCutoffFrequency (120.0f);
    for (int i = 0; i < w.size(); ++i)
    {
        const float l = w.l[(size_t) i], r = w.r[(size_t) i];
        const float m = 0.5f * (l + r), sd = 0.5f * (l - r);
        float lo, hi;
        hp.processSample (0, sd, lo, hi);       // basses en mono : seule la partie > 120 Hz du Side est conservée
        const float s2 = hi * width;
        w.l[(size_t) i] = m + s2; w.r[(size_t) i] = m - s2;
    }
}

// Limiteur true-peak à anticipation : gain[i] garanti <= C / (g * tp[i]).
static void limiterGain (const std::vector<float>& tp, float g, float C, int L, float relCoef, std::vector<float>& gain, std::vector<float>& tmp)
{
    const int n = (int) tp.size();
    gain.resize ((size_t) n); tmp.resize ((size_t) n);
    std::vector<float>& r = tmp;
    for (int i = 0; i < n; ++i) r[(size_t) i] = std::min (1.0f, C / (g * tp[(size_t) i] + 1.0e-12f));

    // minimum glissant sur [i, i+L-1]
    std::vector<int> dq ((size_t) n);
    int head = 0, tail = 0;
    for (int i = n - 1; i >= 0; --i)
    {
        while (tail > head && r[(size_t) dq[(size_t) tail - 1]] >= r[(size_t) i]) --tail;
        dq[(size_t) tail++] = i;
        while (dq[(size_t) head] > i + L - 1) ++head;
        gain[(size_t) i] = r[(size_t) dq[(size_t) head]];
    }
    // release (attaque instantanée sur le minimum anticipé)
    float e = 1.0f;
    for (int i = 0; i < n; ++i)
    {
        const float m = gain[(size_t) i];
        e = m < e ? m : m + (e - m) * relCoef;
        r[(size_t) i] = e;
    }
    // moyenne glissante sur L échantillons : rampe d'attaque
    const double pre = n > 0 ? (double) r[0] : 1.0;   // avant le début : on prolonge la première valeur (garantit le plafond dès le 1er échantillon)
    double sum = pre * L;
    for (int i = 0; i < n; ++i)
    {
        sum += r[(size_t) i];
        const int old = i - L;
        sum -= old >= 0 ? (double) r[(size_t) old] : pre;
        gain[(size_t) i] = (float) (sum / L);
    }
}

// ------------------------------------------------------------------ Chaîne complète
bool masterAudio (const Stereo& in, const Analysis& before, const Settings& s, Stereo& out, Report& rep, const ProgressFn& progress)
{
    rep = Report();
    rep.before = before;
    const int n = in.size();
    if (! before.valid || before.lufs < -69.0) { rep.warnings.push_back ({ WarnSilent, 0.0f }); return false; }
    if (before.seconds < 10.0) rep.warnings.push_back ({ WarnShortFile, (float) before.seconds });

    const auto step = [&] (float a, float b) -> ProgressFn { return [&progress, a, b] (float p) { return ! progress || progress (a + (b - a) * p); }; };
    const auto prog = [&] (float p) { return ! progress || progress (p); };

    Stereo w = in;
    // Niveau interne de travail : -18 LUFS (le niveau final est recalé par l'étage de loudness)
    const float preLin = dbToLin (std::clamp (-18.0f - (float) before.lufs, -40.0f, 40.0f));
    for (int i = 0; i < n; ++i) { w.l[(size_t) i] *= preLin; w.r[(size_t) i] *= preLin; }

    // blocs actifs (> -55 dBFS RMS sur le signal interne) pour les statistiques
    const int hop = std::max (1, (int) std::lround (w.fs * 0.1));
    std::vector<char> active ((size_t) n / (size_t) hop + 1, 0);
    for (size_t b = 0; b < active.size(); ++b)
    {
        double e = 0.0; int c = 0;
        for (int i = (int) b * hop; i < std::min (n, (int) (b + 1) * hop); ++i)
        { e += 0.5 * ((double) w.l[(size_t) i] * w.l[(size_t) i] + (double) w.r[(size_t) i] * w.r[(size_t) i]); ++c; }
        active[b] = (c > 0 && e / c > 3.16e-6) ? 1 : 0;
    }

    if (! prog (0.02f)) return false;
    eqStage (w, before, s, rep);
    if (! prog (0.2f)) return false;
    const bool voice = s.content == 1;
    if (voice && ! deEssStage (w, active, hop, rep, step (0.2f, 0.3f))) return false;
    if (! multibandStage (w, active, hop, s, rep, step (0.3f, 0.5f))) return false;
    saturationStage (w, style (s.style).sat * (voice ? 0.6f : 1.0f));
    if (! prog (0.62f)) return false;
    stereoStage (w, voice ? std::min (s.width, 1.1f) : s.width);   // voix : le centre reste ferme, élargissement limité
    if (! prog (0.7f)) return false;

    // ---- Loudness + limiteur true-peak
    std::vector<float> tp ((size_t) n, 0.0f);
    truePeakEnvelope (w.l.data(), n, tp.data(), 0.03f);
    truePeakEnvelope (w.r.data(), n, tp.data(), 0.03f);
    if (! prog (0.78f)) return false;

    const float C = dbToLin (s.ceilingDb) * 0.97f;
    const int L = std::max (8, (int) std::lround (w.fs * 0.002));
    const float relCoef = std::exp (-1.0f / (style (s.style).releaseMs * (voice ? 1.4f : 1.0f) * 0.001f * (float) w.fs));

    const auto lw = measureLoudness (w.l.data(), w.r.data(), n, w.fs);
    float gDb = (float) (s.targetLufs - lw.integrated);
    std::vector<float> gain, tmp;
    out.fs = in.fs; out.l.assign ((size_t) n, 0.0f); out.r.assign ((size_t) n, 0.0f);

    float avgGr = 0, maxGr = 0;
    for (int it = 0; it < 8; ++it)
    {
        const float g = dbToLin (gDb);
        limiterGain (tp, g, C, L, relCoef, gain, tmp);
        double grSum = 0.0; long ac = 0; float mg = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            out.l[(size_t) i] = w.l[(size_t) i] * g * gain[(size_t) i];
            out.r[(size_t) i] = w.r[(size_t) i] * g * gain[(size_t) i];
            if (isActive (active, i, hop))
            {
                const float gr = -linToDb (gain[(size_t) i]);
                grSum += gr; ++ac; mg = std::max (mg, gr);
            }
        }
        avgGr = ac ? (float) (grSum / (double) ac) : 0.0f; maxGr = mg;
        const auto lo = measureLoudness (out.l.data(), out.r.data(), n, out.fs);
        const double err = s.targetLufs - lo.integrated;
        rep.iterations = it + 1;
        if (std::abs (err) < 0.1 || avgGr > 14.0f) break;
        gDb += (float) err;
        if (! prog (0.78f + 0.015f * (float) it)) return false;
    }
    rep.gainIntoLimiterDb = gDb; rep.limiterAvgGrDb = avgGr; rep.limiterMaxGrDb = maxGr;

    // Contrôle final du true-peak (sécurité)
    {
        const float tpOut = truePeakLinear (out.l.data(), out.r.data(), n);
        const float ceilLin = dbToLin (s.ceilingDb);
        if (tpOut > ceilLin * 1.0005f)
        {
            const float k = ceilLin / tpOut * 0.9995f;
            for (int i = 0; i < n; ++i) { out.l[(size_t) i] *= k; out.r[(size_t) i] *= k; }
        }
    }
    if (! prog (0.92f)) return false;

    rep.after = analyse (out);

    // ---- Avertissements
    if (before.lufs > -9.0) rep.warnings.push_back ({ WarnAlreadyLoud, (float) before.lufs });
    if (before.clippedSamples > 3) rep.warnings.push_back ({ WarnClippedInput, (float) before.clippedSamples });
    if (before.correlation < 0.0f) rep.warnings.push_back ({ WarnPhase, before.correlation });
    if (before.sideMidDb < -30.0f) rep.warnings.push_back ({ WarnNearMono, before.sideMidDb });
    if (rep.limiterAvgGrDb > 6.0f) rep.warnings.push_back ({ WarnHeavyLimiting, rep.limiterAvgGrDb });
    if (std::abs (rep.after.lufs - s.targetLufs) > 0.5) rep.warnings.push_back ({ WarnTargetMissed, (float) rep.after.lufs });
    if (progress) progress (1.0f);
    return true;
}
}
