#pragma once
// Briques DSP bas niveau : biquad, pondération K, mesures LUFS / LRA / true-peak,
// FFT, analyse spectrale en tiers d'octave, convolution FIR rapide.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace master
{
constexpr double kPi = 3.14159265358979323846;

inline float dbToLin (float d)  { return std::pow (10.0f, d / 20.0f); }
inline float linToDb (float l)  { return 20.0f * std::log10 (std::max (l, 1.0e-9f)); }

// ------------------------------------------------------------------ Biquad (DF2 transposée, double)
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Pondération K (ITU-R BS.1770-4), coefficients valables pour toute fréquence d'échantillonnage.
inline void kWeighting (double fs, Biquad& s1, Biquad& s2)
{
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (kPi * f0 / fs);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        s1.b0 = (Vh + Vb * K / Q + K * K) / a0;
        s1.b1 = 2.0 * (K * K - Vh) / a0;
        s1.b2 = (Vh - Vb * K / Q + K * K) / a0;
        s1.a1 = 2.0 * (K * K - 1.0) / a0;
        s1.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (kPi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        s2.b0 = 1.0; s2.b1 = -2.0; s2.b2 = 1.0;
        s2.a1 = 2.0 * (K * K - 1.0) / a0;
        s2.a2 = (1.0 - K / Q + K * K) / a0;
    }
}

// ------------------------------------------------------------------ Loudness (LUFS intégré, LRA)
struct Loudness
{
    double integrated = -70.0;
    double lra = 0.0;
};

inline double energyToLufs (double e) { return -0.691 + 10.0 * std::log10 (std::max (e, 1.0e-12)); }

inline Loudness measureLoudness (const float* L, const float* R, int n, double fs)
{
    Loudness out;
    const int hop = std::max (1, (int) std::lround (fs * 0.1));
    const int nsub = n / hop;
    if (nsub < 4) return out;

    std::vector<double> sub ((size_t) nsub, 0.0);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* x = ch == 0 ? L : R;
        Biquad s1, s2;
        kWeighting (fs, s1, s2);
        for (int k = 0; k < nsub; ++k)
        {
            double acc = 0.0;
            const float* p = x + (size_t) k * (size_t) hop;
            for (int i = 0; i < hop; ++i)
            {
                const double y = s2.process (s1.process ((double) p[i]));
                acc += y * y;
            }
            sub[(size_t) k] += acc / hop;
        }
    }

    // blocs de 400 ms (4 sous-blocs), recouvrement 75 %
    const int nb = nsub - 3;
    std::vector<double> blk ((size_t) nb);
    for (int j = 0; j < nb; ++j)
        blk[(size_t) j] = 0.25 * (sub[(size_t) j] + sub[(size_t) j + 1] + sub[(size_t) j + 2] + sub[(size_t) j + 3]);

    double sumAbs = 0.0; int cntAbs = 0;
    for (double e : blk) if (energyToLufs (e) > -70.0) { sumAbs += e; ++cntAbs; }
    if (cntAbs == 0) return out;
    const double rel = energyToLufs (sumAbs / cntAbs) - 10.0;
    double sum = 0.0; int cnt = 0;
    for (double e : blk) if (energyToLufs (e) > std::max (rel, -70.0)) { sum += e; ++cnt; }
    if (cnt > 0) out.integrated = energyToLufs (sum / cnt);

    // LRA : fenêtres de 3 s, pas de 1 s
    const int w = 30, st = 10;
    if (nsub >= w)
    {
        std::vector<double> sts;
        for (int j = 0; j + w <= nsub; j += st)
        {
            double e = 0.0;
            for (int i = 0; i < w; ++i) e += sub[(size_t) (j + i)];
            sts.push_back (e / w);
        }
        double sa = 0.0; int ca = 0;
        for (double e : sts) if (energyToLufs (e) > -70.0) { sa += e; ++ca; }
        if (ca > 0)
        {
            const double r2 = energyToLufs (sa / ca) - 20.0;
            std::vector<double> v;
            for (double e : sts) if (energyToLufs (e) > std::max (r2, -70.0)) v.push_back (energyToLufs (e));
            if (v.size() >= 2)
            {
                std::sort (v.begin(), v.end());
                const auto pct = [&] (double p) { return v[(size_t) std::min<double> ((double) v.size() - 1.0, std::floor (p * (double) (v.size() - 1) + 0.5))]; };
                out.lra = pct (0.95) - pct (0.10);
            }
        }
    }
    return out;
}

// ------------------------------------------------------------------ True-peak (suréchantillonnage x4, sinc fenêtré Kaiser 32 points)
inline double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; if (term < 1e-14 * sum) break; }
    return sum;
}

struct TruePeakTable
{
    static constexpr int taps = 32;
    float c[3][taps];
    TruePeakTable()
    {
        const double beta = 7.5, i0b = besselI0 (beta);
        for (int p = 1; p <= 3; ++p)
            for (int j = 0; j < taps; ++j)
            {
                const double d = p / 4.0 - (j - (taps / 2 - 1));
                const double s = std::abs (d) < 1e-9 ? 1.0 : std::sin (kPi * d) / (kPi * d);
                const double x = d / (taps / 2.0);
                const double w = std::abs (x) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - x * x)) / i0b;
                c[p - 1][j] = (float) (s * w);
            }
    }
};
inline const TruePeakTable& tpTable() { static const TruePeakTable t; return t; }

