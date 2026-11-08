#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"

namespace
{
// Contenu à taille de conception fixe (1000x660), mis à l'échelle pour remplir la fenêtre.
class ScaledRoot : public juce::Component
{
public:
    ScaledRoot() { addAndMakeVisible (main); main.setBounds (0, 0, ui::MainComponent::designW, ui::MainComponent::designH); setSize (ui::MainComponent::designW, ui::MainComponent::designH); }
    void resized() override
    {
        const float s = std::min (getWidth() / (float) ui::MainComponent::designW, getHeight() / (float) ui::MainComponent::designH);
        main.setTransform (juce::AffineTransform::scale (s));
    }
    ui::MainComponent main;
};
}

class MasterApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "RicchBeatz Master"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override { window = std::make_unique<Window> (getApplicationName()); }
    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    class Window : public juce::DocumentWindow
    {
    public:
        explicit Window (const juce::String& name)
            : DocumentWindow (name, ui::bg0, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new ScaledRoot(), true);
            setResizable (true, true);
            const double ratio = (double) ui::MainComponent::designW / ui::MainComponent::designH;
            auto* c = new juce::ComponentBoundsConstrainer();
            c->setFixedAspectRatio (ratio);
            c->setMinimumSize (750, 495);
            c->setMaximumSize (2000, (int) (2000 / ratio));
            setConstrainer (c);
            centreWithSize (ui::MainComponent::designW, ui::MainComponent::designH + getTitleBarHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION (MasterApp)
