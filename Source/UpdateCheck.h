#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>
#include <thread>

// Looks on GitHub for the latest release of UFODAW and says whether it is newer than this version. Only one small request to
// api.github.com is made (nothing about you or your projects is sent), when you press the update button or, if you leave it on, once a day at start-up.
namespace UpdateCheck
{
    inline const char* latestReleaseUrl() { return "https://api.github.com/repos/Thulehunter/ufodaw/releases/latest"; }

    struct Result
    {
        bool ok = false;               // the answer was read (false: offline, or no release found)
        bool newer = false;            // the release is newer than this version
        juce::String version;          // "0.8.9"
        juce::String notes;            // what is new, as written in the release
        juce::String pageUrl;          // the release page
        juce::String downloadUrl;      // the installer (or the zip) of the release; the page when there is none
    };

    // "0.8.8.2" is newer than "0.8.8"; every part is a number, a missing part counts as 0
    inline bool isNewer (const juce::String& candidate, const juce::String& current)
    {
        auto parts = [] (const juce::String& s) { juce::Array<int> v; for (auto& p : juce::StringArray::fromTokens (s.trimCharactersAtStart ("vV"), ".", "")) v.add (p.getIntValue()); return v; };
        const auto a = parts (candidate), b = parts (current);
        for (int i = 0; i < juce::jmax (a.size(), b.size()); ++i)
        {
            const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
            if (x != y)
                return x > y;
        }
        return false;
    }

    inline Result parse (const juce::String& json, const juce::String& currentVersion)
    {
        Result r;
        const auto root = juce::JSON::parse (json);
        const auto tag = root["tag_name"].toString();
        if (tag.isEmpty())
            return r;
        r.ok = true;
        r.version = tag.trimCharactersAtStart ("vV");
        r.notes = root["body"].toString();
        r.pageUrl = root["html_url"].toString();
        r.downloadUrl = r.pageUrl;
        if (auto* assets = root["assets"].getArray())
        {
            for (auto& a : *assets)   // the installer first, else the zip
                if (a["name"].toString().endsWithIgnoreCase ("-setup.exe"))
                    r.downloadUrl = a["browser_download_url"].toString();
            if (r.downloadUrl == r.pageUrl)
                for (auto& a : *assets)
                    if (a["name"].toString().endsWithIgnoreCase (".zip") && ! a["name"].toString().containsIgnoreCase ("unsigned"))
                        r.downloadUrl = a["browser_download_url"].toString();
        }
        r.newer = isNewer (r.version, currentVersion);
        return r;
    }

    // Asks GitHub on a background thread; the callback is called on the message thread
    inline void checkAsync (juce::String currentVersion, std::function<void (Result)> done)
    {
        std::thread ([currentVersion, done = std::move (done)]
        {
            Result r;
            juce::URL url (latestReleaseUrl());
            if (auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                         .withConnectionTimeoutMs (8000)
                                                         .withExtraHeaders ("User-Agent: UFODAW\r\nAccept: application/vnd.github+json\r\n")))
                r = parse (stream->readEntireStreamAsString(), currentVersion);
            juce::MessageManager::callAsync ([done, r] { if (done) done (r); });
        }).detach();
    }
}
