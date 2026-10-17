#include <juce_gui_basics/juce_gui_basics.h>
#include "MainComponent.h"

class UFODAWApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    void initialise (const juce::String& commandLine) override
    {
        migrateOldSettings();
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);   // the warm red/orange/yellow theme
        window = std::make_unique<MainWindow> (getApplicationName());

        // "UFODAW.exe project.mdaw" (or double-clicking a project file) opens that project
        const juce::File project (commandLine.trim().unquoted().trim());
        if (project.existsAsFile())
            if (auto* main = dynamic_cast<MainComponent*> (window->getContentComponent()))
                main->openProject (project);
    }

    // The program used to be called MiniDAW: take its settings (audio device, plugin list, UI options) along once.
    static void migrateOldSettings()
    {
        const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
        const auto oldDir = appData.getChildFile ("MiniDAW"), newDir = appData.getChildFile ("UFODAW");
        if (! oldDir.isDirectory() || newDir.isDirectory())
            return;

        if (oldDir.copyDirectoryTo (newDir))
        {
            const auto oldSettings = newDir.getChildFile ("MiniDAW.settings");
            if (oldSettings.existsAsFile() && ! newDir.getChildFile ("UFODAW.settings").existsAsFile())
                oldSettings.moveFileTo (newDir.getChildFile ("UFODAW.settings"));
        }
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

START_JUCE_APPLICATION (UFODAWApplication)
