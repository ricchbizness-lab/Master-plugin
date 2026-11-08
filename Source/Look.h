#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <BinaryData.h>
#include <map>
#include <vector>
#include <utility>

namespace ui
{
// ------------------------------------------------------------------ Palette (identique à Nebula)
inline const juce::Colour bg0 { 0xff05030b }, bg1 { 0xff10071f }, surface2 { 0xff171025 }, border { 0xff4a2b78 },
    text { 0xfffbf8ff }, textDim { 0xffc5b5e8 }, textDimmer { 0xff83729f },
    violet { 0xffa33cff }, violetHi { 0xffe0c5ff }, violetDark { 0xff5717bd }, orchid { 0xffd878ff },
    glassTop { 0xff1a102b }, glassBottom { 0xff080610 }, amber { 0xffffc46b }, good { 0xff7be3a8 };

inline juce::Typeface::Ptr typeface (int kind)
{
    static const juce::Typeface::Ptr sora = juce::Typeface::createSystemTypefaceFor (BinaryData::Sora_600SemiBold_ttf, (size_t) BinaryData::Sora_600SemiBold_ttfSize);
    static const juce::Typeface::Ptr interMed = juce::Typeface::createSystemTypefaceFor (BinaryData::Inter_500Medium_ttf, (size_t) BinaryData::Inter_500Medium_ttfSize);
    static const juce::Typeface::Ptr interSemi = juce::Typeface::createSystemTypefaceFor (BinaryData::Inter_600SemiBold_ttf, (size_t) BinaryData::Inter_600SemiBold_ttfSize);
    return kind == 0 ? interMed : (kind == 1 ? interSemi : sora);
}
inline juce::Font font (float h, bool bold = false, float tracking = 0.0f)
{
    juce::Font f (juce::FontOptions (typeface (bold ? 1 : 0)).withHeight (h));
    return tracking != 0.0f ? f.withExtraKerningFactor (tracking) : f;
}
inline juce::Font displayFont (float h, float tracking = 0.0f)
{
    juce::Font f (juce::FontOptions (typeface (2)).withHeight (h));
    return tracking != 0.0f ? f.withExtraKerningFactor (tracking) : f;
}
inline const juce::Image& logoImage()
{
    static const juce::Image img = juce::ImageFileFormat::loadFrom (BinaryData::logo_png, (size_t) BinaryData::logo_pngSize);
    return img;
}
inline const juce::Image& galaxyImage()
{
    static const juce::Image img = juce::ImageFileFormat::loadFrom (BinaryData::galaxy_jpg, (size_t) BinaryData::galaxy_jpgSize);
    return img;
}

// ------------------------------------------------------------------ Langue (FR par défaut, EN)
inline bool& englishRef() { static bool en = false; return en; }
inline juce::String upper (const juce::String& s)
{
    juce::String o;
    for (auto c : s)
    {
        static const juce::String from = juce::String::fromUTF8 ("éèêëàâîïôöùûüç"), to = juce::String::fromUTF8 ("ÉÈÊËÀÂÎÏÔÖÙÛÜÇ");
        const int k = from.indexOfChar (c);
        o += k >= 0 ? juce::String::charToString (to[k]) : juce::String::charToString (juce::CharacterFunctions::toUpperCase (c));
    }
    return o;
}
// Les clés sont les textes français ; la table donne l'anglais.
inline juce::String tr (const juce::String& fr)
{
    if (! englishRef()) return fr;
    static const std::vector<std::pair<const char*, const char*>> raw = {
        { "MORCEAU", "TRACK" }, { "RÉFÉRENCE (OPTIONNEL)", "REFERENCE (OPTIONAL)" }, { "FORME D'ONDE", "WAVEFORM" }, { "SPECTRE", "SPECTRUM" },
        { "RAPPORT", "REPORT" }, { "STYLE", "STYLE" }, { "RÉGLAGES", "SETTINGS" }, { "MASTERISER", "MASTER" }, { "EXPORTER…", "EXPORT…" },
        { "ANNULER", "CANCEL" }, { "ORIGINAL", "ORIGINAL" }, { "MASTER", "MASTER" }, { "Volume égalisé", "Match loudness" },
        { "Glisse un fichier audio ici (WAV, MP3, FLAC, AIFF) ou clique pour parcourir", "Drop an audio file here (WAV, MP3, FLAC, AIFF) or click to browse" },
        { "Glisse un morceau de référence (optionnel)", "Drop a reference track (optional)" },
        { "Utiliser comme référence", "Use as reference" },
        { "Équilibré", "Balanced" }, { "Club", "Club" }, { "Chaud", "Warm" }, { "Transparent", "Transparent" }, { "Rap / Trap", "Rap / Trap" },
        { "Volume", "Loudness" }, { "Glue", "Glue" }, { "Punch", "Punch" }, { "Brillance", "Brightness" }, { "Largeur", "Width" },
        { "Avant", "Before" }, { "Après", "After" }, { "Volume intégré", "Integrated loudness" }, { "Pic réel (true peak)", "True peak" },
        { "Plage de volume (LRA)", "Loudness range (LRA)" }, { "Écart pic / volume (PLR)", "Peak-to-loudness (PLR)" }, { "Corrélation stéréo", "Stereo correlation" },
        { "Compression multibande", "Multiband compression" }, { "Limiteur", "Limiter" }, { "moy.", "avg" },
        { "Ouvre un morceau pour commencer.", "Open a track to start." }, { "Chargement et analyse…", "Loading and analysing…" },
        { "Prêt. Choisis un style puis lance le mastering.", "Ready. Pick a style, then run the mastering." },
        { "Mastering en cours…", "Mastering…" }, { "Master terminé. Compare avec A/B (volume égalisé).", "Master done. Compare with A/B (loudness matched)." },
        { "Réglages modifiés : relance le mastering.", "Settings changed: run the mastering again." },
        { "Export en cours…", "Exporting…" }, { "Export terminé : ", "Exported: " }, { "Échec de l'export.", "Export failed." },
        { "Fichier illisible ou trop long.", "Unreadable or too long file." }, { "Format non pris en charge (WAV, AIFF, FLAC, MP3, OGG).", "Unsupported format (WAV, AIFF, FLAC, MP3, OGG)." }, { "Fichier trop court.", "File too short." }, { "Mastering impossible.", "Mastering failed." }, { "Fichier trop long (25 min maximum).", "File too long (25 min maximum)." }, { "Lecture du fichier impossible.", "Could not read the file." }, { "Annulé.", "Cancelled." }, { "Calcul du master…", "Computing master…" }, { "Aperçu en direct… calcul exact en cours.", "Live preview… exact render in progress." },
        { "Master prêt. Chaque réglage s'écoute en direct (A/B à volume égalisé).", "Master ready. Every setting is heard live (A/B, loudness matched)." },
        { "Calcul du master… l'export démarrera ensuite.", "Computing master… export will start right after." }, { "MP3 invalide ou corrompu.", "Invalid or corrupt MP3." },
        { "Morceau silencieux : rien à masteriser.", "Silent file: nothing to master." },
        { "Aucun périphérique audio disponible.", "No audio device available." },
        { "Aucun mastering pour l'instant.", "No master yet." },
        { "Charge d'abord un morceau.", "Load a track first." },
        { "Fichier déjà très fort (", "Already very loud (" }, { " LUFS) : peu de marge, le résultat restera proche de l'original.", " LUFS): little headroom, the result will stay close to the original." },
        { "Écrêtage détecté en entrée (", "Clipping detected in the input (" }, { " échantillons) : il sera conservé, mieux vaut ré-exporter le mix.", " samples): it will be kept, better re-export the mix." },
        { "Corrélation négative : problème de phase, le morceau sonnera creux en mono.", "Negative correlation: phase problem, the track will sound hollow in mono." },
        { "Morceau quasi mono : le réglage Largeur n'aura presque aucun effet.", "Near-mono track: the Width setting will have little effect." },
        { "Limitation forte (", "Heavy limiting (" }, { " dB en moyenne) : baisse le volume cible ou la glue.", " dB on average): lower the target loudness or the glue." },
        { "Volume cible non atteint (", "Target loudness not reached (" }, { " LUFS).", " LUFS)." },
        { "Morceau très court : l'analyse est moins fiable.", "Very short file: the analysis is less reliable." },
        { "Référence active : la courbe de la référence remplace celle du style.", "Reference active: its curve replaces the style's curve." },
        { "24 bits", "24-bit" }, { "16 bits", "16-bit" }, { "32 float", "32 float" },
        { "Cible", "Target" }, { "Mastering assisté", "Assisted mastering" },
        { "Fichier : ", "File: " }, { " canal", " channel" }, { " canaux", " channels" },
    };
    static const std::map<juce::String, juce::String> en = [&]
    {
        std::map<juce::String, juce::String> m;
        for (auto& p : raw) m[juce::String::fromUTF8 (p.first)] = juce::String::fromUTF8 (p.second);
        return m;
    }();
    const auto it = en.find (fr);
    return it != en.end() ? it->second : fr;
}
inline juce::String tr (const char* fr) { return tr (juce::String::fromUTF8 (fr)); }

// ------------------------------------------------------------------ Verre
inline void drawGlass (juce::Graphics& g, juce::Rectangle<float> r, float radius = 14.0f)
{
    juce::Path p;
    p.addRoundedRectangle (r, radius);
    juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 18, { 0, 7 }).drawForPath (g, p);
    g.setColour (juce::Colour (0xff07050f));
    g.fillPath (p);
    g.setGradientFill (juce::ColourGradient (glassTop.withAlpha (0.98f), r.getX(), r.getY(), glassBottom, r.getX() + r.getWidth() * 0.22f, r.getBottom(), false));
    g.fillPath (p);
    g.saveState();
    g.reduceClipRegion (r.withHeight (r.getHeight() * 0.42f).toNearestInt());
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffc8a0ff).withAlpha (0.07f), r.getX(), r.getY(), juce::Colours::transparentBlack, r.getX(), r.getCentreY(), false));
    g.fillPath (p);
    g.restoreState();
    juce::Path outer;
    outer.addRoundedRectangle (r.reduced (0.5f), radius);
    g.setColour (violet.withAlpha (0.22f));
    g.strokePath (outer, juce::PathStrokeType (2.6f));
    g.setGradientFill (juce::ColourGradient (violetHi.withAlpha (0.55f), r.getX(), r.getY(), violet.withAlpha (0.2f), r.getRight(), r.getBottom(), false));
    g.strokePath (outer, juce::PathStrokeType (0.9f));
}

