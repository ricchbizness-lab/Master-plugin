#include "MainComponent.h"
#include "Mp3Decode.h"
#include "Dsp.h"

namespace ui

{
namespace { std::shared_ptr<Mastered> doMaster (std::shared_ptr<master::Stereo>, master::Analysis, master::Settings, bool, master::Analysis,
                                    const std::function<bool (float)>&, juce::int64 from = 0, juce::int64 len = -1); }
using namespace juce;

// ------------------------------------------------------------------ Fichiers audio
static String fmtTime (double s)
{
    const int t = (int) std::floor (s);
    return String::formatted ("%d:%02d", t / 60, t % 60);
}

static std::vector<float> computePeaks (const master::Stereo& a, int cols = 1000)
{
    std::vector<float> p ((size_t) cols * 2, 0.0f);
    const int n = a.size();
    for (int c = 0; c < cols; ++c)
    {
        const int s = (int) ((int64) n * c / cols), e = jmax (s + 1, (int) ((int64) n * (c + 1) / cols));
        float pk = 0.0f; double ms = 0.0;
        for (int i = s; i < jmin (e, n); ++i)
        {
            const float l = a.l[(size_t) i], r = a.r[(size_t) i];
            pk = jmax (pk, std::abs (l), std::abs (r));
            ms += 0.5 * ((double) l * l + (double) r * r);
        }
        p[(size_t) c * 2] = pk;
        p[(size_t) c * 2 + 1] = (float) std::sqrt (ms / jmax (1, e - s));
    }
    return p;
}

static std::shared_ptr<Loaded> readAndAnalyse (const File& f, const std::function<bool (float)>& progress)
{
    auto out = std::make_shared<Loaded>();
    auto a = std::make_shared<master::Stereo>();
    constexpr int64 maxFrames = (int64) 1 << 26;
    String meta;
    bool decoded = false;
    String firstError;

    auto fillMeta = [&] (double fs, int ch, int bits)
    {
        meta = String (fs / 1000.0, 1) + " kHz  ·  " + String (ch) + (ch > 1 ? tr (" canaux") : tr (" canal"))
             + "  ·  " + fmtTime (a->seconds()) + (bits > 0 ? String::fromUTF8 ("  ·  ") + String (bits) + " bits" : String());
    };

    // MP3 : décodeur dédié d'abord (tolère tags ID3, pochettes, VBR...), JUCE en secours
    if (f.hasFileExtension ("mp3"))
    {
        auto d = master::decodeMp3 (f, maxFrames);
        if (d.ok)
        {
            a->fs = d.fs; a->l = std::move (d.l); a->r = std::move (d.r);
            fillMeta (d.fs, d.channels, 0);
            decoded = true;
        }
        else firstError = d.error;
    }

    if (! decoded)
    {
        AudioFormatManager fm; fm.registerBasicFormats();
        fm.registerFormat (new MP3AudioFormat(), false);
        std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
        if (! r) { out->error = firstError.isNotEmpty() ? firstError : "Format non pris en charge (WAV, AIFF, FLAC, MP3, OGG)."; return out; }
        if (r->lengthInSamples < 4096) { out->error = "Fichier trop court."; return out; }
        if (r->lengthInSamples > maxFrames) { out->error = "Fichier trop long (25 min maximum)."; return out; }
        const int n = (int) r->lengthInSamples;
        AudioBuffer<float> b ((int) jmax (2u, r->numChannels), n);
        b.clear();
        if (! r->read (&b, 0, n, 0, true, true)) { out->error = "Lecture du fichier impossible."; return out; }
        a->fs = r->sampleRate;
        const int chR = r->numChannels > 1 ? 1 : 0;
        a->l.assign (b.getReadPointer (0), b.getReadPointer (0) + n);
        a->r.assign (b.getReadPointer (chR), b.getReadPointer (chR) + n);
        fillMeta (r->sampleRate, (int) r->numChannels, (int) r->bitsPerSample);
    }

    if (progress && ! progress (0.15f)) { out->error = "Annulé."; return out; }
    out->an = master::analyse (*a, [&] (float p) { return ! progress || progress (0.15f + 0.85f * p); });
    if (! out->an.valid) { out->error = "Annulé."; return out; }
    out->peaks = computePeaks (*a);
    out->audio = a;
    out->name = f.getFileName();
    out->file = f;
    out->meta = meta;
    out->ok = true;
    return out;
}

// ------------------------------------------------------------------ Lecture audio (A/B)
class Player : public AudioSource
{
public:
    void prepareToPlay (int block, double fs) override
    {
        deviceFs = fs;
        outL.assign ((size_t) block + 16, 0.0f); outR = outL;
        inL.assign ((size_t) (block * 8) + 64, 0.0f); inR = inL;
        needReset = true;
    }
    void releaseResources() override {}

    // m couvre [off, off + m->size()) de la timeline de l'original (aperçu partiel pendant le calcul exact)
    void setBuffers (std::shared_ptr<const master::Stereo> o, std::shared_ptr<const master::Stereo> m, int64 off = 0)
    {
        const SpinLock::ScopedLockType sl (lock);
        orig = std::move (o); mast = std::move (m); mastOff = off;
        if (! orig) { playing = false; pos = 0; }
    }
    double fraction() const
    {
        const SpinLock::ScopedLockType sl (lock);
        const auto& b = orig ? orig : mast;
        return b && b->size() > 0 ? jlimit (0.0, 1.0, (double) pos.load() / b->size()) : 0.0;
    }
    void seek (double f)
    {
        const SpinLock::ScopedLockType sl (lock);
        const auto& b = orig ? orig : mast;
        if (b) pos = (int64) (jlimit (0.0, 1.0, f) * b->size());
        needReset = true;
    }

    void getNextAudioBlock (const AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        const SpinLock::ScopedTryLockType sl (lock);
        if (! sl.isLocked() || ! playing) return;
        if (! orig) return;
        const int n = info.numSamples;
        const double ratio = orig->fs / deviceFs;
        const int totalO = orig->size();
        int64 p = pos;
        if (p >= totalO) { playing = false; pos = 0; return; }
        const int need = (int) std::ceil (n * ratio) + 8;
        if (need > (int) inL.size()) return;
        // le master n'est lu que si le bloc entier est couvert, sinon repli sur l'original
        const bool useM = useMaster && mast && p >= mastOff && p + need <= mastOff + mast->size();
        const auto buf = useM ? mast : orig;
        const int64 base = useM ? mastOff : 0;
        const float g = useM ? gM : gO;
        const int total = buf->size();
        for (int i = 0; i < need; ++i)
        {
            const int64 idx = p - base + i;
            inL[(size_t) i] = idx < total ? buf->l[(size_t) idx] : 0.0f;
            inR[(size_t) i] = idx < total ? buf->r[(size_t) idx] : 0.0f;
        }
        int used = n;
        if (std::abs (ratio - 1.0) < 1.0e-6)
        {
            std::copy (inL.begin(), inL.begin() + n, outL.begin());
            std::copy (inR.begin(), inR.begin() + n, outR.begin());
        }
        else
        {
            if (needReset.exchange (false)) { lagL.reset(); lagR.reset(); }
            used = lagL.process (ratio, inL.data(), outL.data(), n);
            lagR.process (ratio, inR.data(), outR.data(), n);
        }
        // fondu lors d'un changement A/B ou de gain (rampe sur le bloc)
        const float g0 = lastGain < 0.0f ? g : lastGain;
        auto* l = info.buffer->getWritePointer (0, info.startSample);
        auto* r = info.buffer->getNumChannels() > 1 ? info.buffer->getWritePointer (1, info.startSample) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float gg = g0 + (g - g0) * (float) i / (float) n;
            l[i] = outL[(size_t) i] * gg;
            if (r) r[i] = outR[(size_t) i] * gg;
        }
        lastGain = g;
        pos = p + used;
        if (pos >= totalO) { playing = false; pos = 0; lastGain = -1.0f; }
    }

