// Rend l'interface dans un PNG (sans fenêtre) : MasterSnapshot out.png track.wav [style] [en] [nomaster] [ref.wav]
#include "../Source/MainComponent.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 3) return 1;
    ui::MainComponent mc;
    const bool en = argc > 4 && juce::String (argv[4]) == "en";
    if (en) mc.setEnglish (true);
    mc.loadTrackSync (juce::File (argv[2]));
    if (argc > 6) mc.loadReferenceSync (juce::File (argv[6]));
    mc.setStyleIndex (argc > 3 ? juce::String (argv[3]).getIntValue() : 0);
    if (! (argc > 5 && juce::String (argv[5]) == "nomaster")) mc.masterSync();
    mc.setPlayheadForSnapshot (0.37);
    auto img = mc.createComponentSnapshot (mc.getLocalBounds(), true, 1.5f);
    juce::File out (argv[1]);
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat png;
    png.writeImageToStream (img, os);
    return 0;
}