// env[i] = max (env[i], |x[i]|, pics interpolés entre i et i+1).
// Les échantillons dont le voisinage est sous skipBelow ne sont pas interpolés (aucun risque de dépassement à ce niveau).
inline void truePeakEnvelope (const float* x, int n, float* env, float skipBelow = 0.0f)
{
    const auto& T = tpTable();
    constexpr int H = TruePeakTable::taps / 2 - 1;
    for (int i = 0; i < n; ++i)
    {
        float m = std::abs (x[i]);
        if (i >= H && i + H + 1 < n)
        {
            const float local = std::max (m, std::abs (x[i + 1]));
            if (local >= skipBelow)
            {
                const float* p = x + i - H;
                for (int ph = 0; ph < 3; ++ph)
                {
                    float s = 0.0f;
                    for (int j = 0; j < TruePeakTable::taps; ++j) s += p[j] * T.c[ph][j];
                    m = std::max (m, std::abs (s));
                }
            }
        }
        env[i] = std::max (env[i], m);
    }
}

inline float truePeakLinear (const float* L, const float* R, int n)
{
    float sp = 0.0f;
    for (int i = 0; i < n; ++i) sp = std::max (sp, std::max (std::abs (L[i]), std::abs (R[i])));
    std::vector<float> env ((size_t) n, 0.0f);
    const float skip = sp * 0.55f;   // sous ce niveau, un pic interpolé ne peut pas dépasser le pic d'échantillon
    truePeakEnvelope (L, n, env.data(), skip);
    truePeakEnvelope (R, n, env.data(), skip);
    float m = sp;
    for (float v : env) m = std::max (m, v);
    return m;
}

// ------------------------------------------------------------------ FFT radix-2 (complexe, double)
class FFT
{
public:
    explicit FFT (int order) : n (1 << order), rev ((size_t) n), tw ((size_t) n / 2)
    {
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < order; ++b) if (i & (1 << b)) r |= 1 << (order - 1 - b);
            rev[(size_t) i] = r;
        }
        for (int i = 0; i < n / 2; ++i)
            tw[(size_t) i] = std::polar (1.0, -2.0 * kPi * i / n);
    }
    int size() const { return n; }

    void run (std::complex<double>* a, bool inverse) const
    {
        for (int i = 0; i < n; ++i) if (i < rev[(size_t) i]) std::swap (a[i], a[rev[(size_t) i]]);
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; ++j)
                {
                    std::complex<double> w = tw[(size_t) (j * step)];
                    if (inverse) w = std::conj (w);
                    const auto u = a[i + j], v = a[i + j + half] * w;
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
        }
        if (inverse) { const double s = 1.0 / n; for (int i = 0; i < n; ++i) a[i] *= s; }
    }
private:
    int n;
    std::vector<int> rev;
    std::vector<std::complex<double>> tw;
};

// ------------------------------------------------------------------ Spectre moyen en tiers d'octave
constexpr int kBands = 31;
inline double bandCentre (int k) { return 20.0 * std::pow (2.0, k / 3.0); }

// Énergie par bande (dB, valeur relative arbitraire) du signal mid, moyennée sur les trames actives.
inline std::vector<float> bandSpectrum (const float* L, const float* R, int n, double fs)
{
    const int order = 13, N = 1 << order, hop = N / 2;
    FFT fft (order);
    std::vector<double> win ((size_t) N);
    double wsum = 0.0;
    for (int i = 0; i < N; ++i) { win[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * kPi * i / N); wsum += win[(size_t) i] * win[(size_t) i]; }

    std::vector<double> P ((size_t) N / 2 + 1, 0.0);
    std::vector<std::complex<double>> buf ((size_t) N);
    int frames = 0;
    for (int s = 0; s + N <= n; s += hop)
    {
        double e = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const double m = 0.5 * ((double) L[s + i] + (double) R[s + i]);
            buf[(size_t) i] = { m * win[(size_t) i], 0.0 };
            e += m * m;
        }
        if (e / N < 1.0e-6) continue; // trames sous -60 dBFS ignorées
        fft.run (buf.data(), false);
        for (int k = 0; k <= N / 2; ++k) P[(size_t) k] += std::norm (buf[(size_t) k]);
        ++frames;
    }
    std::vector<float> out ((size_t) kBands, -120.0f);
    if (frames == 0) return out;

    const double binW = fs / N;
    const auto psd = [&] (double f)
    {
        const double x = f / binW;
        const int k = std::min ((int) x, N / 2 - 1);
        const double fr = x - k;
        return ((1.0 - fr) * P[(size_t) k] + fr * P[(size_t) k + 1]) / (frames * wsum * fs);
    };
    for (int b = 0; b < kBands; ++b)
    {
        const double fc = bandCentre (b);
        if (fc * 0.8909 > fs * 0.5) { out[(size_t) b] = -120.0f; continue; }
        const double lo = fc / std::pow (2.0, 1.0 / 6.0), hi = std::min (fc * std::pow (2.0, 1.0 / 6.0), fs * 0.5 - binW);
        const int pts = 16;
        double E = 0.0;
        for (int i = 0; i < pts; ++i)
        {
            const double f0 = lo * std::pow (hi / lo, (double) i / pts), f1 = lo * std::pow (hi / lo, (double) (i + 1) / pts);
            E += psd (std::sqrt (f0 * f1)) * (f1 - f0);
        }
        out[(size_t) b] = (float) (10.0 * std::log10 (E + 1.0e-20));
    }
    return out;
}

