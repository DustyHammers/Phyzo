// Startup log (F-65): what happens between a host loading Phyzo and its window appearing, for hosts where something
// goes wrong that cannot be seen from outside (e.g. Logic Pro, which may run plugins in its AUHostingService helper
// process). Each line goes to ~/Documents/Phyzo/Phyzo-log.txt (started over when it passes 256 KB) and to the macOS
// system log (Console.app, search "Phyzo:"), with the date and time, the host process's name and its pid. Always on,
// short lines, never from the audio thread (it sets flags that the processor's timer logs). The skin log
// (skin-log.txt, Developer > Write skin log) is separate.
#pragma once
#include <juce_core/juce_core.h>
#if !JUCE_WINDOWS
#include <unistd.h>
#endif

#if JUCE_MAC
void phyzoSystemLog(const char* line);                // MacWindow.mm (NSLog)
#endif

namespace phyzodiag {

inline juce::String arch() {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "other";
#endif
}

inline juce::File logFile() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Phyzo").getChildFile("Phyzo-log.txt");
}

inline void log(const juce::String& what) {
    static juce::CriticalSection lock;
    const juce::ScopedLock sl(lock);
    const juce::Time now = juce::Time::getCurrentTime();
    const juce::String line = now.formatted("%Y-%m-%d %H:%M:%S") + "." + juce::String(now.getMilliseconds()).paddedLeft('0', 3)
                              + " [" + juce::File::getSpecialLocation(juce::File::hostApplicationPath).getFileNameWithoutExtension()
                              + " " + juce::String(int(getpid())) + "] " + what;
#if JUCE_MAC
    phyzoSystemLog(("Phyzo: " + line).toRawUTF8());
#endif
    const juce::File f = logFile();
    if (!f.getParentDirectory().isDirectory()) f.getParentDirectory().createDirectory();
    if (f.getSize() > 256 * 1024) f.replaceWithText({});
    f.appendText(line + "\n", false, false, "\n");
}

}  // namespace phyzodiag
