#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginCrash.h"   // (brings in windows.h without the min/max macros)
#if JUCE_WINDOWS
 #include <bcrypt.h>
 #pragma comment (lib, "bcrypt.lib")
#endif

// Installs an update from inside the program: downloads the installer of the release (behind a progress window with a cancel button), checks it
// against the SHA-256 that GitHub lists for that file, starts it silently and closes the program so the installer can replace the files. The
// installer starts UFODAW again when it is done. Anything unexpected (another address, no checksum, a different checksum) stops before anything runs.
namespace UpdateInstall
{
    inline bool isTrustedUrl (const juce::String& url)
    {
        return url.startsWith ("https://github.com/Thulehunter/ufodaw/releases/download/") && url.endsWithIgnoreCase ("-setup.exe");
    }

    // SHA-256 of a file as lower-case hex (Windows' own crypto API); empty when it could not be read.
    inline juce::String sha256Of (const juce::File& file)
    {
        juce::String result;
       #if JUCE_WINDOWS
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        juce::FileInputStream in (file);
        if (in.failedToOpen() || BCryptOpenAlgorithmProvider (&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
            return {};
        if (BCryptCreateHash (alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
        {
            std::vector<char> buffer (64 * 1024);
            bool ok = true;
            while (! in.isExhausted())
            {
                const auto n = in.read (buffer.data(), (int) buffer.size());
                if (n <= 0)
                    break;
                ok = ok && BCryptHashData (hash, (PUCHAR) buffer.data(), (ULONG) n, 0) == 0;
            }
            unsigned char digest[32] = {};
            if (ok && BCryptFinishHash (hash, digest, sizeof (digest), 0) == 0)
                result = juce::String::toHexString (digest, (int) sizeof (digest), 0).toLowerCase();
            BCryptDestroyHash (hash);
        }
        BCryptCloseAlgorithmProvider (alg, 0);
       #endif
        return result;
    }

    class Job : public juce::ThreadWithProgressWindow
    {
    public:
        Job (juce::String urlToGet, juce::String expectedSha, juce::String versionName, juce::Component* centreAround)
            : juce::ThreadWithProgressWindow ("Downloading UFODAW " + versionName, true, true, 5000, "Cancel", centreAround),
              url (std::move (urlToGet)), sha (std::move (expectedSha)), version (std::move (versionName))
        {
            target = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("UFODAW-" + version + "-setup.exe");
        }

        void run() override
        {
            setStatusMessage ("Downloading the installer...");
            std::unique_ptr<juce::InputStream> in (juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                                                      .withConnectionTimeoutMs (15000)
                                                                                      .withNumRedirectsToFollow (5)
                                                                                      .withExtraHeaders ("User-Agent: UFODAW\r\n")));
            if (in == nullptr)
            {
                error = "No answer from GitHub. Are you online?";
                return;
            }

            const auto total = in->getTotalLength();
            target.deleteFile();
            juce::FileOutputStream out (target);
            if (out.failedToOpen())
            {
                error = "The installer could not be saved in the temporary folder.";
                return;
            }

            char buffer[64 * 1024];
            juce::int64 got = 0;
            while (! threadShouldExit())
            {
                const auto n = in->read (buffer, (int) sizeof (buffer));
                if (n < 0) { error = "The download was interrupted."; return; }
                if (n == 0)
                    break;
                out.write (buffer, (size_t) n);
                got += n;
                if (total > 0)
                    setProgress ((double) got / (double) total);
            }
            out.flush();
            if (threadShouldExit())
                return;
            if (total > 0 && got != total)
            {
                error = "The download was incomplete.";
                return;
            }

            setStatusMessage ("Checking the download...");
            if (sha256Of (target) != sha)
                error = "The downloaded file does not match the checksum GitHub lists for it, so it is not installed.";
        }

        void threadComplete (bool userPressedCancel) override
        {
            if (userPressedCancel)
            {
                target.deleteFile();
            }
            else if (error.isNotEmpty())
            {
                target.deleteFile();
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "The update was not installed",
                                                        error + "\n\nYou can download it yourself from\nhttps://github.com/Thulehunter/ufodaw/releases");
            }
            else
            {
                // (silent: no wizard; the installer closes the running program, replaces the files and starts UFODAW again)
                const auto started = juce::Process::openDocument (target.getFullPathName(), "/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS");
                if (started)
                    juce::JUCEApplication::getInstance()->systemRequestedQuit();
                else
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "The update was not installed",
                                                            "The installer could not be started. It is saved as\n" + target.getFullPathName());
            }
            delete this;
        }

    private:
        juce::String url, sha, version, error;
        juce::File target;
    };

    // Starts the download (the job deletes itself when it is done).
    inline void start (const juce::String& url, const juce::String& sha, const juce::String& version, juce::Component* parent)
    {
        (new Job (url, sha.toLowerCase(), version, parent))->launchThread();
    }
}
