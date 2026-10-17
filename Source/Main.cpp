#include <juce_gui_basics/juce_gui_basics.h>
#include "MainComponent.h"

class MiniDAWApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    void initialise (const juce::String& commandLine) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);   // the warm red/orange/yellow theme
        window = std::make_unique<MainWindow> (getApplicationName());

        // "MiniDAW.exe project.mdaw" (or double-clicking a project file) opens that project
        const juce::File project (commandLine.trim().unquoted().trim());
        if (project.existsAsFile())
            if (auto* main = dynamic_cast<MainComponent*> (window->getContentComponent()))
                main->openProject (project);
    }

    void shutdown() override
    {
        window = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

private:
    WarmLookAndFeel lookAndFeel;

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, Theme::background, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (MiniDAWApplication)
