#include <juce_gui_basics/juce_gui_basics.h>
#include "ProjectTabs.h"
#include <iostream>
#include "PluginScan.h"
#include "PluginCrash.h"
#include "CrashReport.h"
#include "Scripting.h"
#include "ProjectTemplates.h"
#include "StarterPack.h"
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
        PluginCrash::processLastCrash();   // the plugin that closed the program last time is blocked now
        PluginCrash::install();
        registerProjectFileType();
        Scripting::ensureExamples();         // the example scripts (Lua)
        StarterPack::ensureInstalled();      // the sounds, drum kits and project templates that are made by the program (a fraction of a second, once)
        ProjectTemplates::ensureWritten();
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);   // the warm red/orange/yellow theme
        window = std::make_unique<MainWindow> (getApplicationName());

        // "UFODAW.exe --selftest result.txt a.mdaw b.mdaw": opens each project without dialogs, renders a few seconds and writes one line per
        // project to result.txt (and exit code 1 if any failed). The automatic tests use it to see that old and new project files keep opening.
        if (commandLine.trim().startsWith ("--selftest"))
        {
            auto args = juce::StringArray::fromTokens (commandLine.trim(), true);
            args.removeRange (0, 1);
            juce::MessageManager::callAsync ([this, args] { runSelfTest (args); });
            return;
        }

        // "UFODAW.exe --render project.mdaw out.wav": renders the whole project to an audio file (wav, flac, aiff, mp3 or ogg) without a dialog and ends
        if (commandLine.trim().startsWith ("--render"))
        {
            auto args = juce::StringArray::fromTokens (commandLine.trim(), true);
            args.removeRange (0, 1);
            juce::MessageManager::callAsync ([this, args]
            {
                auto* tabs = dynamic_cast<ProjectTabs*> (window->getContentComponent());
                bool ok = false;
                if (tabs != nullptr && tabs->getActive() != nullptr && args.size() >= 2 && juce::File (args[0].unquoted()).existsAsFile())
                {
                    MainComponent::selfTesting = true;   // (no dialogs)
                    tabs->getActive()->openFileHere (juce::File (args[0].unquoted()));
                    ok = tabs->getActive()->renderForCommandLine (juce::File (args[1].unquoted()));
                }
                std::cout << (ok ? "rendered " : "FAILED ") << (args.size() >= 2 ? args[1] : juce::String()) << std::endl;
                setApplicationReturnValue (ok ? 0 : 1);
                quit();
            });
            return;
        }

        // "UFODAW.exe --run-script script.lua [project.mdaw] [name=value ...]": runs a Lua script without dialogs (on the project, if one is given), prints
        // what it printed and the error if there is one, and ends with exit code 0 (ok) or 1 (the script failed)
        if (commandLine.trim().startsWith ("--run-script"))
        {
            auto args = juce::StringArray::fromTokens (commandLine.trim(), true);
            args.removeRange (0, 1);
            juce::MessageManager::callAsync ([this, args]
            {
                auto* tabs = dynamic_cast<ProjectTabs*> (window->getContentComponent());
                bool ok = false;
                if (tabs != nullptr && tabs->getActive() != nullptr && args.size() >= 1 && juce::File (args[0].unquoted()).existsAsFile())
                {
                    MainComponent::selfTesting = true;   // (no dialogs)
                    std::map<juce::String, juce::String> params;
                    for (int i = 1; i < args.size(); ++i)
                    {
                        const auto a = args[i].unquoted();
                        if (a.contains ("=") && ! juce::File (a).existsAsFile())
                            params[a.upToFirstOccurrenceOf ("=", false, false)] = a.fromFirstOccurrenceOf ("=", false, false);
                        else if (juce::File (a).existsAsFile())
                            tabs->getActive()->openFileHere (juce::File (a));
                    }
                    const juce::File script (args[0].unquoted());
                    const auto source = script.loadFileAsString();
                    for (auto& p : Scripting::readParams (source))   // (a parameter that is not given takes its default, as the dialog would)
                        if (params.find (p.key) == params.end())
                            params[p.key] = p.defaultValue;
                    const double maxSeconds = params.count ("max_seconds") ? params["max_seconds"].getDoubleValue() : 20.0;
                    const auto result = Scripting::run (*tabs->getActive(), source, script.getFileName(), params, maxSeconds);
                    if (result.output.isNotEmpty())
                        std::cout << result.output << std::endl;
                    if (! result.ok)
                        std::cout << "ERROR " << result.error << std::endl;
                    ok = result.ok;
                }
                else
                    std::cout << "ERROR script not found" << std::endl;
                setApplicationReturnValue (ok ? 0 : 1);
                quit();
            });
            return;
        }

        // "UFODAW.exe project.mdaw" (or double-clicking a project file) opens that project in a tab
        const juce::File project (commandLine.trim().unquoted().trim());
        if (auto* tabs = dynamic_cast<ProjectTabs*> (window->getContentComponent()))
            tabs->start (project);

        if (PluginCrash::crashedLastTime)
        {
            const auto module = PluginCrash::pendingModule == "unknown" ? juce::String() : PluginCrash::pendingModule;
            const auto plugin = PluginCrash::pendingPlugin;
            const auto notice = PluginCrash::pendingNotice;
            juce::MessageManager::callAsync ([module, plugin, notice]
            {
                CrashReport::show (plugin.isNotEmpty() ? "A plugin crashed" : "UFODAW closed unexpectedly", notice,
                                   plugin.isNotEmpty() ? "Crash in plugin " + plugin : juce::String ("Crash on start-up after an unexpected close"),
                                   CrashReport::build ("(Please add what you were doing when it happened.)", module, plugin));
            });
        }
        PluginCrash::crashedLastTime = false;
        PluginCrash::pendingNotice.clear();
    }

    void runSelfTest (juce::StringArray args)
    {
        const juce::File resultFile (args[0].unquoted());
        args.remove (0);
        juce::String report;
        bool all = true;
        auto* tabs = dynamic_cast<ProjectTabs*> (window->getContentComponent());
        MainComponent::selfTesting = true;
        for (auto& a : args)
        {
            const juce::File project (a.unquoted());
            bool ok = false;
            juce::String line;
            if (tabs == nullptr || tabs->getActive() == nullptr)
                line = "no window";
            else if (! project.existsAsFile())
                line = "file not found";
            else
            {
                MainComponent::selfTestProblems.clear();
                tabs->getActive()->openFileHere (project);
                line = tabs->getActive()->selfTestReport (ok, project.getFileName().startsWith ("warn-"));
            }
            all = all && ok;
            report << (ok ? "PASS " : "FAIL ") << project.getFileName() << ": " << line << "\n";
        }
        resultFile.replaceWithText (report + (all ? "ALL PASSED\n" : "FAILED\n"));
        setApplicationReturnValue (all ? 0 : 1);
        quit();
    }

    // With /EHa the access violations in a plugin's window or callbacks end up here instead of ending the program: close the plugin windows,
    // block the plugin that did it and carry on.
    void unhandledException (const std::exception*, const juce::String&, int) override
    {
        static juce::int64 lastShown = 0;
        juce::String name;
       #if JUCE_WINDOWS
        name = PluginCrash::blockModule (PluginCrash::takeRecentFaultModule());
       #endif
        juce::MessageManager::callAsync ([this]
        {
            if (auto* tabs = dynamic_cast<ProjectTabs*> (window != nullptr ? window->getContentComponent() : nullptr))
                if (auto* tab = tabs->getActive())
                    tab->closePluginEditors();
        });

        const auto now = juce::Time::currentTimeMillis();
        if (now - lastShown < 5000)
            return;
        lastShown = now;
        const auto message = name.isNotEmpty() ? PluginCrash::noticeFor (name)
                                               : juce::String ("Something went wrong inside the program or a plugin and was caught. "
                                                               "It is safest to save your work now.");
        const bool plugin = name.isNotEmpty();
        juce::MessageManager::callAsync ([message, plugin]
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle (plugin ? "A plugin crashed" : "Something went wrong").withMessage (message).withButton ("OK"), nullptr);
        });
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
