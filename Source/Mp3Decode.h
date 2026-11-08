#pragma once
#include <juce_core/juce_core.h>
#include <vector>

// Décodeur MP3 de secours (dr_mp3, domaine public) : lit les fichiers que le décodeur JUCE refuse
// (tags ID3 volumineux, pochettes, VBR atypique, MPEG-2...). Sortie : float stéréo non entrelacé.
namespace master
{
    struct DecodedAudio { std::vector<float> l, r; double fs = 0.0; int channels = 0; bool ok = false; juce::String error; };
    DecodedAudio decodeMp3 (const juce::File& f, juce::int64 maxFrames);
}
