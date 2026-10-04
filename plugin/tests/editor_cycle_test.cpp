// Editor open/close test (F-27, no ROMs): the plugin's editor with the text-only test skin (OpenGL) is opened, shown
// until it has drawn, hidden (it must draw nothing then), and closed, 50 times. Reports process memory after the first
// and the last cycle (on macOS the physical footprint, which includes the GPU allocations made for the process) and
// CPU time while the editor is open but hidden; fails if memory grows or a hidden editor draws.
//
//   editor_cycle_test <test skin folder>
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "PluginProcessor.h"
#include "RmlSkinComponent.h"
#include "SkinSettings.h"
#if JUCE_MAC
#include <mach/mach.h>
#endif
#include <sys/resource.h>
#if JUCE_LINUX
#include <X11/Xlib.h>
#endif

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace {
double memoryMB() {
#if JUCE_MAC
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    return double(info.phys_footprint) / 1048576.0;
#else
    long pages = 0, rss = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) { if (std::fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; std::fclose(f); }
    return double(rss) * double(sysconf(_SC_PAGESIZE)) / 1048576.0;
#endif
}
double cpuSeconds() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}
void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }
RmlSkinComponent* findSkin(juce::Component* c) {
    if (auto* r = dynamic_cast<RmlSkinComponent*>(c)) return r;
    for (auto* child : c->getChildren()) if (auto* r = findSkin(child)) return r;
    return nullptr;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: editor_cycle_test <test skin folder>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
#if JUCE_LINUX
    // A display without a window manager (CI, Xvfb) has no WM_STATE atom; JUCE's queries for it must not end the run.
    XSetErrorHandler([](Display*, XErrorEvent*) { return 0; });
#endif
    // The test skin under ~/Documents/Phyzo/skins/, chosen for this run (the previous choice is put back at the end).
    const juce::File skins = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Phyzo/skins/cycletest");
    skins.deleteRecursively();
    skins.createDirectory();
    for (const char* f : {"test.rml", "test.rcss", "test.lua"})
        juce::File(argv[1]).getChildFile(f).copyFileTo(skins.getChildFile(juce::String(f).replace("test.rml", "cycletest.rml")));
    const juce::String previousSkin = SkinSettings::skin();
    const bool builtin = argc > 2 && juce::String(argv[2]) == "builtin";     // for comparison: no OpenGL, no RmlUi
    SkinSettings::setSkin(builtin ? SkinSettings::kBuiltIn : juce::String("cycletest"));

    std::unique_ptr<juce::AudioProcessor> proc(createPluginFilter());
    proc->prepareToPlay(48000, 512);
    int failures = 0;
    double memCold = 0, memFirst = 0, memLast = 0, hiddenCpu = 0, shownCpu = 0;
    uint64_t hiddenFrames = 0;
    bool glFrames = false;
    const int cycles = getenv("CYCLES") ? std::max(2, atoi(getenv("CYCLES"))) : 50;
    for (int cycle = 1; cycle <= cycles; ++cycle) {
        std::unique_ptr<juce::AudioProcessorEditor> ed(proc->createEditorIfNeeded());
        ed->setOpaque(true);
        ed->addToDesktop(0);
        ed->setTopLeftPosition(40, 40);
        ed->setVisible(true);
        pump(400);
        RmlSkinComponent* skin = findSkin(ed.get());
        const uint64_t shownFrames = skin ? skin->framesDrawn() : 0;
        glFrames |= shownFrames > 0;
        if (cycle == 1 || cycle == cycles) {                 // CPU while shown (the test skin draws at its onframe rate)
            const double c0 = cpuSeconds(); pump(1000); shownCpu = cpuSeconds() - c0;
        }
        ed->setVisible(false);                           // hidden: nothing may be drawn
        pump(100);
        const uint64_t f0 = skin ? skin->framesDrawn() : 0;
        const double c0 = cpuSeconds();
        pump(cycle == 1 || cycle == cycles ? 1000 : 200);
        if (cycle == 1 || cycle == cycles) hiddenCpu = cpuSeconds() - c0;
        hiddenFrames += (skin ? skin->framesDrawn() : 0) - f0;
        proc->editorBeingDeleted(ed.get());
        ed.reset();
        pump(150);
        if (cycle == 1) memCold = memoryMB();
        if (cycle == std::min(5, cycles - 1)) memFirst = memoryMB();   // after the driver's one-time warm-up
        if (cycle == cycles) memLast = memoryMB();
    }
    proc.reset();
    SkinSettings::setSkin(previousSkin);
    skins.deleteRecursively();

    std::printf("editor_cycle_test: %d open/close cycles (%s): memory after the 1st %.1f MB, after the 5th %.1f MB, after the last %.1f MB"
                " (%+.1f MB since the 5th)\n", cycles, builtin ? "Built-in skin" : "test skin, OpenGL", memCold, memFirst, memLast, memLast - memFirst);
    std::printf("editor_cycle_test: CPU per second, editor shown (test skin, onframe) %.1f %%, hidden %.1f %%; frames drawn while hidden: %llu\n",
                shownCpu * 100, hiddenCpu * 100, (unsigned long long)hiddenFrames);
    if (!glFrames && !builtin) { std::printf("editor_cycle_test: no OpenGL frames in this environment (no GPU/display): memory check only\n"); }
    if (hiddenFrames != 0) { std::printf("FAIL: the hidden editor drew frames\n"); ++failures; }
    // Memory must not grow once the GL driver has warmed up (compiled shaders, pools: done in the first cycles).
    if (memLast - memFirst > 8.0) { std::printf("FAIL: memory grew by more than 8 MB after the 5th cycle\n"); ++failures; }
    std::printf("editor_cycle_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
