#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <memory>
#include "Look.h"
#include "MasterEngine.h"

namespace ui
{
class WaveView; class SpecView; class ReportView; class DropZone; class Player; class TransportButton;

struct Loaded
{
    bool ok = false;
    std::shared_ptr<master::Stereo> audio;
    master::Analysis an;
    std::vector<float> peaks;      // 2 valeurs par colonne : crête, RMS
    juce::String name, meta;
    juce::File file;
};

struct Mastered
{
    bool ok = false;
    std::shared_ptr<master::Stereo> audio;
    master::Report rep;
    master::Settings settings;     // réglages utilisés (sans pointeur de référence)
    std::vector<float> peaks;
};

class MainComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    static constexpr int designW = 1000, designH = 660;

    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    // Versions synchrones (outil de capture et tests) : mêmes fonctions que l'interface, sans thread.
    void loadTrackSync (const juce::File&);
    void loadReferenceSync (const juce::File&);
    void masterSync();
    void setStyleIndex (int i);
    void setEnglish (bool en);
    void setAB (bool master);
    void setPlayheadForSnapshot (double fraction);

    // Chemins asynchrones réels (threads) et hooks de test
    void openTrack (const juce::File& f) { startLoadTrack (f); }
    void runMaster() { startMaster(); }
    void exportToFile (const juce::File& f, int bits);
    bool isBusy() const { return busy; }
    bool hasMaster() const { return mastered && mastered->ok; }
    const master::Report* lastReport() const { return mastered ? &mastered->rep : nullptr; }
    juce::AudioBuffer<float> renderPlaybackForTest (double deviceFs, int blockSize, int numBlocks, bool useMaster);

private:
    friend class WaveView;
    void timerCallback() override;

    // actions
    void chooseTrack(); void chooseReference();
    void startLoadTrack (const juce::File&); void startLoadReference (const juce::File&);
    void startMaster(); void startExport();
    void applyTrack (std::shared_ptr<Loaded>); void applyReference (std::shared_ptr<Loaded>); void applyMaster (std::shared_ptr<Mastered>);
    master::Settings currentSettings() const;
    void markDirty();
    void togglePlay();
    void updateEnabled();
    void updatePlaybackGains();
    void setStatus (const juce::String& s, bool error = false);
    void relabel();
    void runJob (std::function<void()> fn);

    // état
    std::shared_ptr<Loaded> track, reference;
    std::shared_ptr<Mastered> mastered;
    bool busy = false;
    juce::String statusText; bool statusError = false;
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> cancelFlag { false };
    int styleIndex = 0;
    bool dirty = false, usedReference = false;
    juce::String exportedName;

    // interface
    Lnf lnf;
    std::unique_ptr<DropZone> trackZone, refZone;
    std::unique_ptr<WaveView> wave;
    std::unique_ptr<SpecView> spec;
    std::unique_ptr<ReportView> report;
    std::unique_ptr<TransportButton> playBtn;
    juce::TextButton abOrig, abMaster, masterBtn, exportBtn, langFr, langEn;
    juce::TextButton chips[master::kNumStyles];
    juce::ToggleButton matchToggle, refToggle;
    juce::Slider knobs[5];
    juce::ComboBox formatBox;
    std::unique_ptr<juce::FileChooser> chooser;

    // audio
    std::unique_ptr<juce::AudioDeviceManager> deviceManager;
    std::unique_ptr<juce::AudioSourcePlayer> sourcePlayer;
    std::unique_ptr<Player> player;
    bool audioReady = false;
    juce::ThreadPool pool { 1 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (MainComponent)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
}
