#include "PluginHost.h"
#include "Theme.h"
#include "PluginScan.h"
#include "PluginProxy.h"
#include "TestPlugin.h"
#include <algorithm>

namespace
{
    juce::File appDataFolder()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW");
        dir.createDirectory();
        return dir;
    }
}

//==============================================================================
// A window around a plugin's own editor (or a generic one if it has none).
class PluginHost::EditorWindow : public juce::DocumentWindow
{
public:
    EditorWindow (PluginHost& h, PluginSlot& s)
        : juce::DocumentWindow (s.description.name, Theme::panel, juce::DocumentWindow::closeButton),
          host (h), slot (&s)
    {
        s.editorOpen = true;   // (from here on the audio thread keeps the plugin running, even with the transport stopped)
        setUsingNativeTitleBar (true);

        juce::AudioProcessorEditor* editor = s.instance->hasEditor() ? s.instance->createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (*s.instance);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);

        // Plugin editors are native child windows (often with their own GPU drawing). JUCE's default Direct2D renderer of the parent can paint
        // over them or leave the window white on some PCs, so these windows use the software (GDI) renderer.
        if (auto* peer = getPeer())
        {
            const auto engines = peer->getAvailableRenderingEngines();
            const int software = engines.indexOf ("Software Renderer");
            const int gdi = software >= 0 ? software : engines.indexOf ("GDI");
            if (gdi >= 0)
                peer->setCurrentRenderingEngine (gdi);
        }
        if (host.onEditorOpened)
            host.onEditorOpened (s);
    }

    ~EditorWindow() override
    {
        slot->editorOpen = false;
        if (host.onEditorClosed)
            host.onEditorClosed (*slot);
    }

    const PluginSlot* getSlot() const { return slot; }

    void closeButtonPressed() override
    {
        // Deleted later: we are inside this window's own callback.
        auto* h = &host;
        auto* s = slot;
        juce::MessageManager::callAsync ([h, s] { h->closeEditorsFor (s); });
    }

private:
    PluginHost& host;
    PluginSlot* slot;
};

//==============================================================================
class PluginHost::ManagerWindow : public juce::DocumentWindow
{
    // The scan list with, below it, where plugins run
    class Content : public juce::Component
    {
    public:
        Content (PluginHost& h, juce::PluginListComponent* l) : list (l)
        {
            addAndMakeVisible (*list);
            label.setText ("Run plugins in a separate process:", juce::dontSendNotification);
            addAndMakeVisible (label);
            mode.addItem ("Automatic: only plugins that gave trouble, and Waves", 1);
            mode.addItem ("Always: every plugin (safest, a little more memory)", 2);
            mode.addItem ("Never: inside UFODAW", 3);
            mode.setSelectedId ((int) h.getSandboxMode() + 1, juce::dontSendNotification);
            mode.onChange = [this, &h] { h.setSandboxMode ((SandboxMode) (mode.getSelectedId() - 1)); };
            addAndMakeVisible (mode);
            note.setText ("A plugin in a separate process cannot take UFODAW along when it crashes or hangs: its track goes silent and you can restart it. "
                          "The setting applies to plugins loaded from now on.", juce::dontSendNotification);
            note.setFont (juce::FontOptions (12.0f));
            note.setJustificationType (juce::Justification::topLeft);
            note.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (note);
            setSize (720, 540);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            auto bottom = r.removeFromBottom (74).reduced (10, 6);
            auto row = bottom.removeFromTop (26);
            label.setBounds (row.removeFromLeft (230));
            mode.setBounds (row);
            note.setBounds (bottom.withTrimmedTop (4));
            list->setBounds (r);
        }