    mutable SpinLock lock;
    std::atomic<bool> playing { false }, useMaster { false }, needReset { true };
    std::atomic<float> gO { 1.0f }, gM { 1.0f };
    std::atomic<int64> pos { 0 };
private:
    std::shared_ptr<const master::Stereo> orig, mast;
    int64 mastOff = 0;
    double deviceFs = 44100.0;
    float lastGain = -1.0f;
    std::vector<float> outL, outR, inL, inR;
    LagrangeInterpolator lagL, lagR;
};

// ------------------------------------------------------------------ Vues
class TransportButton : public Button
{
public:
    TransportButton() : Button ("play") {}
    bool playing = false;
    void paintButton (Graphics& g, bool hover, bool) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool en = isEnabled();
        g.setGradientFill (ColourGradient ((en ? violet : surface2).brighter (hover ? 0.25f : 0.1f), 0, r.getY(), (en ? violetDark : surface2).darker (0.1f), 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (en ? violetHi.withAlpha (0.7f) : border);
        g.drawRoundedRectangle (r, 8.0f, 1.0f);
        g.setColour (Colours::white.withMultipliedAlpha (en ? 1.0f : 0.4f));
        const auto c = r.getCentre();
        Path p;
        if (playing) { p.addRectangle (c.x - 6.0f, c.y - 6.0f, 4.0f, 12.0f); p.addRectangle (c.x + 2.0f, c.y - 6.0f, 4.0f, 12.0f); }
        else p.addTriangle (c.x - 4.5f, c.y - 7.0f, c.x - 4.5f, c.y + 7.0f, c.x + 7.0f, c.y);
        g.fillPath (p);
    }
};

class DropZone : public Component
{
public:
    std::function<void()> onClick;
    String name, meta, hint;
    bool loaded = false, busyNow = false;
    float progress = 0.0f;
    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (inkWell);
        g.fillRoundedRectangle (r, 10.0f);
        Path p; p.addRoundedRectangle (r, 10.0f);
        PathStrokeType st (1.2f);
        Path dashed;
        const float dl[] = { 5.0f, 4.0f };
        st.createDashedStroke (dashed, p, dl, 2);
        g.setColour ((isMouseOver() ? violetHi : violet).withAlpha (loaded ? 0.0f : 0.55f));
        g.fillPath (dashed);
        if (loaded) { g.setColour (violet.withAlpha (isMouseOver() ? 0.8f : 0.45f)); g.drawRoundedRectangle (r, 10.0f, 1.0f); }
        auto inner = getLocalBounds().reduced (12, 6);
        if (loaded)
        {
            g.setColour (text); g.setFont (font (13.0f, true));
            g.drawFittedText (name, inner.removeFromTop (inner.getHeight() / 2 + 4), Justification::bottomLeft, 1);
            g.setColour (textDimmer); g.setFont (font (10.5f));
            g.drawFittedText (meta, inner, Justification::topLeft, 2);
        }
        else
        {
            g.setColour (textDim); g.setFont (font (11.0f));
            g.drawFittedText (hint, inner, Justification::centred, 3);
        }
        if (busyNow)
        {
            auto bar = getLocalBounds().reduced (12, 0).removeFromBottom (8).toFloat().withTrimmedBottom (3.0f);
            g.setColour (Colours::white.withAlpha (0.08f)); g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (violet); g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * progress), 2.0f);
        }
    }
    void mouseEnter (const MouseEvent&) override { repaint(); }
    void mouseExit (const MouseEvent&) override { repaint(); }
    void mouseUp (const MouseEvent& e) override { if (e.mouseWasClicked() && onClick) onClick(); }
};

class WaveView : public Component
{
public:
    std::vector<float> orig, mast;
    std::function<double()> playhead;
    std::function<void (double)> onSeek;
    bool showMaster = true;
    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (inkWell); g.fillRoundedRectangle (r, 10.0f);
        g.setColour (border.withAlpha (0.6f)); g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);
        const float mid = r.getCentreY(), half = r.getHeight() * 0.5f - 6.0f;
        g.setColour (Colours::white.withAlpha (0.05f)); g.drawHorizontalLine ((int) mid, r.getX() + 6, r.getRight() - 6);
        const auto draw = [&] (const std::vector<float>& p, Colour peak, Colour rms)
        {
            if (p.empty()) return;
            const int cols = (int) p.size() / 2;
            const float x0 = r.getX() + 6.0f, w = r.getWidth() - 12.0f;
            Path pk, rm;
            for (int c = 0; c < cols; ++c)
            {
                const float x = x0 + w * ((float) c + 0.5f) / cols;
                pk.addRectangle (x - 0.5f * w / cols, mid - p[(size_t) c * 2] * half, jmax (1.0f, w / cols), jmax (1.0f, p[(size_t) c * 2] * half * 2.0f));
                rm.addRectangle (x - 0.5f * w / cols, mid - p[(size_t) c * 2 + 1] * half, jmax (1.0f, w / cols), jmax (1.0f, p[(size_t) c * 2 + 1] * half * 2.0f));
            }
            g.setColour (peak); g.fillPath (pk);
            g.setColour (rms); g.fillPath (rm);
        };
        if (! orig.empty() && ! mast.empty())
        {
            // les deux courbes sont tracées à leur niveau réel : on voit le gain de volume du master
            draw (orig, Colour (0xff7d7a90).withAlpha (showMaster ? 0.30f : 0.55f), Colour (0xffb9b6c9).withAlpha (showMaster ? 0.35f : 0.8f));
            draw (mast, violet.withAlpha (showMaster ? 0.55f : 0.0f), violetHi.withAlpha (showMaster ? 0.9f : 0.0f));
            if (showMaster)   // contour de l'original par-dessus : on voit ce que le master a ajouté
            {
                const int cols = (int) orig.size() / 2;
                const float x0 = r.getX() + 6.0f, w = r.getWidth() - 12.0f;
                Path top, bot;
                for (int c = 0; c < cols; ++c)
                {
                    const float x = x0 + w * ((float) c + 0.5f) / cols, a = orig[(size_t) c * 2] * half;
                    if (c == 0) { top.startNewSubPath (x, mid - a); bot.startNewSubPath (x, mid + a); } else { top.lineTo (x, mid - a); bot.lineTo (x, mid + a); }
                }
                g.setColour (Colours::white.withAlpha (0.55f));
                g.strokePath (top, PathStrokeType (1.0f)); g.strokePath (bot, PathStrokeType (1.0f));
            }
        }
        else draw (orig, Colour (0xff7d7a90).withAlpha (0.55f), Colour (0xffb9b6c9).withAlpha (0.85f));
        if (orig.empty())
        {
            g.setColour (textDimmer); g.setFont (font (12.0f));
            g.drawText (tr ("Ouvre un morceau pour commencer."), getLocalBounds(), Justification::centred);
            return;
        }
        if (playhead)
        {
            const float x = r.getX() + 6.0f + (r.getWidth() - 12.0f) * (float) playhead();
            g.setColour (Colours::white.withAlpha (0.9f)); g.drawVerticalLine ((int) x, r.getY() + 3, r.getBottom() - 3);
            g.setColour (violetHi.withAlpha (0.25f)); g.fillRect (x - 2.0f, r.getY() + 3, 5.0f, r.getHeight() - 6.0f);
        }
    }
    void mouseDown (const MouseEvent& e) override { seek (e); }
    void mouseDrag (const MouseEvent& e) override { seek (e); }
