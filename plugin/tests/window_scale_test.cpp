// Window size test (no ROMs). The new scale: 100 % of a file skin is 3/4 of its 1795 x 848 dp body in points.
//  1. The scale rules: Zoom menu steps and labels, 133 % = full size (4/3), clamping, the old -> new conversion.
//  2. Project state: version 4 keeps the size and its scale; a version 3 project's size is marked as old scale.
//  3. The editor: an old-scale size reopens at the same physical size on a file skin (old x 4/3) and unchanged on
//     Built-in; the corner drag (constrainer) stops at 50 % and 200 %; the menu steps size the window exactly.
//
//   window_scale_test <test skin folder>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <cstdio>
#include "PluginProcessor.h"
#include "SkinSettings.h"
#include "WindowScale.h"
#if JUCE_LINUX
#include <X11/Xlib.h>
#endif

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {
void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }
bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }

juce::MemoryBlock stateOf(juce::AudioProcessor& p) { juce::MemoryBlock b; p.getStateInformation(b); return b; }

// A project state as processor version `version` writes it (the layout in PluginProcessor.cpp), with a stand-in
// engine part (no ROMs here; the engine keeps it until it has ROMs). Version 3 has no scale-version byte.
juce::MemoryBlock projectState(int version, float scale, bool legacy) {
    juce::MemoryBlock dest;
    juce::MemoryOutputStream out(dest, false);
    out.writeInt(0x505A4850);
    out.writeInt(version);
    out.writeInt(1);
    juce::MemoryBlock engine(1000, true), packed;
    {
        juce::MemoryOutputStream raw(packed, false);
        juce::GZIPCompressorOutputStream gz(raw, 6);
        gz.write(engine.getData(), engine.getSize());
    }
    out.writeInt64(juce::int64(engine.getSize()));
    out.writeInt64(juce::int64(packed.getSize()));
    out.write(packed.getData(), packed.getSize());
    out.writeInt(0);                                                  // no knob positions
    out.writeFloat(scale);
    if (version >= 4) out.writeByte(legacy ? 1 : 0);
    out.flush();
    return dest;
}

// The scale part of what a processor saves now: the last 5 bytes (float, then the old-scale flag).
std::pair<float, bool> savedScale(juce::AudioProcessor& p) {
    const juce::MemoryBlock b = stateOf(p);
    juce::MemoryInputStream in(static_cast<const char*>(b.getData()) + b.getSize() - 5, 5, false);
    const float s = in.readFloat();
    return {s, in.readByte() != 0};
}

// Opens an editor on the current skin, returns its size, closes it.
juce::Point<int> openEditor(PhyzoProcessor& p, std::function<void(juce::AudioProcessorEditor&)> also = {}) {
    std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
    ed->addToDesktop(0);
    ed->setVisible(true);
    pump(300);
    const juce::Point<int> size(ed->getWidth(), ed->getHeight());
    if (also) also(*ed);
    p.editorBeingDeleted(ed.get());
    ed.reset();
    pump(100);
    return size;
}
}  // namespace

