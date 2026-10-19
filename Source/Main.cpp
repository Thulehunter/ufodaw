#include <juce_gui_basics/juce_gui_basics.h>
#include "ProjectTabs.h"
#include "PluginScan.h"
#if JUCE_WINDOWS
 #include <shlobj.h>
#endif

class UFODAWApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    void initialise (const juce::String& commandLine) override
    {
        // Started by the plugin scan as its helper process: no window, it only answers the scan (and ends with the program that started it)
        scanWorker = std::make_unique<PluginScan::Worker>();
        if (scanWorker->initialiseFromCommandLine (commandLine, PluginScan::commandLineId))
            return;
        scanWorker.reset();

        migrateOldSettings();
        registerProjectFileType();
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);   // the warm red/orange/yellow theme
        window = std::make_unique<MainWindow> (getApplicationName());

        // "UFODAW.exe project.mdaw" (or double-clicking a project file) opens that project in a tab
        const juce::File project (commandLine.trim().unquoted().trim());
        if (auto* tabs = dynamic_cast<ProjectTabs*> (window->getContentComponent()))
            tabs->start (project);
    }

    // Windows: .mdaw files get the UFODAW icon and open in UFODAW when double-clicked (for the current user; the installer does the same).
    static void registerProjectFileType()
    {
#if JUCE_WINDOWS
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
        const juce::String command = "\"" + exe + "\" \"%1\"", icon = "\"" + exe + "\",0";
        const juce::String base = "HKEY_CURRENT_USER\\Software\\Classes\\";
        if (juce::WindowsRegistry::getValue (base + "UFODAW.Project\\shell\\open\\command\\") == command
            && juce::WindowsRegistry::getValue (base + "UFODAW.Project\\DefaultIcon\\") == icon
            && juce::WindowsRegistry::getValue (base + ".mdaw\\") == "UFODAW.Project")
            return;

        juce::WindowsRegistry::setValue (base + ".mdaw\\", "UFODAW.Project");
        juce::WindowsRegistry::setValue (base + "UFODAW.Project\\", "UFODAW project");
        juce::WindowsRegistry::setValue (base + "UFODAW.Project\\DefaultIcon\\", icon);
        juce::WindowsRegistry::setValue (base + "UFODAW.Project\\shell\\open\\command\\", command);
        SHChangeNotify (SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);   // (the Explorer shows the new icons)
#endif
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
        scanWorker = nullptr;
        window = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

private:
    WarmLookAndFeel lookAndFeel;
    std::unique_ptr<PluginScan::Worker> scanWorker;

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, Theme::background, 0)   // no buttons of its own: the tab strip is the title bar
        {
            setUsingNativeTitleBar (false);
            setTitleBarHeight (0);
            auto* tabs = new ProjectTabs();
            setContentOwned (tabs, true);
            tabs->onMinimise = [this] { setMinimised (true); };
            tabs->onToggleMaximise = [this] { toggleMaximised(); };
            tabs->isMaximised = [this] { return isMaximised(); };
            tabs->onClose = [this] { closeButtonPressed(); };
            tabs->onDragStart = [this] (const juce::MouseEvent& e) { if (! isMaximised()) dragger.startDraggingComponent (this, e); };
            tabs->onDragMove = [this] (const juce::MouseEvent& e) { if (! isMaximised()) dragger.dragComponent (this, e, nullptr); };
            setResizeLimits (900, 560, 10000, 10000);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

        // When no component has the keyboard focus (right after the start, or after a dialog or a popup closed) the keys come to the window itself:
        // they go to the open project, so the shortcuts (Ctrl+K, Space, R...) work without clicking in the project first.
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (auto* tab = activeTab())
                if (tab->keyPressed (k))
                    return true;
            return DocumentWindow::keyPressed (k);
        }

        void activeWindowStatusChanged() override
        {
            DocumentWindow::activeWindowStatusChanged();
            if (isActiveWindow() && juce::Component::getCurrentlyFocusedComponent() == nullptr)
                if (auto* tab = activeTab())
                    tab->grabKeyboardFocus();
        }

    private:
        juce::ComponentDragger dragger;
        juce::Rectangle<int> restoreBounds;

        // Maximise = the work area of the screen the window is on (so the taskbar stays visible)
        juce::Rectangle<int> workArea() const
        {
            if (auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getBounds()))
                return d->userArea;
            return getBounds();
        }

        bool isMaximised() const { return getBounds() == workArea(); }

        void toggleMaximised()
        {
            if (isMaximised())
            {
                setBounds (restoreBounds.isEmpty() ? juce::Rectangle<int> (0, 0, 1400, 780).withCentre (workArea().getCentre()) : restoreBounds);
                return;
            }
            restoreBounds = getBounds();
            setBounds (workArea());
        }

        MainComponent* activeTab() const
        {
            if (auto* tabs = dynamic_cast<ProjectTabs*> (getContentComponent()))
                return tabs->getActive();
            return nullptr;
        }
    };

    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (UFODAWApplication)