private:
    void seek (const MouseEvent& e) { if (onSeek && ! orig.empty()) onSeek (jlimit (0.0, 1.0, (e.position.x - 6.0) / jmax (1.0, getWidth() - 12.0))); }
};

class SpecView : public Component
{
public:
    std::vector<float> before, after, target;
    double fs = 44100.0;
    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (inkWell); g.fillRoundedRectangle (r, 10.0f);
        g.setColour (border.withAlpha (0.6f)); g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);
        auto plot = r.reduced (10.0f, 8.0f); plot.removeFromBottom (14.0f);
        const float yMin = -42.0f, yMax = 12.0f;
        const auto X = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / 25.0) / std::log (20000.0 / 25.0)); };
        const auto Y = [&] (float db) { return plot.getBottom() - plot.getHeight() * (jlimit (yMin, yMax, db) - yMin) / (yMax - yMin); };
        g.setFont (font (9.5f));
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            g.setColour (Colours::white.withAlpha (0.06f)); g.drawVerticalLine ((int) X (f), plot.getY(), plot.getBottom());
            g.setColour (textDimmer); g.drawText (f >= 1000 ? String (f / 1000.0, f == 1000.0 || f == 2000.0 || f == 5000.0 || f == 10000.0 ? 0 : 1) + "k" : String ((int) f),
                                                  Rectangle<int> ((int) X (f) - 20, (int) plot.getBottom() + 1, 40, 12), Justification::centred);
        }
        for (float db = -40.0f; db <= 10.0f; db += 10.0f) { g.setColour (Colours::white.withAlpha (db == 0.0f ? 0.12f : 0.045f)); g.drawHorizontalLine ((int) Y (db), plot.getX(), plot.getRight()); }

        const auto path = [&] (const std::vector<float>& v, bool close)
        {
            Path p; bool started = false; float lastX = 0;
            for (int b = 0; b < master::kBands && b < (int) v.size(); ++b)
            {
                const double f = master::bandCentre (b);
                if (f > fs * 0.5 * 0.9 || v[(size_t) b] < -100.0f) continue;
                const float x = X (f), y = Y (v[(size_t) b]);
                if (! started) { p.startNewSubPath (x, y); started = true; } else p.lineTo (x, y);
                lastX = x;
            }
            if (started && close) { p.lineTo (lastX, plot.getBottom()); p.lineTo (X (master::bandCentre (0)), plot.getBottom()); p.closeSubPath(); }
            return p.createPathWithRoundedCorners (6.0f);
        };
        if (! target.empty())
        {
            Path t = path (target, false), d;
            const float dl[] = { 4.0f, 4.0f };
            PathStrokeType (1.2f).createDashedStroke (d, t, dl, 2);
            g.setColour (Colours::white.withAlpha (0.4f)); g.fillPath (d);
        }
        if (! before.empty())
        {
            g.setColour (Colour (0xffb9b6c9).withAlpha (after.empty() ? 0.9f : 0.5f));
            g.strokePath (path (before, false), PathStrokeType (1.5f));
        }
        if (! after.empty())
        {
            g.setGradientFill (ColourGradient (violet.withAlpha (0.35f), 0, plot.getY(), violet.withAlpha (0.0f), 0, plot.getBottom(), false));
            g.fillPath (path (after, true));
            g.setColour (violet.withAlpha (0.35f)); g.strokePath (path (after, false), PathStrokeType (5.0f));
            g.setColour (violetHi); g.strokePath (path (after, false), PathStrokeType (1.8f));
        }
        else if (before.empty())
        {
            g.setColour (textDimmer); g.setFont (font (11.0f));
            g.drawText (tr ("Ouvre un morceau pour commencer."), getLocalBounds(), Justification::centred);
        }
        // légende
        g.setFont (font (9.5f, true));
        float lx = plot.getRight() - 4.0f;
        const auto legend = [&] (const String& s, Colour c)
        {
            const float w = font (9.5f, true).getStringWidthFloat (s);
            lx -= w; g.setColour (c); g.drawText (s, Rectangle<float> (lx, plot.getY() + 1, w + 2, 12.0f), Justification::centredLeft);
            lx -= 14.0f; g.fillRect (lx, plot.getY() + 6.0f, 9.0f, 2.0f); lx -= 8.0f;
        };
        if (! after.empty()) legend (tr ("Après"), violetHi);
        if (! before.empty()) legend (tr ("Avant"), Colour (0xffb9b6c9));
        if (! target.empty()) legend (tr ("Cible"), Colours::white.withAlpha (0.55f));
    }
};

