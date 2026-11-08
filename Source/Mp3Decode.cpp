#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "third_party/dr_mp3.h"
#include "Mp3Decode.h"

namespace master
{
    DecodedAudio decodeMp3 (const juce::File& f, juce::int64 maxFrames)
    {
        DecodedAudio out;
        juce::MemoryBlock data;
        if (! f.loadFileAsData (data) || data.getSize() < 128) { out.error = "Lecture du fichier impossible."; return out; }

        drmp3 mp3;
        if (! drmp3_init_memory (&mp3, data.getData(), data.getSize(), nullptr)) { out.error = "MP3 invalide ou corrompu."; return out; }
        out.fs = (double) mp3.sampleRate;
        out.channels = (int) mp3.channels;
        const int ch = (int) mp3.channels;
        std::vector<float> tmp ((size_t) 4096 * (size_t) ch);
        for (;;)
        {
            const drmp3_uint64 got = drmp3_read_pcm_frames_f32 (&mp3, 4096, tmp.data());
            if (got == 0) break;
            for (drmp3_uint64 i = 0; i < got; ++i)
            {
                out.l.push_back (tmp[(size_t) i * (size_t) ch]);
                out.r.push_back (tmp[(size_t) i * (size_t) ch + (size_t) (ch > 1 ? 1 : 0)]);
            }
            if ((juce::int64) out.l.size() > maxFrames) { drmp3_uninit (&mp3); out.error = "Fichier trop long (25 min maximum)."; return out; }
        }
        drmp3_uninit (&mp3);
        out.ok = out.l.size() >= 4096;
        if (! out.ok) out.error = "MP3 invalide ou corrompu.";
        return out;
    }
}