// soustrait la moyenne des bandes 100 Hz - 4 kHz : rend les courbes comparables
inline std::vector<float> normaliseBands (std::vector<float> v)
{
    double s = 0.0; int c = 0;
    for (int b = 0; b < kBands; ++b)
        if (bandCentre (b) >= 100.0 && bandCentre (b) <= 4000.0) { s += v[(size_t) b]; ++c; }
    const float m = (float) (s / std::max (1, c));
    for (auto& x : v) x -= m;
    return v;
}

// ------------------------------------------------------------------ Convolution FIR rapide (2 canaux, phase linéaire)
// Retourne le résultat compensé du retard du filtre (phase nulle effective).
inline void firZeroLatencyStereo (float* L, float* R, int n, const std::vector<float>& h)
{
    const int K = (int) h.size();
    const int order = 15, F = 1 << order;
    const int step = F - K + 1;
    const int M = (K - 1) / 2;
    FFT fft (order);

    std::vector<std::complex<double>> H ((size_t) F, { 0.0, 0.0 });
    for (int i = 0; i < K; ++i) H[(size_t) i] = { (double) h[(size_t) i], 0.0 };
    fft.run (H.data(), false);

    std::vector<float> outL ((size_t) n, 0.0f), outR ((size_t) n, 0.0f);
    std::vector<std::complex<double>> buf ((size_t) F);
    for (int s = 0; s < n + M; s += step)
    {
        for (int i = 0; i < F; ++i)
        {
            const int idx = s + i;
            if (i < step && idx < n) buf[(size_t) i] = { (double) L[idx], (double) R[idx] };
            else buf[(size_t) i] = { 0.0, 0.0 };
        }
        fft.run (buf.data(), false);
        for (int i = 0; i < F; ++i) buf[(size_t) i] *= H[(size_t) i];
        fft.run (buf.data(), true);
        for (int i = 0; i < F; ++i)
        {
            const int idx = s + i - M;
            if (idx >= 0 && idx < n) { outL[(size_t) idx] += (float) buf[(size_t) i].real(); outR[(size_t) idx] += (float) buf[(size_t) i].imag(); }
        }
    }
    std::copy (outL.begin(), outL.end(), L);
    std::copy (outR.begin(), outR.end(), R);
}

// Conçoit un noyau FIR à phase linéaire depuis une courbe de gain (dB) définie aux centres de bandes
// + passe-haut Butterworth d'ordre 4 à hpHz.
inline std::vector<float> designKernel (const std::vector<float>& curveDb, double fs, double hpHz, int taps = 8191)
{
    const int order = 15, N = 1 << order;
    FFT fft (order);
    std::vector<std::complex<double>> S ((size_t) N);
    const auto gainAt = [&] (double f)
    {
        if (f <= bandCentre (0)) return (double) curveDb[0];
        if (f >= bandCentre (kBands - 1)) return (double) curveDb[(size_t) kBands - 1];
        const double pos = 3.0 * std::log2 (f / 20.0);
        const int k = (int) std::floor (pos);
        const double fr = pos - k;
        return (1.0 - fr) * curveDb[(size_t) k] + fr * curveDb[(size_t) std::min (k + 1, kBands - 1)];
    };
    for (int k = 0; k <= N / 2; ++k)
    {
        const double f = k * fs / N;
        double mag = 0.0;
        if (k > 0)
        {
            mag = std::pow (10.0, gainAt (f) / 20.0);
            mag /= std::sqrt (1.0 + std::pow (hpHz / f, 8.0));
        }
        S[(size_t) k] = { mag, 0.0 };
        if (k > 0 && k < N / 2) S[(size_t) (N - k)] = { mag, 0.0 };
    }
    fft.run (S.data(), true); // réponse impulsionnelle à phase nulle (centrée en 0, circulaire)
    std::vector<float> h ((size_t) taps);
    const int M = (taps - 1) / 2;
    for (int i = 0; i < taps; ++i)
    {
        const int idx = ((i - M) % N + N) % N;
        const double x = (double) (i - M) / (M + 1);
        const double w = 0.42 + 0.5 * std::cos (kPi * x) + 0.08 * std::cos (2.0 * kPi * x); // Blackman
        h[(size_t) i] = (float) (S[(size_t) idx].real() * w);
    }
    return h;
}
} // namespace master