class ReportView : public Component
{
public:
    master::Analysis before;
    std::shared_ptr<Mastered> m;
    float targetLufs = -14.0f;
    bool refActive = false;
    void paint (Graphics& g) override
    {
        const bool hasAfter = m && m->ok;
        const int x0 = 10, colB = 212, colA = 284, w = getWidth();
        g.setFont (font (10.0f, true, 0.1f)); g.setColour (textDimmer);
        g.drawText (upper (tr ("Avant")), colB - 60, 0, 60, 16, Justification::centredRight);
        if (hasAfter) { g.setColour (champagne); g.drawText (upper (tr ("Après")), colA - 60, 0, 60, 16, Justification::centredRight); }
        const auto& A = before; const auto* B = hasAfter ? &m->rep.after : nullptr;
        struct Row { String label; String a, b; bool bad; };
        const auto f1 = [] (double v) { return String (v, 1); };
        std::vector<Row> rows = {
            { tr ("Volume intégré"), f1 (A.lufs) + " LUFS", B ? f1 (B->lufs) + " LUFS" : "", false },
            { tr ("Pic réel (true peak)"), f1 (A.truePeakDb) + " dBTP", B ? f1 (B->truePeakDb) + " dBTP" : "", B && B->truePeakDb > -0.95 },
            { tr ("Plage de volume (LRA)"), f1 (A.lra) + " LU", B ? f1 (B->lra) + " LU" : "", false },
            { tr ("Écart pic / volume (PLR)"), f1 (A.plr()) + " dB", B ? f1 (B->plr()) + " dB" : "", false },
            { tr ("Corrélation stéréo"), String (A.correlation, 2), B ? String (B->correlation, 2) : "", false } };
        int y = 17;
        for (auto& rw : rows)
        {
            g.setFont (font (11.0f)); g.setColour (textDim);
            g.drawText (rw.label, x0, y, 150, 20, Justification::centredLeft);
            g.setColour (textDimmer); g.setFont (font (11.0f, true));
            g.drawText (rw.a, colB - 76, y, 76, 20, Justification::centredRight);
            if (B) { g.setColour (rw.bad ? amber : champagne); g.drawText (rw.b, colA - 76, y, 76, 20, Justification::centredRight); }
            g.setColour (Colours::white.withAlpha (0.05f)); g.drawHorizontalLine (y + 20, (float) x0, (float) colA);
            y += 21;
        }
        g.setFont (font (10.5f)); g.setColour (textDim);
        if (hasAfter)
        {
            const auto& rp = m->rep;
            g.drawText (tr ("Compression multibande") + String::formatted (": %.1f / %.1f / %.1f dB", rp.bandGrDb[0], rp.bandGrDb[1], rp.bandGrDb[2])
                            + (m->settings.content == 1 ? "  |  De-esser" + String::formatted (": %.1f dB", rp.deEssGrDb) : String()), x0, y + 3, colA - x0, 14, Justification::centredLeft);
            g.drawText (tr ("Limiteur") + String::formatted (": %.1f dB ", rp.limiterAvgGrDb) + tr ("moy.") + String::formatted (", max %.1f dB  |  ", rp.limiterMaxGrDb)
                            + tr ("Cible") + String::formatted (" %.1f LUFS", m->settings.targetLufs), x0, y + 17, colA - x0 + 10, 14, Justification::centredLeft);
        }
        else
        {
            g.setColour (textDimmer);
            g.drawText (tr ("Aucun mastering pour l'instant."), x0, y + 3, colA - x0 + 10, 14, Justification::centredLeft);
        }
        auto right = Rectangle<int> (colA + 22, 2, w - colA - 22 - 8, getHeight() - 4);
        std::vector<master::Warning> warns;
        if (hasAfter) warns = m->rep.warnings;
        else
        {
            if (A.lufs > -9.0) warns.push_back ({ master::WarnAlreadyLoud, (float) A.lufs });
            if (A.clippedSamples > 3) warns.push_back ({ master::WarnClippedInput, (float) A.clippedSamples });
            if (A.correlation < 0.0f) warns.push_back ({ master::WarnPhase, A.correlation });
            if (A.seconds < 10.0) warns.push_back ({ master::WarnShortFile, (float) A.seconds });
        }
        AttributedString as; as.setWordWrap (AttributedString::byWord);
        const auto add = [&] (const String& s, Colour c) { as.append (String::fromUTF8 ("\xe2\x80\xa2 ") + s + "\n", font (10.0f), c); };
        for (auto& w2 : warns)
        {
            using namespace master;
            switch (w2.code)
            {
                case WarnAlreadyLoud:   add (tr ("Fichier déjà très fort (") + String (w2.value, 1) + tr (" LUFS) : peu de marge, le résultat restera proche de l'original."), amber); break;
                case WarnClippedInput:  add (tr ("Écrêtage détecté en entrée (") + String ((int) w2.value) + tr (" échantillons) : il sera conservé, mieux vaut ré-exporter le mix."), amber); break;
                case WarnPhase:         add (tr ("Corrélation négative : problème de phase, le morceau sonnera creux en mono."), amber); break;
                case WarnNearMono:      add (tr ("Morceau quasi mono : le réglage Largeur n'aura presque aucun effet."), textDim); break;
                case WarnHeavyLimiting: add (tr ("Limitation forte (") + String (w2.value, 1) + tr (" dB en moyenne) : baisse le volume cible ou la glue."), amber); break;
                case WarnTargetMissed:  add (tr ("Volume cible non atteint (") + String (w2.value, 1) + tr (" LUFS)."), amber); break;
                case WarnShortFile:     add (tr ("Morceau très court : l'analyse est moins fiable."), textDim); break;
                default: break;
            }
        }
        if (refActive) add (tr ("Référence active : la courbe de la référence remplace celle du style."), good);
        as.draw (g, right.toFloat());
    }
};

// ------------------------------------------------------------------ MainComponent
MainComponent::MainComponent()
{
    setSize (designW, designH);
    setLookAndFeel (&lnf);
    setOpaque (true);

    trackZone = std::make_unique<DropZone>(); refZone = std::make_unique<DropZone>();
    wave = std::make_unique<WaveView>(); spec = std::make_unique<SpecView>(); report = std::make_unique<ReportView>();
    playBtn = std::make_unique<TransportButton>();
    for (Component* c : { (Component*) trackZone.get(), (Component*) refZone.get(), (Component*) wave.get(), (Component*) spec.get(), (Component*) report.get(), (Component*) playBtn.get() })
        addAndMakeVisible (c);

    trackZone->onClick = [this] { chooseTrack(); };
    refZone->onClick = [this] { chooseReference(); };
    wave->playhead = [this] { return player ? player->fraction() : 0.0; };
    wave->onSeek = [this] (double f) { if (player) player->seek (f); repaint(); };
    playBtn->onClick = [this] { togglePlay(); };

    for (auto* b : { &abOrig, &abMaster, &exportBtn, &langFr, &langEn })
        addAndMakeVisible (b);
    for (auto& c : chips) addAndMakeVisible (c);
    for (int i = 0; i < 2; ++i)
    {
        auto& c = contentBtn[i];
        addAndMakeVisible (c); c.setClickingTogglesState (true); c.setRadioGroupId (5);
        c.onClick = [this, i] { setContentIndex (i); };
    }
    contentBtn[0].setToggleState (true, dontSendNotification);
    addAndMakeVisible (matchToggle); addAndMakeVisible (refToggle); addAndMakeVisible (formatBox); addAndMakeVisible (platformBox);
    addAndMakeVisible (roomBtn); roomBtn.setClickingTogglesState (true);
    roomBtn.onClick = [this] { markDirty(); };
    platformBox.onChange = [this] { setPlatform (platformBox.getSelectedId()); };

    abOrig.setClickingTogglesState (true); abMaster.setClickingTogglesState (true);
    abOrig.setRadioGroupId (2); abMaster.setRadioGroupId (2);
    abOrig.onClick = [this] { setAB (false); }; abMaster.onClick = [this] { setAB (true); };
    langFr.setClickingTogglesState (true); langEn.setClickingTogglesState (true);
    langFr.setRadioGroupId (4); langEn.setRadioGroupId (4);
    langFr.setButtonText ("FR"); langEn.setButtonText ("EN");
    langFr.setToggleState (true, dontSendNotification);
    langFr.onClick = [this] { setEnglish (false); }; langEn.onClick = [this] { setEnglish (true); };

    exportBtn.getProperties().set ("primary", true);
    masterBtn.setVisible (false);
    exportBtn.onClick = [this] { startExport(); };

    matchToggle.setToggleState (true, dontSendNotification);
    matchToggle.onClick = [this] { updatePlaybackGains(); };
    refToggle.onClick = [this]
    {
        if (refToggle.getToggleState() && reference && reference->ok)
            knobs[0].setValue (jlimit (-20.0, -6.0, std::round (reference->an.lufs * 2.0) / 2.0), dontSendNotification);
        markDirty(); repaint();
    };

    for (int i = 0; i < master::kNumStyles; ++i)
    {
        chips[i].setClickingTogglesState (true);
        chips[i].setRadioGroupId (3);
        chips[i].onClick = [this, i] { setStyleIndex (i); };
    }
    struct K { double lo, hi, step; };
    const K ranges[5] = { { -20, -6, 0.5 }, { 0, 100, 1 }, { 0, 100, 1 }, { -3, 3, 0.1 }, { 0, 150, 1 } };
    for (int i = 0; i < 5; ++i)
    {
        auto& k = knobs[i];
        k.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        k.setTextBoxStyle (Slider::NoTextBox, true, 0, 0);
        k.setRotaryParameters (MathConstants<float>::pi * 1.2f, MathConstants<float>::pi * 2.8f, true);
        k.setRange (ranges[i].lo, ranges[i].hi, ranges[i].step);
        k.setDoubleClickReturnValue (false, 0.0);
        k.onValueChange = [this, i] { if (i == 0) platformBox.setSelectedId (1, dontSendNotification); markDirty(); repaint (342, 500, 440, 124); };
        addAndMakeVisible (k);
    }
    formatBox.addItem ("24 bits", 1); formatBox.addItem ("16 bits", 2); formatBox.addItem ("32 float", 3); formatBox.addItem ("FLAC 24", 4);
    formatBox.setSelectedId (1, dontSendNotification);

    player = std::make_unique<Player>();
    setStyleIndex (0);
    dirty = false;
    relabel();
    setStatus (tr ("Ouvre un morceau pour commencer."));
    updateEnabled();
    resized();
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    cancelFlag = true;
    pool.removeAllJobs (true, 10000);
    if (deviceManager) { deviceManager->removeAudioCallback (sourcePlayer.get()); sourcePlayer->setSource (nullptr); }
    setLookAndFeel (nullptr);
}

