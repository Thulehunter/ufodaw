#include "PluginHost.h"
#include "Theme.h"

namespace
{
    juce::File appDataFolder()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("MiniDAW");
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
        setUsingNativeTitleBar (true);

        juce::AudioProcessorEditor* editor = s.instance->hasEditor() ? s.instance->createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (*s.instance);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
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
public:
    ManagerWindow (PluginHost& h)
        : juce::DocumentWindow ("Plugins", Theme::panel, juce::DocumentWindow::closeButton), host (h)
    {
        setUsingNativeTitleBar (true);
        auto* list = new juce::PluginListComponent (host.formats, host.knownPlugins,
                                                    appDataFolder().getChildFile ("scan-crashed.txt"),
                                                    host.properties.get(), true);
        list->setSize (720, 480);
        setContentOwned (list, true);
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
    options.applicationName = "MiniDAW";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = "MiniDAW";
    properties = std::make_unique<juce::PropertiesFile> (options);

    if (auto xml = juce::XmlDocument::parse (listFile()))
        knownPlugins.recreateFromXml (*xml);
    knownPlugins.addChangeListener (this);
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

std::shared_ptr<PluginSlot> PluginHost::createSlot (const juce::PluginDescription& description, Engine& engine, juce::String& error)
{
    auto instance = formats.createPluginInstance (description, engine.getSampleRate(), engine.getMaxBlockSize(), error);
    if (instance == nullptr)
        return nullptr;

    auto slot = std::make_shared<PluginSlot>();
    slot->instance = std::move (instance);
    slot->description = description;
    slot->key = PluginSlot::newKey();
    engine.preparePlugin (*slot);
    return slot;
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
    for (int i = editors.size(); --i >= 0;)
        if (editors[i]->getSlot() == slot)
            editors.remove (i);
}

void PluginHost::closeAllEditors()
{
    editors.clear();
}

void PluginHost::showManager()
{
    if (manager == nullptr)
        manager = std::make_unique<ManagerWindow> (*this);
    manager->setVisible (true);
    manager->toFront (true);
}
