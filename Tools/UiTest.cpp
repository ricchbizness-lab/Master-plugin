// Test de bout en bout de l'interface (chemins asynchrones réels) sans fenêtre ni carte son.
//   UiTest track.wav out.wav
#include "../Source/MainComponent.h"
#include <cstdio>

static void pump (ui::MainComponent& mc, int maxMs)
{
    const auto t0 = juce::Time::getMillisecondCounter();
    do { juce::MessageManager::getInstance()->runDispatchLoopUntil (20); }
    while ((mc.isBusy() || mc.isComputing()) && juce::Time::getMillisecondCounter() - t0 < (juce::uint32) maxMs);
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 3) return 1;
    if (juce::String (argv[1]) == "load")   // UiTest load f1 f2 ... : chargement seul, affiche le statut
    {
        for (int i = 2; i < argc; ++i)
        {
            ui::MainComponent m;
            m.openTrack (juce::File (argv[i]));
            pump (m, 60000);
            std::printf ("%-28s -> %s | %s\n", juce::File (argv[i]).getFileName().toRawUTF8(), m.hasTrack() ? "OK" : "ECHEC", m.statusMessage().toRawUTF8());
        }
        return 0;
    }
    int fails = 0;
    const auto check = [&] (const char* n, bool ok, double v) { std::printf ("  [%s] %s (%.4f)\n", ok ? "OK " : "KO!", n, v); if (! ok) ++fails; };
    ui::MainComponent mc;
    mc.openTrack (juce::File (argv[1]));
    pump (mc, 60000);
    mc.setStyleIndex (0);
    mc.runMaster();
    pump (mc, 120000);
    check ("mastering asynchrone terminé", mc.hasFullMaster(), 0);
    if (! mc.hasMaster()) return 2;
    const auto* r = mc.lastReport();
    check ("LUFS final = cible (-14 ± 0.2)", std::abs (r->after.lufs + 14.0) < 0.2, r->after.lufs);
    check ("true peak <= -1.0 dBTP", r->after.truePeakDb <= -1.0, r->after.truePeakDb);

    // écoute en direct : un changement de style doit produire un aperçu rapide puis le rendu exact
    {
        mc.setStyleIndex (3);
        const auto t0 = juce::Time::getMillisecondCounter();
        juce::uint32 tPreview = 0, tFull = 0;
        while (juce::Time::getMillisecondCounter() - t0 < 60000 && ! tFull)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
            const auto* rr = mc.lastReport();
            if (! tPreview && rr && rr->after.seconds < 100.0 && mc.isComputing()) tPreview = juce::Time::getMillisecondCounter() - t0;
            if (mc.hasFullMaster()) tFull = juce::Time::getMillisecondCounter() - t0;
        }
        std::printf ("  live : aperçu après %u ms, rendu exact après %u ms\n", tPreview, tFull);
        check ("aperçu live < 2 s", tPreview > 0 && tPreview < 2000, tPreview);
        check ("rendu exact après changement de style", tFull > 0, tFull);
        // rafale de changements : seul le dernier réglage doit être rendu
        mc.setStyleIndex (1); mc.setStyleIndex (2); mc.setStyleIndex (4); mc.setStyleIndex (0);
        pump (mc, 60000);
        check ("rafale : rendu final cohérent (style 0 → -14 LUFS)", mc.hasFullMaster() && std::abs (mc.lastReport()->after.lufs + 14.0) < 0.2, mc.lastReport() ? mc.lastReport()->after.lufs : 0);
    }

    // export asynchrone puis relecture
    const juce::File out (argv[2]);
    mc.exportToFile (out, 24);
    pump (mc, 60000);
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (out));
    check ("fichier exporté lisible", rd != nullptr, 0);
    if (rd) { check ("export 24 bits stéréo", rd->bitsPerSample == 24 && rd->numChannels == 2, rd->bitsPerSample);
              check ("durée exportée = durée source", std::abs ((double) rd->lengthInSamples / rd->sampleRate - r->after.seconds) < 0.01, (double) rd->lengthInSamples / rd->sampleRate); }

    // autres formats d'export
    for (int bits : { 16, 32 })
    {
        const juce::File f2 = out.getSiblingFile ("ui_export_" + juce::String (bits) + ".wav");
        mc.exportToFile (f2, bits); pump (mc, 60000);
        std::unique_ptr<juce::AudioFormatReader> r2 (fm.createReaderFor (f2));
        check (("export " + juce::String (bits) + " bits relisible").toRawUTF8(), r2 && r2->bitsPerSample == (unsigned) bits && (bits != 32 || r2->usesFloatingPointData), r2 ? r2->bitsPerSample : 0);
    }
    // lecture A/B : mêmes niveaux apparents (volume égalisé), pas de coupure, bonnes fréquences d'échantillonnage
    for (double dfs : { 44100.0, 48000.0 })
    {
        const int bs = 512, nb = 200;
        auto o = mc.renderPlaybackForTest (dfs, bs, nb, false), m = mc.renderPlaybackForTest (dfs, bs, nb, true);
        double eo = 0, em = 0; float po = 0, pm = 0;
        for (int i = 0; i < bs * nb; ++i) { eo += o.getSample (0, i) * o.getSample (0, i); em += m.getSample (0, i) * m.getSample (0, i); po = juce::jmax (po, std::abs (o.getSample (0, i))); pm = juce::jmax (pm, std::abs (m.getSample (0, i))); }
        const double dDb = 10.0 * std::log10 ((em + 1e-12) / (eo + 1e-12));
        std::printf ("  lecture %.0f Hz : RMS original %.1f dB, master %.1f dB, écart %.2f dB, pics %.2f / %.2f\n", dfs, 10 * std::log10 (eo / (bs * nb)), 10 * std::log10 (em / (bs * nb)), dDb, po, pm);
        check ("lecture non silencieuse", eo > 1e-6 && em > 1e-6, eo);
        check ("A/B à volume égalisé (écart court-terme < 2 dB, hors dynamique du passage)", std::abs (dDb) < 2.0, dDb);
        check ("pas d'écrêtage en lecture", po <= 1.0f && pm <= 1.0f, juce::jmax (po, pm));
    }
    std::printf (fails ? "ECHECS: %d\n" : "interface : tous les tests passent\n", fails);
    return fails;
}