void MainComponent::relabel()
{
    const bool en = englishRef();
    for (int i = 0; i < master::kNumStyles; ++i) chips[i].setButtonText (CharPointer_UTF8 (en ? master::style (i).nameEn : master::style (i).nameFr));
    contentBtn[0].setButtonText (tr ("INSTRU")); contentBtn[1].setButtonText (tr ("AVEC VOIX"));
    abOrig.setButtonText (tr ("ORIGINAL")); abMaster.setButtonText (tr ("MASTER"));
    exportBtn.setButtonText (tr ("EXPORTER…"));
    matchToggle.setButtonText (tr ("Volume égalisé"));
    refToggle.setButtonText (tr ("Utiliser comme référence"));
    const int sel = formatBox.getSelectedId();
    formatBox.clear (dontSendNotification);
    formatBox.addItem (tr ("24 bits"), 1); formatBox.addItem (tr ("16 bits"), 2); formatBox.addItem (tr ("32 float"), 3); formatBox.addItem ("FLAC 24", 4);
    formatBox.setSelectedId (sel > 0 ? sel : 1, dontSendNotification);
    const int psel = platformBox.getSelectedId();
    platformBox.clear (dontSendNotification);
    platformBox.addItem (tr ("Plateforme : libre"), 1); platformBox.addItem ("Spotify · YouTube  -14", 2);
    platformBox.addItem ("Apple Music  -16", 3); platformBox.addItem ("Club · DJ  -9", 4);
    platformBox.setSelectedId (psel > 0 ? psel : 1, dontSendNotification);
    roomBtn.setButtonText (tr ("Place voix"));
    trackZone->hint = tr ("Glisse un fichier audio ici (WAV, MP3, FLAC, AIFF) ou clique pour parcourir");
    refZone->hint = tr ("Glisse un morceau de référence (optionnel)");
    langFr.setToggleState (! en, dontSendNotification); langEn.setToggleState (en, dontSendNotification);
    repaint();
}

void MainComponent::setEnglish (bool en) { englishRef() = en; relabel(); repaint(); }

void MainComponent::setStatus (const String& s, bool error) { statusText = s; statusError = error; repaint (0, 630, designW, 30); }

void MainComponent::paint (Graphics& g)
{
    g.fillAll (bg0);
    g.setGradientFill (ColourGradient (bg1, 0, 0, bg0, 0, (float) designH, false));
    g.fillAll();
    // bandeau galaxie, très discret
    {
        const auto& img = galaxyImage();
        g.saveState();
        g.reduceClipRegion (0, 0, designW, 70);
        g.setOpacity (0.16f);
        g.drawImage (img, Rectangle<float> (0, -150, (float) designW, (float) designW * img.getHeight() / img.getWidth()), RectanglePlacement::stretchToFit);
        g.setGradientFill (ColourGradient (Colours::transparentBlack, 0, 0, bg1, 0, 70, false));
        g.fillRect (0, 0, designW, 70);
        g.restoreState();
    }
    const auto& logo = logoImage();
    if (logo.isValid()) g.drawImage (logo, Rectangle<float> (20.0f, 8.0f, 48.0f * logo.getWidth() / logo.getHeight(), 48.0f), RectanglePlacement::centred);
    g.setColour (text); g.setFont (displayFont (22.0f, 0.28f));
    g.drawText ("MASTER", 148, 14, 200, 30, Justification::centredLeft);
    g.setColour (textDimmer); g.setFont (font (10.5f, true, 0.18f));
    g.drawText (upper (tr ("Mastering assisté")), 148, 38, 240, 14, Justification::centredLeft);

    const Rectangle<int> cards[] = { { 20, 76, 292, 214 }, { 324, 76, 656, 214 }, { 20, 302, 470, 186 }, { 502, 302, 478, 186 }, { 20, 500, 310, 124 }, { 342, 500, 440, 124 }, { 794, 500, 186, 124 } };
    const char* titles[] = { "MORCEAU", "FORME D'ONDE", "SPECTRE", "RAPPORT", "STYLE", "RÉGLAGES", "" };
    for (int i = 0; i < 7; ++i)
    {
        drawGlass (g, cards[i].toFloat());
        if (*titles[i]) cardTitle (g, cards[i], String::fromUTF8 (titles[i]));
    }
    g.setColour (textDim); g.setFont (font (10.5f, true, 0.16f));
    g.drawText (upper (tr ("RÉFÉRENCE (OPTIONNEL)")), 36, 76 + 108, 260, 16, Justification::centredLeft);

    // potards : libellés et valeurs
    const char* names[5] = { "Volume", "Glue", "Punch", "Brillance", "Largeur" };
    for (int i = 0; i < 5; ++i)
    {
        const auto b = knobs[i].getBounds();
        g.setColour (text); g.setFont (font (11.5f, true, 0.04f));
        g.drawText (tr (names[i]), b.getX() - 10, b.getBottom() + 1, b.getWidth() + 20, 15, Justification::centred);
        const double v = knobs[i].getValue();
        String s;
        if (i == 0) s = String (v, 1) + " LUFS"; else if (i == 3) s = (v > 0 ? "+" : "") + String (v, 1) + " dB"; else s = String ((int) v) + " %";
        g.setColour (textDim); g.setFont (font (10.5f));
        g.drawText (s, b.getX() - 10, b.getBottom() + 15, b.getWidth() + 20, 14, Justification::centred);
    }
    if (busy)
    {
        auto bar = Rectangle<float> (808.0f, 556.0f, 158.0f, 6.0f);
        g.setColour (Colours::white.withAlpha (0.08f)); g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (violet); g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * progress.load()), 3.0f);
    }
    g.setColour (statusError ? amber : textDim); g.setFont (font (11.0f));
    g.drawText (statusText, 24, 632, 760, 20, Justification::centredLeft);
    g.setColour (textDimmer); g.setFont (font (10.0f));
    g.drawText (String::fromUTF8 ("PAR RICCHBEATZ  ·  MASTER 1.0"), designW - 280, 632, 256, 20, Justification::centredRight);

    // temps de lecture
    if (track && track->ok)
    {
        const double fr = player ? player->fraction() : 0.0;
        g.setColour (textDim); g.setFont (font (11.5f, true));
        g.drawText (fmtTime (fr * track->audio->seconds()) + " / " + fmtTime (track->audio->seconds()), 326 + 14 + 52, 76 + 214 - 46, 110, 28, Justification::centredLeft);
    }
}

