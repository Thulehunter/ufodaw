#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <functional>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

// What happens when a plugin brings down code that no guard covers (its own editor window, its own threads).
//  - While running: a vectored handler remembers which module the last access violation (or similar) happened in. The program is built with /EHa,
//    so JUCE's catch-all around the message loop catches it and calls the application's unhandledException(): the plugin's windows close, the
//    plugin is blocked, the user is told, and the program goes on.
//  - If the program dies anyway: a crash handler writes the faulting module to last-crash.txt. At the next start that plugin is blocked, so
//    opening the same project does not crash again, and the user hears which plugin it was.
namespace PluginCrash
{
    inline juce::File folder()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW");
        dir.createDirectory();
        return dir;
    }

    inline juce::File blockedFile()   { return folder().getChildFile ("plugin-blocked.txt"); }
    inline juce::File lastCrashFile() { return folder().getChildFile ("last-crash.txt"); }

    inline juce::String pendingNotice;   // shown once by the application after the window is up

    // The plugin whose file contains the module (or is the module) is added to the blocked list. Returns its name, or empty if it is no plugin.
    inline juce::String blockModule (const juce::String& modulePath)
    {
        if (modulePath.isEmpty())
            return {};

        auto xml = juce::XmlDocument::parse (folder().getChildFile ("plugins.xml"));
        if (xml == nullptr)
            return {};

        for (auto* e : xml->getChildIterator())
        {
            juce::PluginDescription d;
            if (! d.loadFromXml (*e) || d.fileOrIdentifier.isEmpty())
                continue;

            const auto file = d.fileOrIdentifier.replaceCharacter ('/', '\\').trimCharactersAtEnd ("\\");
            if (modulePath.replaceCharacter ('/', '\\').startsWithIgnoreCase (file))
            {
                auto blocked = juce::StringArray::fromLines (blockedFile().loadFileAsString());
                blocked.addIfNotAlreadyThere (d.createIdentifierString());
                blockedFile().replaceWithText (blocked.joinIntoString ("\n").trim());
                return d.name;
            }
        }
        return {};
    }

    inline juce::String noticeFor (const juce::String& pluginName)
    {
        return pluginName + " crashed. It is blocked from now on, so it cannot take the program down again; the rest of your project is unaffected. "
                            "Open Plugins (the scan window) to allow it again.";
    }

    // The crash file holds UTF-16 (written without allocations); this reads it.
    inline juce::String readLastCrashModule()
    {
        juce::MemoryBlock data;
        if (! lastCrashFile().loadFileAsData (data) || data.getSize() < 2)
            return {};
        const auto count = data.getSize() / sizeof (wchar_t);
        return juce::String (static_cast<const wchar_t*> (data.getData()), count);
    }

    // At start: the program died last time because of a plugin?
    inline void processLastCrash()
    {
        const auto file = lastCrashFile();
        if (! file.existsAsFile())
            return;

        const auto module = readLastCrashModule().trim();
        file.deleteFile();
        const auto name = blockModule (module);
        if (name.isNotEmpty())
            pendingNotice = noticeFor (name) + "\n\n(The program was closed by that plugin. Your work is restored from the autosave if you accept the restore prompt.)";
    }

#if JUCE_WINDOWS
    inline wchar_t crashFilePath[MAX_PATH] = {};
    inline wchar_t faultModule[MAX_PATH] = {};   // the module of the most recent access violation (any thread)
    inline std::atomic<ULONGLONG> faultTick { 0 };
    inline std::atomic<bool> faultBusy { false };

    inline void moduleOf (const void* address, wchar_t* out)
    {
        out[0] = 0;
        HMODULE module = nullptr;
        if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR) address, &module) && module != nullptr)
            GetModuleFileNameW (module, out, MAX_PATH);
    }

    inline bool isFault (DWORD code)
    {
        return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION
            || code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_IN_PAGE_ERROR;
    }

    inline LONG CALLBACK vectoredHandler (EXCEPTION_POINTERS* ep)
    {
        if (ep != nullptr && ep->ExceptionRecord != nullptr && isFault (ep->ExceptionRecord->ExceptionCode)
            && ! faultBusy.exchange (true))
        {
            moduleOf (ep->ExceptionRecord->ExceptionAddress, faultModule);
            faultTick = GetTickCount64();
            faultBusy = false;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Called when the program is going down: write who did it.
    inline void crashHandler (void* pointers)
    {
        auto* ep = static_cast<EXCEPTION_POINTERS*> (pointers);
        wchar_t module[MAX_PATH] = {};
        if (ep != nullptr && ep->ExceptionRecord != nullptr)
            moduleOf (ep->ExceptionRecord->ExceptionAddress, module);
        if (module[0] == 0 && faultTick.load() != 0 && GetTickCount64() - faultTick.load() < 3000)
            wcscpy_s (module, MAX_PATH, faultModule);

        if (module[0] != 0 && crashFilePath[0] != 0)
        {
            HANDLE h = CreateFileW (crashFilePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile (h, module, (DWORD) (wcslen (module) * sizeof (wchar_t)), &written, nullptr);   // (UTF-16, read back below)
                CloseHandle (h);
            }
        }
    }

    // The module of the last fault, if it happened a moment ago (and forgets it).
    inline juce::String takeRecentFaultModule()
    {
        juce::String result;
        if (faultTick.load() != 0 && GetTickCount64() - faultTick.load() < 3000)
            result = juce::String (faultModule);
        faultTick = 0;
        return result;
    }
#endif

    inline void install()
    {
       #if JUCE_WINDOWS
        const auto path = lastCrashFile().getFullPathName();
        wcsncpy_s (crashFilePath, MAX_PATH, path.toWideCharPointer(), _TRUNCATE);
        AddVectoredExceptionHandler (1, vectoredHandler);
        juce::SystemStats::setApplicationCrashHandler (crashHandler);
       #endif
    }

}