    private:
        std::unique_ptr<juce::PluginListComponent> list;
        juce::Label label, note;
        juce::ComboBox mode;
    };

public:
    ManagerWindow (PluginHost& h)
        : juce::DocumentWindow ("Plugins", Theme::panel, juce::DocumentWindow::closeButton), host (h)
    {
        setUsingNativeTitleBar (true);
        auto* list = new juce::PluginListComponent (host.formats, host.knownPlugins,
                                                    appDataFolder().getChildFile ("scan-crashed.txt"),
                                                    host.properties.get(), true);
        setContentOwned (new Content (host, list), true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { setVisible (false); }

private:
    PluginHost& host;
};

//==============================================================================
PluginHost::PluginHost()
{
    formats.addDefaultFormats();   // VST3 (JUCE_PLUGINHOST_VST3)

    juce::PropertiesFile::Options options;
    options.applicationName = "UFODAW";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = "UFODAW";
    properties = std::make_unique<juce::PropertiesFile> (options);

    if (auto xml = juce::XmlDocument::parse (listFile()))
        knownPlugins.recreateFromXml (*xml);
    knownPlugins.addChangeListener (this);
    knownPlugins.setCustomScanner (std::make_unique<PluginScan::Scanner>());   // scanning happens in a helper process: a crashing plugin can't take the program along

    // A plugin that crashed the program while it was being loaded is not loaded again (the project then opens without it instead of crashing
    // again at once). The list is cleared when the plugin manager is opened.
    const auto marker = appDataFolder().getChildFile ("plugin-loading.txt");
    if (marker.existsAsFile())
    {
        auto blocked = blockedFile().loadFileAsString().trim();
        blocked += "\n" + marker.loadFileAsString().trim();
        blockedFile().replaceWithText (blocked.trim());
        marker.deleteFile();
    }
}

juce::File PluginHost::blockedFile() const { return appDataFolder().getChildFile ("plugin-blocked.txt"); }

bool PluginHost::isBlocked (const juce::PluginDescription& d) const
{
    return juce::StringArray::fromLines (blockedFile().loadFileAsString()).contains (d.createIdentifierString());
}

PluginHost::~PluginHost()
{
    knownPlugins.removeChangeListener (this);
    closeAllEditors();
}

juce::File PluginHost::listFile() const { return appDataFolder().getChildFile ("plugins.xml"); }

void PluginHost::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (auto xml = knownPlugins.createXml())
        xml->writeTo (listFile());
}

juce::File PluginHost::riskyFile() const { return appDataFolder().getChildFile ("plugin-risky.txt"); }

bool PluginHost::isRisky (const juce::PluginDescription& d) const
{
    return d.manufacturerName.containsIgnoreCase ("waves") || d.fileOrIdentifier.containsIgnoreCase ("waveshell")
        || juce::StringArray::fromLines (riskyFile().loadFileAsString()).contains (d.createIdentifierString());
}

void PluginHost::markRisky (const juce::PluginDescription& d)   // (kept for good: unlike the blocked list it is not cleared)
{
    auto risky = juce::StringArray::fromLines (riskyFile().loadFileAsString());
    if (! risky.contains (d.createIdentifierString()))
    {
        risky.add (d.createIdentifierString());
        riskyFile().replaceWithText (risky.joinIntoString ("\n").trim());
    }
}

PluginHost::SandboxMode PluginHost::getSandboxMode() const
{
    return (SandboxMode) juce::jlimit (0, 2, properties->getIntValue ("pluginSandbox", 0));
}

void PluginHost::setSandboxMode (SandboxMode mode)
{
    properties->setValue ("pluginSandbox", (int) mode);
    properties->saveIfNeeded();
}

bool PluginHost::wouldSandbox (const juce::PluginDescription& d) const
{
    if (! PluginProxy::isSupported())
        return false;
    const auto mode = getSandboxMode();
    if (mode == SandboxMode::never)
        return false;
    if (d.pluginFormatName == PluginSandbox::testFormatName)
        return true;
    if (d.pluginFormatName != "VST3")
        return false;
    return mode == SandboxMode::always || isRisky (d) || isBlocked (d);
}

std::shared_ptr<PluginSlot> PluginHost::createSlot (const juce::PluginDescription& description, Engine& engine, juce::String& error)
{
    bool sandbox = wouldSandbox (description);
    if (isBlocked (description))
    {
        // A plugin that crashed the program while it was loaded stays out of the program, but in a helper process it can do no harm
        if (PluginProxy::isSupported() && getSandboxMode() != SandboxMode::never && description.pluginFormatName == "VST3")
        {
            markRisky (description);
            sandbox = true;
        }
        else
        {
            error = description.name + " crashed the program the last time it was loaded, so it is skipped. Open Plugins (the scan window) to allow it again.";
            return nullptr;
        }
    }

    auto slot = std::make_shared<PluginSlot>();
    slot->description = description;
    slot->key = PluginSlot::newKey();

    if (sandbox)
    {
        bool helperFailedToStart = false;
        if (auto proxy = PluginProxy::create (description, engine.getSampleRate(), engine.getMaxBlockSize(), error, &helperFailedToStart))
        {
            slot->remote = proxy.get();
            auto* raw = slot.get();
            auto* eng = &engine;
            proxy->onFailed = [raw, eng]
            {
                if (! raw->crashed.exchange (true))   // (the audio thread may have noticed first)
                    eng->reportCrashedPlugin (*raw);
            };
            slot->instance = std::move (proxy);
            engine.preparePlugin (*slot);
            return slot;
        }
        if (! helperFailedToStart || isBlocked (description))
            return nullptr;   // the plugin itself did not load (or must not enter the program): the message says why
        error.clear();        // no helper process could be started: the plugin runs inside the program instead
    }

    if (description.pluginFormatName == PluginSandbox::testFormatName)
    {
        slot->instance = std::make_unique<TestPlugin> (description.fileOrIdentifier);
    }
    else
    {
        const auto marker = appDataFolder().getChildFile ("plugin-loading.txt");   // (still there after a crash: the next start knows who did it)
        marker.replaceWithText (description.createIdentifierString());
        slot->instance = formats.createPluginInstance (description, engine.getSampleRate(), engine.getMaxBlockSize(), error);
        marker.deleteFile();
    }
    if (slot->instance == nullptr)
        return nullptr;

    engine.preparePlugin (*slot);
    return slot;
}

bool PluginHost::restartSlot (PluginSlot& slot, juce::String& error)
{
    if (slot.remote == nullptr)
    {
        error = "This plugin runs inside UFODAW and cannot be restarted";
        return false;
    }
    closeEditorsFor (&slot);
    if (! slot.remote->restart (error))
        return false;
    slot.crashed = false;
    return true;
}

juce::var PluginHost::slotToVar (const PluginSlot& slot) const
{
    juce::MemoryBlock state;
    slot.instance->getStateInformation (state);

    auto* o = new juce::DynamicObject();
    if (auto xml = slot.description.createXml())
        o->setProperty ("description", xml->toString());
    o->setProperty ("state", state.toBase64Encoding());
    o->setProperty ("bypass", slot.bypass.load());
    o->setProperty ("key", slot.key);
    return juce::var (o);
}

std::shared_ptr<PluginSlot> PluginHost::slotFromVar (const juce::var& v, Engine& engine, juce::String& error)
{
    juce::PluginDescription description;
    auto xml = juce::XmlDocument::parse (v["description"].toString());
    if (xml == nullptr || ! description.loadFromXml (*xml))
    {
        error = "Unreadable plugin description";
        return nullptr;
    }

    auto slot = createSlot (description, engine, error);
    if (slot == nullptr)
        return nullptr;

    juce::MemoryBlock state;
    if (state.fromBase64Encoding (v["state"].toString()) && state.getSize() > 0)
        slot->instance->setStateInformation (state.getData(), (int) state.getSize());
    slot->bypass = (bool) v.getProperty ("bypass", false);
    if (v.hasProperty ("key"))   // keep the saved key so automation lanes find their plugin again
    {
        slot->key = (int) v.getProperty ("key", slot->key);
        PluginSlot::reserveKey (slot->key);
    }
    return slot;
}

void PluginHost::showEditor (PluginSlot& slot)
{
    if (slot.remote != nullptr)   // the window opens in the plugin's helper process
    {
        if (slot.remote->hasFailed())   // stopped: offer to start it again
        {
            auto* host = this;
            auto* s = &slot;
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle ("Plugin stopped")
                                              .withMessage (slot.description.name + " has stopped. Restart it with the settings it had?")
                                              .withButton ("Restart plugin").withButton ("Cancel"),
                                          [host, s] (int result)
                                          {
                                              juce::String error;
                                              if (result == 1)
                                              {
                                                  if (host->restartSlot (*s, error))
                                                      host->showEditor (*s);
                                                  else
                                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "The plugin could not be restarted", error);
                                              }
                                          });
            return;
        }
        if (slot.remote->isEditorOpen())
        {
            slot.remote->showEditor (slot.description.name);   // (brings it to the front)
            return;
        }
        slot.editorOpen = true;
        remoteEditors.push_back (&slot);
        auto* s = &slot;
        slot.remote->onEditorClosed = [this, s]
        {
            s->editorOpen = false;
            remoteEditors.erase (std::remove (remoteEditors.begin(), remoteEditors.end(), s), remoteEditors.end());
            if (onEditorClosed)
                onEditorClosed (*s);
        };
        slot.remote->showEditor (slot.description.name);
        if (onEditorOpened)
            onEditorOpened (slot);
        return;
    }

    for (auto* w : editors)
        if (w->getSlot() == &slot)
        {
            w->toFront (true);
            return;
        }
    auto* window = editors.add (new EditorWindow (*this, slot));
    window->toFront (true);   // make sure it isn't hidden behind the main window
}

void PluginHost::closeEditorsFor (const PluginSlot* slot)
{
    if (auto it = std::find (remoteEditors.begin(), remoteEditors.end(), slot); it != remoteEditors.end())
    {
        auto* s = *it;
        remoteEditors.erase (it);
        s->remote->closeEditor();
        s->editorOpen = false;
        if (onEditorClosed)
            onEditorClosed (*s);
    }
    for (int i = editors.size(); --i >= 0;)
        if (editors[i]->getSlot() == slot)
            editors.remove (i);
}

void PluginHost::closeAllEditors()
{
    for (auto* s : std::vector<PluginSlot*> (remoteEditors))
        closeEditorsFor (s);
    editors.clear();
}

void PluginHost::showManager()
{
    blockedFile().deleteFile();   // plugins that crashed the program are allowed again (the user is looking at the plugins)
    if (manager == nullptr)
        manager = std::make_unique<ManagerWindow> (*this);
    manager->setVisible (true);
    manager->toFront (true);
}