void MainComponent::resized()
{
    if (! trackZone || ! report || ! playBtn) return;   // appelé pendant la construction
    trackZone->setBounds (34, 76 + 32, 264, 64);
    refZone->setBounds (34, 76 + 128, 264, 46);
    refToggle.setBounds (34, 76 + 180, 264, 22);
    wave->setBounds (338, 76 + 32, 628, 118);
    playBtn->setBounds (338, 76 + 214 - 46, 44, 30);
    abOrig.setBounds (338 + 52 + 116 + 120, 76 + 214 - 46, 96, 30);
    abMaster.setBounds (338 + 52 + 116 + 120 + 98, 76 + 214 - 46, 96, 30);
    matchToggle.setBounds (338 + 628 - 150, 76 + 214 - 42, 150, 22);
    spec->setBounds (34, 302 + 28, 442, 186 - 40);
    report->setBounds (516, 302 + 24, 450, 186 - 34);
    for (int i = 0; i < master::kNumStyles; ++i)
        chips[i].setBounds (34 + (i % 3) * 98, 500 + 34 + (i / 3) * 40, 92, 32);
    for (int i = 0; i < 5; ++i)
        knobs[i].setBounds (342 + 18 + i * 82, 500 + 28, 58, 58);
    contentBtn[0].setBounds (150, 500 + 4, 82, 24);
    contentBtn[1].setBounds (234, 500 + 4, 88, 24);
    platformBox.setBounds (808, 500 + 10, 158, 26);
    roomBtn.setBounds (230, 500 + 74, 92, 32);
    exportBtn.setBounds (808, 500 + 44, 158, 38);
    formatBox.setBounds (808, 500 + 90, 158, 26);
    langFr.setBounds (designW - 20 - 76, 22, 36, 24);
    langEn.setBounds (designW - 20 - 38, 22, 36, 24);
}

// ------------------------------------------------------------------ Réglages
master::Settings MainComponent::currentSettings() const
{
    master::Settings s;
    s.style = styleIndex;
    s.targetLufs = (float) knobs[0].getValue();
    s.glue = (float) knobs[1].getValue() / 100.0f;
    s.punch = (float) knobs[2].getValue() / 100.0f;
    s.bright = (float) knobs[3].getValue() / 3.0f;
    s.width = (float) knobs[4].getValue() / 100.0f;
    s.ceilingDb = -1.0f;
    s.content = contentIndex;
    s.vocalRoom = roomBtn.getToggleState() && contentIndex == 0;
    return s;
}

void MainComponent::setStyleIndex (int i)
{
    styleIndex = jlimit (0, master::kNumStyles - 1, i);
    for (int k = 0; k < master::kNumStyles; ++k) chips[k].setToggleState (k == styleIndex, dontSendNotification);
    const auto s = master::settingsForStyle (styleIndex);
    knobs[0].setValue (s.targetLufs, dontSendNotification);
    platformBox.setSelectedId (1, dontSendNotification);
    knobs[1].setValue (s.glue * 100.0, dontSendNotification);
    knobs[2].setValue (s.punch * 100.0, dontSendNotification);
    knobs[3].setValue (s.bright * 3.0, dontSendNotification);
    knobs[4].setValue (s.width * 100.0, dontSendNotification);
    if (refToggle.getToggleState() && reference && reference->ok)
        knobs[0].setValue (jlimit (-20.0, -6.0, std::round (reference->an.lufs * 2.0) / 2.0), dontSendNotification);
    markDirty();
    repaint();
}

void MainComponent::setPlatform (int id)
{
    static const double lufs[] = { 0.0, 0.0, -14.0, -16.0, -9.0 };
    if (id >= 2 && id <= 4) { knobs[0].setValue (lufs[id], dontSendNotification); markDirty(); repaint(); }
}

void MainComponent::setContentIndex (int i)
{
    contentIndex = jlimit (0, 1, i);
    for (int k = 0; k < 2; ++k) contentBtn[k].setToggleState (k == contentIndex, dontSendNotification);
    roomBtn.setVisible (contentIndex == 0);
    markDirty();
    repaint();
}

void MainComponent::markDirty() { requestLive (false); }

// Écoute en direct : tout changement de réglage relance (après 180 ms) un aperçu autour de la tête de lecture,
// puis le rendu exact du morceau entier. Les calculs obsolètes sont annulés via liveGen.
void MainComponent::requestLive (bool immediate)
{
    if (! autoLive || ! track || ! track->ok) return;
    ++liveGen;
    livePending = true;
    liveDueAt = Time::getMillisecondCounter() + (immediate ? 0u : 180u);
    liveComputing = true;
    if (mastered) mastered->full = false;
    setStatus (tr ("Calcul du master…"));
    updateEnabled();
}

void MainComponent::launchLive()
{
    livePending = false;
    if (! track || ! track->ok) return;
    const int gen = liveGen.load();
    const auto s = currentSettings();
    const bool useRef = refToggle.getToggleState() && reference && reference->ok;
    const auto refAn = useRef ? reference->an : master::Analysis();
    auto in = track->audio; auto an = track->an;
    const int64 fs = (int64) in->fs, total = in->size();
    const int64 pos = player ? player->pos.load() : 0;
    const int64 winLen = 24 * fs;
    const bool needPreview = total > 2 * winLen;
    const int64 from = jlimit ((int64) 0, jmax ((int64) 0, total - winLen), pos - fs);
    WeakReference<MainComponent> self (this);
    runJob ([self, this, in, an, s, useRef, refAn, gen, from, winLen, needPreview]
    {
        const auto alive = [this, gen] (float) { return liveGen.load() == gen; };
        if (needPreview)
        {
            auto pre = doMaster (in, an, s, useRef, refAn, alive, from, winLen);
            if (liveGen.load() != gen) return;
            MessageManager::callAsync ([self, pre, gen, useRef] { if (self != nullptr) self->applyLive (pre, gen, useRef); });
        }
        auto full = doMaster (in, an, s, useRef, refAn, alive);
        if (liveGen.load() != gen) return;
        MessageManager::callAsync ([self, full, gen, useRef] { if (self != nullptr) self->applyLive (full, gen, useRef); });
    });
}

void MainComponent::setAB (bool m)
{
    const bool useM = m && mastered && mastered->ok;
    if (player) player->useMaster = useM;
    abMaster.setToggleState (useM, dontSendNotification);
    abOrig.setToggleState (! useM, dontSendNotification);
    wave->showMaster = useM;
    repaint();
}

void MainComponent::updatePlaybackGains()
{
    float gO = 1.0f, gM = 1.0f;
    if (track && mastered && mastered->ok && matchToggle.getToggleState())
    {
        const double lo = track->an.lufs, lm = mastered->rep.after.lufs, ref = jmin (lo, lm);
        gO = (float) std::pow (10.0, (ref - lo) / 20.0);
        gM = (float) std::pow (10.0, (ref - lm) / 20.0);
    }
    if (player) { player->gO = gO; player->gM = gM; }
}