int main(int argc, char** argv) {
    // 1. The rules.
    std::vector<int> steps(std::begin(winscale::kSteps), std::end(winscale::kSteps));
    CHECK((steps == std::vector<int>{50, 75, 100, 125, 133, 150, 175, 200}));
    CHECK(near(winscale::stepScale(133), 4.0f / 3.0f) && near(winscale::stepScale(75), 0.75f));
    CHECK(winscale::stepLabel(133, true) == "133 % (full size)" && winscale::stepLabel(133, false) == "133 %");
    CHECK(winscale::stepLabel(100, true) == "100 %");
    CHECK(winscale::isStep(4.0f / 3.0f, 133) && !winscale::isStep(1.3f, 133) && winscale::isStep(1.0f, 100));
    CHECK(winscale::windowLength(1795, 1.0f, true) == 1346 && winscale::windowLength(848, 1.0f, true) == 636);
    CHECK(winscale::windowLength(1795, 4.0f / 3.0f, true) == 1795 && winscale::windowLength(848, 4.0f / 3.0f, true) == 848);
    CHECK(winscale::windowLength(1795, 0.1f, true) == winscale::windowLength(1795, 0.5f, true));      // clamped
    CHECK(winscale::windowLength(1795, 9.0f, true) == winscale::windowLength(1795, 2.0f, true));
    CHECK(winscale::windowLength(1000, 1.0f, false) == 1000);                                         // Built-in
    CHECK(near(winscale::fromLegacy(0.75f), 1.0f) && near(winscale::fromLegacy(1.0f), 4.0f / 3.0f));
    CHECK(near(winscale::fromLegacy(1.8f), 2.0f));                                                    // beyond 200 %: clamped
    CHECK(near(winscale::scaleForWidth(1346, 1795, true), 1346.0f / 1346.25f));
    for (int z : winscale::kSteps) {                       // every step round-trips through a window width
        const float s = winscale::scaleForWidth(winscale::windowLength(1795, winscale::stepScale(z), true), 1795, true);
        CHECK(winscale::isStep(s, z));
    }

    if (argc < 2) { std::printf("usage: window_scale_test <test skin folder>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
#if JUCE_LINUX
    XSetErrorHandler([](Display*, XErrorEvent*) { return 0; });
#endif
    const juce::File skins = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Phyzo/skins/scaletest");
    skins.deleteRecursively();
    skins.createDirectory();
    for (const char* f : {"test.rml", "test.rcss", "test.lua"})
        juce::File(argv[1]).getChildFile(f).copyFileTo(skins.getChildFile(juce::String(f).replace("test.rml", "scaletest.rml")));
    const juce::String previousSkin = SkinSettings::skin();
    const float previousScale = SkinSettings::scale();

    // 2. Project state.
    {
        std::unique_ptr<juce::AudioProcessor> a(createPluginFilter());
        auto& p = static_cast<PhyzoProcessor&>(*a);
        juce::MemoryBlock s = projectState(4, 1.25f, false);
        p.setStateInformation(s.getData(), int(s.getSize()));
        CHECK(near(p.editorScale.load(), 1.25f) && !p.editorScaleLegacy.load());
        s = projectState(3, 0.75f, false);                          // a project saved by an older version
        p.setStateInformation(s.getData(), int(s.getSize()));
        CHECK(near(p.editorScale.load(), 0.75f) && p.editorScaleLegacy.load());
        // Saved again before an editor has opened: still marked as old scale.
        const auto saved = savedScale(p);
        CHECK(near(saved.first, 0.75f) && saved.second);
        p.editorScale = 1.5f; p.editorScaleLegacy = false;
        CHECK(near(savedScale(p).first, 1.5f) && !savedScale(p).second);
    }

    // 3. The editor.
    {
        std::unique_ptr<juce::AudioProcessor> a(createPluginFilter());
        auto& p = static_cast<PhyzoProcessor&>(*a);
        p.prepareToPlay(48000, 512);
        SkinSettings::setSkin("scaletest");

        // An old project at the old 75 % reopens at 1346 x 636, now called 100 %, and is saved on the new scale.
        p.editorScale = 0.75f; p.editorScaleLegacy = true;
        juce::Point<int> size = openEditor(p);
        std::printf("window_scale_test: old-scale 75 %% on the rack-size skin reopens at %d x %d (new scale %.1f %%)\n",
                    size.x, size.y, p.editorScale.load() * 100);
        CHECK(size == juce::Point<int>(1346, 636));
        CHECK(near(p.editorScale.load(), 1.0f) && !p.editorScaleLegacy.load());
        // The old 100 % (full size) reopens at full size: 133 %.
        p.editorScale = 1.0f; p.editorScaleLegacy = true;
        size = openEditor(p);
        CHECK(size == juce::Point<int>(1795, 848) && winscale::isStep(p.editorScale.load(), 133));

        // New instance without a size of its own: the global size, which starts at the new 100 %.
        SkinSettings::setScale(1.0f);
        p.editorScale = 0.0f; p.editorScaleLegacy = false;
        CHECK(openEditor(p) == juce::Point<int>(1346, 636));

        // Drag limits: the constrainer stops the corner drag at 50 % and 200 %, aspect locked.
        p.editorScale = 1.0f;
        openEditor(p, [&](juce::AudioProcessorEditor& ed) {
            auto* c = ed.getConstrainer();
            c->setBoundsForComponent(&ed, {0, 0, 10000, 10000}, false, false, true, true);
            CHECK(ed.getWidth() == winscale::windowLength(1795, 2.0f, true) && ed.getHeight() == winscale::windowLength(848, 2.0f, true));
            CHECK(near(p.editorScale.load(), 2.0f));
            c->setBoundsForComponent(&ed, {0, 0, 10, 10}, false, false, true, true);
            CHECK(ed.getWidth() == winscale::windowLength(1795, 0.5f, true) && ed.getHeight() == winscale::windowLength(848, 0.5f, true));
            CHECK(near(p.editorScale.load(), 0.5f));
            std::printf("window_scale_test: corner drag limits %d x %d to %d x %d\n", winscale::windowLength(1795, 0.5f, true),
                        winscale::windowLength(848, 0.5f, true), winscale::windowLength(1795, 2.0f, true), winscale::windowLength(848, 2.0f, true));
        });

        // Every Zoom step sizes the window exactly.
        for (int z : winscale::kSteps) {
            p.editorScale = winscale::stepScale(z);
            size = openEditor(p);
            CHECK(size.x == winscale::windowLength(1795, winscale::stepScale(z), true));
            CHECK(winscale::isStep(p.editorScale.load(), z));
        }

        // Built-in: its 100 % is unchanged, and an old-scale size is not converted.
        SkinSettings::setSkin(SkinSettings::kBuiltIn);
        p.editorScale = 1.0f; p.editorScaleLegacy = false;
        const juce::Point<int> builtin100 = openEditor(p);
        p.editorScale = 1.2f; p.editorScaleLegacy = true;
        size = openEditor(p);
        CHECK(size.x == juce::roundToInt(builtin100.x * 1.2f) && near(p.editorScale.load(), 1.2f));
        std::printf("window_scale_test: Built-in 100 %% is %d x %d\n", builtin100.x, builtin100.y);
    }

    SkinSettings::setSkin(previousSkin);
    SkinSettings::setScale(previousScale);
    skins.deleteRecursively();
    std::printf("window_scale_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