inline void cardTitle (juce::Graphics& g, juce::Rectangle<int> card, const juce::String& title)
{
    g.setColour (textDim);
    g.setFont (font (10.5f, true, 0.16f));
    g.drawText (upper (tr (title)), card.getX() + 16, card.getY() + 9, card.getWidth() - 32, 16, juce::Justification::centredLeft);
}

// ------------------------------------------------------------------ Look & feel
class Lnf : public juce::LookAndFeel_V4
{
public:
    Lnf()
    {
        setColour (juce::ComboBox::backgroundColourId, surface2);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::outlineColourId, border);
        setColour (juce::ComboBox::arrowColourId, textDim);
        setColour (juce::PopupMenu::backgroundColourId, glassTop);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, violetDark);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::Slider::textBoxTextColourId, text);
    }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (12.0f); }
    juce::Font getPopupMenuFont() override { return font (12.5f); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return font (12.0f, true, 0.04f); }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s) override
    {
        const auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
        const auto c = r.getCentre();
        const float ang = a0 + pos * (a1 - a0);
        const float ringR = rad - 2.0f;

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff261a3c), c.x, c.y - rad, juce::Colour (0xff0a0714), c.x, c.y + rad, false));
        g.fillEllipse (c.x - rad + 7.0f, c.y - rad + 7.0f, (rad - 7.0f) * 2.0f, (rad - 7.0f) * 2.0f);

        juce::Path track;
        track.addCentredArc (c.x, c.y, ringR, ringR, 0.0f, a0, a1, true);
        g.setColour (juce::Colour (0xff2a1d45));
        g.strokePath (track, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path val;
        val.addCentredArc (c.x, c.y, ringR, ringR, 0.0f, a0, ang, true);
        const bool hot = s.isMouseOverOrDragging();
        g.setColour (violet.withAlpha (0.28f));
        g.strokePath (val, juce::PathStrokeType (8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setGradientFill (juce::ColourGradient (violetDark, c.x - ringR, c.y + ringR, hot ? violetHi : orchid, c.x + ringR, c.y - ringR, false));
        g.strokePath (val, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path ptr;
        const float ir = rad - 12.0f;
        ptr.startNewSubPath (c.x + std::sin (ang) * ir * 0.35f, c.y - std::cos (ang) * ir * 0.35f);
        ptr.lineTo (c.x + std::sin (ang) * ir, c.y - std::cos (ang) * ir);
        g.setColour (violetHi);
        g.strokePath (ptr, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hover, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const bool on = b.getToggleState(), enabled = b.isEnabled();
        const bool primary = b.getProperties().getWithDefault ("primary", false);
        juce::Colour fill = on ? violetDark : surface2;
        if (primary) fill = violet;
        if (hover && enabled) fill = fill.brighter (0.15f);
        if (down) fill = fill.darker (0.2f);
        if (! enabled) fill = fill.withMultipliedAlpha (0.45f);
        g.setGradientFill (juce::ColourGradient (fill.brighter (0.12f), 0, r.getY(), fill.darker (0.2f), 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour ((on || primary ? violetHi : border).withMultipliedAlpha (enabled ? (on || primary ? 0.8f : 1.0f) : 0.4f));
        g.drawRoundedRectangle (r, 7.0f, 1.0f);
    }
    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        g.setFont (getTextButtonFont (b, b.getHeight()));
        const bool on = b.getToggleState();
        const bool primary = b.getProperties().getWithDefault ("primary", false);
        g.setColour ((on || primary ? juce::Colours::white : textDim).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.45f));
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
    }
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool hover, bool) override
    {
        const auto box = juce::Rectangle<float> (2.0f, b.getHeight() * 0.5f - 8.0f, 16.0f, 16.0f);
        g.setColour (surface2.brighter (hover ? 0.15f : 0.0f));
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (b.getToggleState() ? violetHi : border);
        g.drawRoundedRectangle (box, 4.0f, 1.2f);
        if (b.getToggleState())
        {
            juce::Path tick;
            tick.startNewSubPath (box.getX() + 3.5f, box.getCentreY());
            tick.lineTo (box.getCentreX() - 0.5f, box.getBottom() - 4.0f);
            tick.lineTo (box.getRight() - 3.0f, box.getY() + 4.0f);
            g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour ((b.isEnabled() ? textDim : textDimmer));
        g.setFont (font (11.5f));
        g.drawText (b.getButtonText(), 24, 0, b.getWidth() - 24, b.getHeight(), juce::Justification::centredLeft);
    }
    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
        g.setColour (surface2.withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (border);
        g.drawRoundedRectangle (r, 7.0f, 1.0f);
        juce::Path arrow;
        const float cx = w - 14.0f, cy = h * 0.5f;
        arrow.addTriangle (cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
        g.setColour (textDim);
        g.fillPath (arrow);
    }
    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 1, box.getWidth() - 24, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }
};
}