void MainComponent::togglePlay()
{
    if (! track || ! track->ok) { setStatus (tr ("Charge d'abord un morceau."), true); return; }
    if (! audioReady)
    {
        deviceManager = std::make_unique<AudioDeviceManager>();
        const auto err = deviceManager->initialiseWithDefaultDevices (0, 2);
        if (err.isNotEmpty() || deviceManager->getCurrentAudioDevice() == nullptr) { setStatus (tr ("Aucun périphérique audio disponible."), true); deviceManager.reset(); return; }
        sourcePlayer = std::make_unique<AudioSourcePlayer>();
        sourcePlayer->setSource (player.get());
        deviceManager->addAudioCallback (sourcePlayer.get());
        audioReady = true;
    }
    player->playing = ! player->playing.load();
}

void MainComponent::setPlayheadForSnapshot (double f) { if (player) player->seek (f); }

// ------------------------------------------------------------------ Fichiers, glisser-déposer
static bool isAudioFile (const File& f) { return f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg;m4a;aac;wma"); }

bool MainComponent::isInterestedInFileDrag (const StringArray& files) { return ! busy && files.size() >= 1 && isAudioFile (File (files[0])); }

void MainComponent::filesDropped (const StringArray& files, int, int y)
{
    if (busy || files.isEmpty()) return;
    const File f (files[0]);
    if (! isAudioFile (f)) return;
    if (y >= 76 + 104 && y < 76 + 214 && getMouseXYRelative().x < 320) startLoadReference (f); else startLoadTrack (f);
}

void MainComponent::chooseTrack()
{
    if (busy) return;
    chooser = std::make_unique<FileChooser> (tr ("MORCEAU"), File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a;*.aac;*.wma");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
    { const auto f = fc.getResult(); if (f != File()) startLoadTrack (f); });
}
void MainComponent::chooseReference()
{
    if (busy) return;
    chooser = std::make_unique<FileChooser> (tr ("RÉFÉRENCE (OPTIONNEL)"), File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a;*.aac;*.wma");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
    { const auto f = fc.getResult(); if (f != File()) startLoadReference (f); });
}

// ------------------------------------------------------------------ Tâches de fond
namespace
{
struct FnJob : ThreadPoolJob
{
    std::function<void()> fn;
    explicit FnJob (std::function<void()> f) : ThreadPoolJob ("job"), fn (std::move (f)) {}
    JobStatus runJob() override { fn(); return jobHasFinished; }
};

std::shared_ptr<Mastered> doMaster (std::shared_ptr<master::Stereo> in, master::Analysis an, master::Settings s, bool useRef, master::Analysis refAn,
                                    const std::function<bool (float)>& progress, int64 from, int64 len)
{
    auto res = std::make_shared<Mastered>();
    if (useRef) s.reference = &refAn;
    res->offset = from;
    res->full = (from == 0 && (len < 0 || len >= in->size()));
    std::shared_ptr<master::Stereo> src = in;
    if (! res->full)   // aperçu : le segment est masterisé avec l'analyse (EQ, niveau interne) de tout le morceau
    {
        src = std::make_shared<master::Stereo>();
        src->fs = in->fs;
        const int64 e = jmin ((int64) in->size(), from + len);
        src->l.assign (in->l.begin() + from, in->l.begin() + e);
        src->r.assign (in->r.begin() + from, in->r.begin() + e);
    }
    res->ok = master::masterAudio (*src, an, s, *(res->audio = std::make_shared<master::Stereo>()), res->rep, progress);
    s.reference = nullptr;
    res->settings = s;
    if (res->ok && res->full) res->peaks = computePeaks (*res->audio);
    return res;
}

bool writeWav (const master::Stereo& a, const File& f, int bits, const std::function<bool (float)>& progress)
{
    f.deleteFile();
    std::unique_ptr<FileOutputStream> os (f.createOutputStream());
    if (! os || os->failedToOpen()) return false;
    WavAudioFormat wav; FlacAudioFormat flac;
    std::unique_ptr<AudioFormatWriter> w (bits < 0 ? flac.createWriterFor (os.get(), a.fs, 2, -bits, {}, 5)
                                                   : wav.createWriterFor (os.get(), a.fs, 2, bits, {}, 0));
    if (! w) return false;
    os.release();
    const int n = a.size(), chunk = 1 << 16;
    AudioBuffer<float> b (2, chunk);
    Random rnd (1234);
    for (int s = 0; s < n; s += chunk)
    {
        const int len = jmin (chunk, n - s);
        for (int i = 0; i < len; ++i)
        {
            float l = a.l[(size_t) (s + i)], r = a.r[(size_t) (s + i)];
            if (bits == 16)   // dither TPDF
            {
                l += (rnd.nextFloat() - rnd.nextFloat()) / 32768.0f;
                r += (rnd.nextFloat() - rnd.nextFloat()) / 32768.0f;
            }
            b.setSample (0, i, jlimit (-1.0f, 1.0f, l)); b.setSample (1, i, jlimit (-1.0f, 1.0f, r));
        }
        if (! w->writeFromAudioSampleBuffer (b, 0, len)) return false;
        if (progress && ! progress ((float) s / n)) return false;
    }
    return true;
}
}

void MainComponent::runJob (std::function<void()> fn) { pool.addJob (new FnJob (std::move (fn)), true); }

void MainComponent::startLoadTrack (const File& f)
{
    if (busy) return;
    busy = true; cancelFlag = false; progress = 0.0f;
    trackZone->busyNow = true; trackZone->loaded = false; trackZone->hint = tr ("Chargement et analyse…");
    setStatus (tr ("Chargement et analyse…")); updateEnabled();
    WeakReference<MainComponent> self (this);
    runJob ([self, f, this]
    {
        auto res = readAndAnalyse (f, [this] (float p) { progress = p; return ! cancelFlag.load(); });
        MessageManager::callAsync ([self, res] { if (self != nullptr) self->applyTrack (res); });
    });
}
void MainComponent::startLoadReference (const File& f)
{
    if (busy) return;
    busy = true; cancelFlag = false; progress = 0.0f;
    refZone->busyNow = true;
    setStatus (tr ("Chargement et analyse…")); updateEnabled();
    WeakReference<MainComponent> self (this);
    runJob ([self, f, this]
    {
        auto res = readAndAnalyse (f, [this] (float p) { progress = p; return ! cancelFlag.load(); });
        MessageManager::callAsync ([self, res] { if (self != nullptr) self->applyReference (res); });
    });
}
void MainComponent::loadTrackSync (const File& f) { busy = true; autoLive = false; applyTrack (readAndAnalyse (f, nullptr)); }
void MainComponent::loadReferenceSync (const File& f) { busy = true; autoLive = false; applyReference (readAndAnalyse (f, nullptr)); }

void MainComponent::applyTrack (std::shared_ptr<Loaded> res)
{
    busy = false; trackZone->busyNow = false;
    trackZone->hint = tr ("Glisse un fichier audio ici (WAV, MP3, FLAC, AIFF) ou clique pour parcourir");
    if (! res || ! res->ok) { setStatus (cancelFlag ? tr ("Annulé.") : (res && res->error.isNotEmpty() ? tr (res->error) : tr ("Fichier illisible ou trop long.")), true); updateEnabled(); repaint(); return; }
    track = res; mastered.reset(); dirty = false; usedReference = false;
    trackZone->loaded = true; trackZone->name = res->name; trackZone->meta = res->meta;
    wave->orig = res->peaks; wave->mast.clear();
    spec->fs = res->audio->fs;
    spec->before = master::normaliseBandsPublic (res->an.bands); spec->after.clear();
    spec->target = (refToggle.getToggleState() && reference && reference->ok) ? master::normaliseBandsPublic (reference->an.bands) : master::genericTargetNormalised();
    report->before = res->an; report->m.reset(); report->refActive = refToggle.getToggleState() && reference && reference->ok;
    player->setBuffers (res->audio, nullptr); player->seek (0.0);
    setAB (false); updatePlaybackGains(); updateEnabled();
    setStatus (tr ("Prêt. Choisis un style puis lance le mastering."));
    liveComputing = false; liveGen++;
    requestLive (true);
    repaint();
}

void MainComponent::applyReference (std::shared_ptr<Loaded> res)
{
    busy = false; refZone->busyNow = false;
    if (! res || ! res->ok) { setStatus (cancelFlag ? tr ("Annulé.") : (res && res->error.isNotEmpty() ? tr (res->error) : tr ("Fichier illisible ou trop long.")), true); updateEnabled(); repaint(); return; }
    reference = res;
    refZone->loaded = true; refZone->name = res->name; refZone->meta = res->meta;
    refToggle.setToggleState (true, dontSendNotification);
    knobs[0].setValue (jlimit (-20.0, -6.0, std::round (res->an.lufs * 2.0) / 2.0), dontSendNotification);
    spec->target = master::normaliseBandsPublic (res->an.bands);
    report->refActive = true;
    updateEnabled(); markDirty();
    if (! (mastered && mastered->ok)) setStatus (tr ("Référence active : la courbe de la référence remplace celle du style."));
    repaint();
}

void MainComponent::startMaster() { requestLive (true); }

void MainComponent::masterSync()
{
    if (! track || ! track->ok) return;
    const auto s = currentSettings();
    const bool useRef = refToggle.getToggleState() && reference && reference->ok;
    ++liveGen; livePending = false;
    applyLive (doMaster (track->audio, track->an, s, useRef, useRef ? reference->an : master::Analysis(), nullptr), liveGen.load(), useRef);
}

void MainComponent::applyLive (std::shared_ptr<Mastered> res, int gen, bool useRef)
{
    if (gen != liveGen.load()) return;   // résultat obsolète
    if (! res || ! res->ok)
    {
        liveComputing = false;
        const bool silent = res && ! res->rep.warnings.empty() && res->rep.warnings.front().code == master::WarnSilent;
        setStatus (silent ? tr ("Morceau silencieux : rien à masteriser.") : tr ("Mastering impossible."), true);
        updateEnabled(); repaint(); return;
    }
    const bool first = ! (mastered && mastered->ok);
    const bool wasMaster = player->useMaster.load();
    usedReference = useRef;
    mastered = res;
    spec->after = master::normaliseBandsPublic (res->rep.after.bands);
    spec->target = res->rep.targetBands;
    report->m = res; report->targetLufs = res->settings.targetLufs; report->refActive = usedReference;
    player->setBuffers (track->audio, res->audio, res->offset);
    if (res->full) { wave->mast = res->peaks; liveComputing = false; }
    updatePlaybackGains(); updateEnabled(); setAB (first ? true : wasMaster);
    setStatus (res->full ? tr ("Master prêt. Chaque réglage s'écoute en direct (A/B à volume égalisé).")
                         : tr ("Aperçu en direct… calcul exact en cours."));
    if (res->full && pendingExportBits > 0)
    {
        const auto f = pendingExport; const int bits = pendingExportBits; pendingExportBits = 0;
        exportToFile (f, bits);
    }
    repaint();
}

void MainComponent::startExport()
{
    if (busy || ! mastered || ! mastered->ok || ! track) return;
    const int fmt = formatBox.getSelectedId();
    const int bits = fmt == 4 ? -24 : (fmt == 2 ? 16 : (fmt == 3 ? 32 : 24));   // négatif = FLAC
    const char* ext = bits < 0 ? "flac" : "wav";
    const File def = track->file.getParentDirectory().getChildFile (track->file.getFileNameWithoutExtension() + "_master." + ext);
    chooser = std::make_unique<FileChooser> (tr ("EXPORTER…"), def, String ("*.") + ext);
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting, [this, bits] (const FileChooser& fc)
    {
        File f = fc.getResult();
        if (f != File()) exportToFile (f, bits);
    });
}

void MainComponent::exportToFile (const File& file, int bits)
{
    if (busy || ! mastered || ! mastered->ok) return;
    File f = file;
    if (! f.hasFileExtension (bits < 0 ? "flac" : "wav")) f = f.withFileExtension (bits < 0 ? "flac" : "wav");
    if (! mastered->full)   // le fichier exporté est toujours le rendu exact : on attend la fin du calcul
    {
        pendingExport = f; pendingExportBits = bits;
        setStatus (tr ("Calcul du master… l'export démarrera ensuite."));
        return;
    }
    busy = true; cancelFlag = false; progress = 0.0f;
    setStatus (tr ("Export en cours…")); updateEnabled();
    auto audio = mastered->audio;
    WeakReference<MainComponent> self (this);
    runJob ([self, this, f, bits, audio]
    {
        const bool ok = writeWav (*audio, f, bits, [this] (float p) { progress = p; return ! cancelFlag.load(); });
        MessageManager::callAsync ([self, ok, f]
        {
            if (self == nullptr) return;
            self->busy = false; self->updateEnabled();
            self->setStatus (ok ? tr ("Export terminé : ") + f.getFullPathName() : tr ("Échec de l'export."), ! ok);
        });
    });
}

AudioBuffer<float> MainComponent::renderPlaybackForTest (double deviceFs, int blockSize, int numBlocks, bool useMaster)
{
    AudioBuffer<float> out (2, blockSize * numBlocks);
    player->prepareToPlay (blockSize, deviceFs);
    player->seek (0.0);
    setAB (useMaster);
    player->playing = true;
    for (int b = 0; b < numBlocks; ++b)
    {
        AudioBuffer<float> blk (2, blockSize);
        AudioSourceChannelInfo info (&blk, 0, blockSize);
        player->getNextAudioBlock (info);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, b * blockSize, blk, c, 0, blockSize);
    }
    player->playing = false;
    return out;
}

void MainComponent::updateEnabled()
{
    const bool has = track && track->ok;
    exportBtn.setEnabled (! busy && mastered && mastered->ok);
    abMaster.setEnabled (mastered && mastered->ok);
    abOrig.setEnabled (has);
    playBtn->setEnabled (has);
    refToggle.setEnabled (reference && reference->ok && ! busy);
    matchToggle.setEnabled (mastered && mastered->ok);
    for (auto& c : chips) c.setEnabled (! busy);
    for (auto& k : knobs) k.setEnabled (! busy);
    repaint();
}

void MainComponent::timerCallback()
{
    if (busy)
    {
        trackZone->progress = refZone->progress = progress.load();
        trackZone->repaint(); refZone->repaint();
        repaint (794, 500, 186, 124);
    }
    if (livePending && ! busy && Time::getMillisecondCounter() >= liveDueAt) launchLive();
    if (liveComputing) repaint (794, 500, 186, 124);
    if (player)
    {
        const bool p = player->playing.load();
        if (playBtn->playing != p) { playBtn->playing = p; playBtn->repaint(); }
        if (p) { wave->repaint(); repaint (326, 76 + 214 - 50, 200, 36); }
    }
}
}
