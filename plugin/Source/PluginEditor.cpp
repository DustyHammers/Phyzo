#include "PluginEditor.h"
#include "RmlSkinComponent.h"
#include "SkinSettings.h"
#include "skin_discovery.h"
#include "skin_view.h"

namespace {
constexpr int Meter_kParts = PhyzoProcessor::Meter::kParts;
// Built-in skin colours (approved mockup).
const juce::Colour kPurple(0xff593159), kHeader(0xff46264a), kRed(0xffff3020), kGhost(0xff1d0706), kWindow(0xff120509);

juce::Font uiFont(float size, bool bold = false) { return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain)); }
juce::String rateText(double hz) {
    const auto rounded = juce::roundToInt(hz);
    return juce::String(hz / 1000.0, rounded % 1000 != 0 ? 1 : 0) + " kHz";
}

// One seven-segment digit from the OS's segment byte: bits 0-6 = g (middle), c, b, a (top), f, e, d (bottom).
void drawDigit(juce::Graphics& g, juce::Rectangle<float> r, uint8_t bits) {
    const float t = 6, x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
    struct Seg { int bit; float x1, y1, x2, y2; };
    const Seg segs[] = {{3, x + t, y, x + w - t, y},                     // a
                        {2, x + w, y + t, x + w, y + h / 2 - t},         // b
                        {1, x + w, y + h / 2 + t, x + w, y + h - t},     // c
                        {6, x + t, y + h, x + w - t, y + h},             // d
                        {5, x, y + h / 2 + t, x, y + h - t},             // e
                        {4, x, y + t, x, y + h / 2 - t},                 // f
                        {0, x + t, y + h / 2, x + w - t, y + h / 2}};    // g
    for (const Seg& s : segs) {
        juce::Path p;
        p.startNewSubPath(s.x1, s.y1);
        p.lineTo(s.x2, s.y2);
        g.setColour((bits >> s.bit) & 1 ? kRed : kGhost);
        g.strokePath(p, juce::PathStrokeType(6, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

juce::String home(const juce::File& f) {
    const juce::String h = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName();
    const juce::String p = f.getFullPathName();
    return (p.startsWith(h) ? "~" + p.substring(h.length()) : p) + "/";
}
}  // namespace

// ================================================================== Built-in skin

void BuiltinView::PanelButton::paint(juce::Graphics& g) {
    const bool on = isEnabled();
    auto r = getLocalBounds().toFloat().reduced(1);
    if (down) { g.setColour(kRed.withAlpha(0.25f)); g.fillRoundedRectangle(r, 6); }
    g.setColour(kRed.withAlpha(on ? 1.0f : 0.45f));
    g.drawRoundedRectangle(r, 6, 1);
    g.setFont(uiFont(16, true));
    g.drawText(text, getLocalBounds(), juce::Justification::centred);
}

void BuiltinView::PanelButton::mouseDown(const juce::MouseEvent& e) {
    if (!isEnabled() || e.mods.isPopupMenu()) return;
    down = true; repaint();
    proc.engine().pressButton(id, true);
}

void BuiltinView::PanelButton::mouseUp(const juce::MouseEvent&) {
    if (!down) return;
    down = false; repaint();
    proc.engine().pressButton(id, false);
}

BuiltinView::BuiltinView(PhyzoProcessor& p)
    : proc(p),
      minus(p, 0, juce::String::fromUTF8("\xe2\x97\x80  \xe2\x88\x92")),    // "◀  −"
      plus(p, 1, juce::String::fromUTF8("+  \xe2\x96\xb6")) {               // "+  ▶"
    addAndMakeVisible(minus);
    addAndMakeVisible(plus);
    debugToggle.setToggleState(proc.showDebug, juce::dontSendNotification);
    for (auto id : {juce::ToggleButton::textColourId, juce::ToggleButton::tickColourId, juce::ToggleButton::tickDisabledColourId})
        debugToggle.setColour(id, kRed);
    debugToggle.onClick = [this] { proc.showDebug = debugToggle.getToggleState(); repaint(); };
    addAndMakeVisible(debugToggle);
    setSize(kWidth, kHeight);
    timerCallback();
    startTimerHz(15);
}

BuiltinView::~BuiltinView() { stopTimer(); }

void BuiltinView::resized() {
    minus.setBounds(40, 60, 84, 40);
    plus.setBounds(kWidth - 40 - 84, 60, 84, 40);
    debugToggle.setBounds(12, 217, 90, 22);
}

namespace { constexpr int kCorner = 24; }
static bool inCorner(const juce::Component& c, const juce::MouseEvent& e) {
    return e.x >= c.getWidth() - kCorner && e.y >= c.getHeight() - kCorner;
}

void BuiltinView::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(e); return; }
    if (onCornerResize && inCorner(*this, e)) { cornerDrag_ = true; cornerStart_ = e.getScreenPosition(); onCornerResize(0, {}); }
}
void BuiltinView::mouseDrag(const juce::MouseEvent& e) {
    if (cornerDrag_ && onCornerResize) onCornerResize(1, e.getScreenPosition() - cornerStart_);
}
void BuiltinView::mouseUp(const juce::MouseEvent& e) {
    if (cornerDrag_) { cornerDrag_ = false; if (onCornerResize) onCornerResize(2, e.getScreenPosition() - cornerStart_); }
}
void BuiltinView::mouseMove(const juce::MouseEvent& e) {
    setMouseCursor(inCorner(*this, e) ? juce::MouseCursor::BottomRightCornerResizeCursor : juce::MouseCursor::NormalCursor);
}

void BuiltinView::setNotice(const juce::String& text) {
    notice_ = text;
    noticeUntil_ = juce::Time::getMillisecondCounter() + 15000;
    repaint();
}

// Repaints only when something shown changed: display and LEDs (engine UI counter), status, notice, ROM scan, and
// the Debug meters (once a second, as they are measured).
void BuiltinView::timerCallback() {
    const Engine::State st = proc.engine().state();
    const bool running = st == Engine::State::Running;
    minus.setEnabled(running);
    plus.setEnabled(running);
    if (notice_.isNotEmpty() && juce::Time::getMillisecondCounter() > noticeUntil_) notice_.clear();
    const std::size_t sig = std::hash<std::string>()(proc.engine().message()) ^
                            (std::hash<std::string>()(notice_.toStdString()) << 1) ^
                            (std::size_t(proc.engine().uiSerial()) << 3) ^ (std::size_t(st) << 40) ^
                            (std::size_t(proc.romScan().complete()) << 44) ^ (std::size_t(proc.showDebug) << 45) ^
                            (proc.showDebug ? std::size_t(proc.meter.head.load()) << 48 : 0) ^ std::size_t(proc.hostRate() * 10);
    if (sig != lastSignature_) { lastSignature_ = sig; repaint(); }
}

void BuiltinView::paint(juce::Graphics& g) {
    g.fillAll(kPurple);
    g.setColour(kHeader);
    g.fillRect(0, 0, kWidth, 34);
    g.setColour(kRed);
    g.setFont(uiFont(16, true));
    g.drawText("PHYZO", 16, 0, 200, 34, juce::Justification::centredLeft);
    g.setFont(uiFont(13));
    g.drawText("v" JucePlugin_VersionString, kWidth - 216, 0, 200, 34, juce::Justification::centredRight);
    g.drawHorizontalLine(34, 0, float(kWidth));

    paintDisplay(g, {140, 50, 240, 60});
    g.setColour(kRed);
    g.setFont(uiFont(12));
    g.drawText("previous / next preset", 140, 116, 240, 16, juce::Justification::centred);

    paintStatus(g, 146);
    g.setColour(kRed.withAlpha(0.6f));
    g.drawHorizontalLine(212, 0, float(kWidth));
    if (proc.showDebug) paintDebug(g, 212);
    g.setColour(kRed.withAlpha(0.6f));                                  // resize grip
    for (int i = 1; i <= 3; ++i) {
        const float d = float(i) * 4.0f;
        g.drawLine(float(kWidth) - d, float(kHeight) - 2.0f, float(kWidth) - 2.0f, float(kHeight) - d, 1.0f);
    }
}

void BuiltinView::paintDisplay(juce::Graphics& g, juce::Rectangle<int> area) {
    g.setColour(kWindow);
    g.fillRoundedRectangle(area.toFloat(), 4);
    g.setColour(kRed);
    g.drawRoundedRectangle(area.toFloat(), 4, 1);
    const bool noRoms = proc.engine().state() == Engine::State::NoRoms;
    const std::array<uint8_t, 4> bytes = noRoms ? std::array<uint8_t, 4>{1, 1, 1, 1} : proc.engine().segments();   // dashes
    const float x0 = float(area.getX()) + 26, y0 = float(area.getY()) + 10;
    for (int i = 0; i < 4; ++i) drawDigit(g, {x0 + float(i) * 50, y0, 26, 40}, bytes[size_t(i)]);
    const uint8_t dots = noRoms ? 0 : proc.engine().displayDots();            // 8 = colon, 4 = point
    auto dot = [&](float x, float y, bool lit) { g.setColour(lit ? kRed : kGhost); g.fillEllipse(x - 3, y - 3, 6, 6); };
    dot(x0 + 88, y0 + 12, dots & 8);
    dot(x0 + 88, y0 + 28, dots & 8);
    dot(x0 + 138, y0 + 40, dots & 4);
}

void BuiltinView::paintStatus(juce::Graphics& g, int y) {
    g.setColour(kRed);
    g.setFont(uiFont(13, true));
    g.drawText("Status", 20, y, 70, 18, juce::Justification::centredLeft);
    g.setFont(uiFont(13));
    const romid::ScanResult& s = proc.romScan();
    juce::String line1, line2;
    if (!s.complete()) {
        // Only when a ROM is missing (or has the wrong checksum): which one, and where it goes.
        juce::String what = !s.hasOs() && !s.hasWave() ? "the OS image and the native wave image" : !s.hasOs() ? "the OS image" : "the native wave image";
        line1 = "Missing (or wrong checksum): " + what + ".";
        line2 = "Copy " + juce::String(!s.hasOs() && !s.hasWave() ? "them" : "it") + " into " + home(proc.romFolder());   // same as romMessage()
    } else {
        switch (proc.engine().state()) {
        case Engine::State::NoRoms: line1 = "waiting for the ROMs"; break;
        case Engine::State::Booting: line1 = juce::String::fromUTF8("booting\xe2\x80\xa6"); break;
        case Engine::State::Running:
            line1 = "running " + juce::String::fromUTF8("\xc2\xb7") + " 44.1 kHz " + juce::String::fromUTF8("\xe2\x86\x92") + " host " + rateText(proc.hostRate());
            break;
        case Engine::State::Stopped: line1 = "stopped"; break;
        }
        line2 = juce::String(proc.engine().message());
    }
    // Input Clip LED (approved mockup): right end of the Status row; lit while Audio In reaches the clip level. Not
    // while a ROM is missing (that message needs the whole row).
    g.drawText(line1, 100, y, kWidth - (s.complete() ? 190 : 110), 18, juce::Justification::centredLeft);
    if (s.complete()) {
        const bool clip = proc.engine().inputClip();
        const juce::Rectangle<float> dot(float(kWidth) - 92, float(y) + 5, 8, 8);
        if (clip) {
            g.setColour(kRed.withAlpha(0.3f)); g.fillEllipse(dot.expanded(3));   // glow
            g.setColour(kRed); g.fillEllipse(dot);
        } else {
            g.setColour(kGhost); g.fillEllipse(dot);
            g.setColour(kRed.withAlpha(0.6f)); g.drawEllipse(dot, 1.0f);
        }
        g.setColour(kRed.withAlpha(clip ? 1.0f : 0.45f));
        g.setFont(uiFont(11, true));
        g.drawText("IN CLIP", kWidth - 78, y, 66, 18, juce::Justification::centredLeft);
        g.setColour(kRed);
    }
    g.setFont(uiFont(12));
    if (line2.isNotEmpty()) g.drawFittedText(line2, 100, y + 18, kWidth - 110, 16, juce::Justification::topLeft, 1);
    if (notice_.isNotEmpty()) g.drawFittedText(notice_, 20, y + 36, kWidth - 30, 28, juce::Justification::topLeft, 2, 0.9f);
}

void BuiltinView::paintDebug(juce::Graphics& g, int y) {
    const auto& m = proc.meter;
    g.setFont(uiFont(12));
    g.setColour(kRed);
    const float avg = m.avg.load() * 100, peak = m.peak.load() * 100;
    g.drawText("CPU per block", 20, y + 34, 100, 16, juce::Justification::centredLeft);
    g.drawText("avg " + juce::String(avg, 1) + " %", 130, y + 34, 90, 16, juce::Justification::centredLeft);
    g.drawText("peak " + juce::String(peak, 1) + " %", 220, y + 34, 100, 16, juce::Justification::centredLeft);
    // history: one bar per second, newest on the right; full height = 100 %
    const uint32_t head = m.head.load();
    const int n = int(m.history.size()), bx = 330, bw = 10, bh = 16;
    for (int i = 0; i < n; ++i) {
        const float v = juce::jlimit(0.0f, 1.0f, m.history[size_t((head + uint32_t(i)) % uint32_t(n))].load());
        g.setColour(kRed.withAlpha(0.6f));
        g.drawRect(bx + i * (bw + 2), y + 34, bw, bh, 1);
        g.setColour(kRed);
        const int hh = int(std::round(v * bh));
        g.fillRect(bx + i * (bw + 2), y + 34 + bh - hh, bw, hh);
    }
    const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");
    g.setColour(kRed.withAlpha(0.85f));
    g.setFont(uiFont(11));
    g.drawText("block " + juce::String(m.blockSize.load()) + " smp" + dot + juce::String(m.blockMs.load(), 1) + " ms" + dot + "last " +
                   juce::String(m.lastMs.load(), 2) + " ms" + dot + "overruns " + juce::String(juce::int64(m.overruns.load())),
               20, y + 52, kWidth - 30, 16, juce::Justification::centredLeft);
    g.drawText("worst block " + juce::String(m.worstMs.load(), 2) + " ms" + dot + "audio lock wait " +
                   juce::String(proc.engine().maxLockWaitUs() / 1000.0, 2) + " ms" + dot + "state requests " +
                   juce::String(juce::int64(m.stateRequests.load())),
               20, y + 68, kWidth - 30, 16, juce::Justification::centredLeft);
    // Per part: average / peak share of the block budget over the last second.
    static const char* const names[Meter_kParts] = {"68k+dev", "voice", "ESP2", "rate", "queue"};
    juce::String parts = "% avg/peak:";
    for (int i = 0; i < Meter_kParts; ++i)
        parts << " " << names[i] << " " << juce::String(m.partAvg[size_t(i)].load() * 100, 1) << "/"
              << juce::String(m.partPeak[size_t(i)].load() * 100, 1) << (i + 1 < Meter_kParts ? dot.trimEnd() : juce::String());
    g.drawText(parts, 20, y + 84, kWidth - 30, 16, juce::Justification::centredLeft);
}

// ================================================================== editor

PhyzoEditor::PhyzoEditor(PhyzoProcessor& p) : AudioProcessorEditor(&p), proc(p) {
    setConstrainer(&constrainer_);
    setResizable(true, false);                      // our own corner drag (JUCE's corner would sit under the OpenGL view)
    juce::String want = SkinSettings::skin();
    if (want.isEmpty()) want = skinNames().contains(SkinSettings::kDefaultSkin) ? SkinSettings::kDefaultSkin : SkinSettings::kBuiltIn;
    // Size: the project's own if it has one (a file skin's size saved on the old scale reopens at the same physical
    // size), else the global setting.
    const float projectScale = proc.editorScale.load();
    if (projectScale >= winscale::kMin && projectScale <= (proc.editorScaleLegacy ? 2.0f : winscale::kMax))
        scale_ = proc.editorScaleLegacy && want != SkinSettings::kBuiltIn ? winscale::fromLegacy(projectScale) : projectScale;
    else
        scale_ = SkinSettings::scale();
    applying_ = true;                                // a placeholder size, not a resize by the user: keeps scale_
    setSize(BuiltinView::kWidth, BuiltinView::kHeight);
    applying_ = false;
    showSkin(want);
    startTimer(1000);
}

// The same short message as Built-in's Status line, over a file skin, while a ROM is missing.
static juce::String romMessage(const PhyzoProcessor& proc) {
    const romid::ScanResult& s = proc.romScan();
    if (s.complete()) return {};
    const juce::String what = !s.hasOs() && !s.hasWave() ? "the OS image and the native wave image" : !s.hasOs() ? "the OS image" : "the native wave image";
    return "Missing (or wrong checksum): " + what + ". Copy " + juce::String(!s.hasOs() && !s.hasWave() ? "them" : "it") + " into " +
           home(proc.romFolder());
}

void PhyzoEditor::timerCallback() {
    if (rml_) rml_->setPersistentMessage(romMessage(proc));
    if (scaleDirty_) { scaleDirty_ = false; SkinSettings::setScale(scale_); }   // not on every drag step
}

PhyzoEditor::~PhyzoEditor() {
    stopTimer();
    proc.setEditorActive(false);                     // no window shows it: the editor sends nothing
    if (scaleDirty_) SkinSettings::setScale(scale_);
    rml_.reset();                                    // closes its OpenGL context first
    builtin_.reset();
}

juce::StringArray PhyzoEditor::skinNames() const {
    juce::StringArray names;
    for (const auto& s : skin::discoverSkins(proc.skinsFolder().getFullPathName().toStdString())) names.add(juce::String::fromUTF8(s.name.c_str()));
    return names;
}

juce::File PhyzoEditor::skinFolder(const juce::String& name) const { return proc.skinsFolder().getChildFile(name); }

void PhyzoEditor::showSkin(const juce::String& name, const juce::String& notice) {
    if (name == SkinSettings::kBuiltIn) { showBuiltIn(notice); return; }
    if (!skinNames().contains(name)) {
        skinFailed(name, "it is not in " + home(proc.skinsFolder()) + " (a skin is a folder <name> containing <name>.rml)");
        return;
    }
    builtin_.reset();
    if (!rml_) {
        rml_ = std::make_unique<RmlSkinComponent>(proc);
        rml_->onRightClick = [this](const juce::MouseEvent& e) { showMenu(e); };
        rml_->onCornerResize = [this](int phase, juce::Point<int> d) { cornerResize(phase, d); };
        addAndMakeVisible(*rml_);
    }
    current_ = name;
    rml_->onLoaded = [this, name, notice](bool ok, const juce::String& error) {
        if (!ok) { skinFailed(name, error); return; }
        if (notice.isNotEmpty() && rml_) rml_->showMessage(notice);
        proc.loadWaveNames(skinFolder(name));
        updateEditorUnit();
    };
    rml_->setDebugger(debugger_);
    rml_->setPersistentMessage(romMessage(proc));
    rml_->loadSkin(skinFolder(name), name);
    applyScale(scale_);
}

void PhyzoEditor::showBuiltIn(const juce::String& notice) {
    rml_.reset();
    editorShown_ = false;                            // Built-in has no editor unit
    proc.setEditorActive(false);
    if (!builtin_) {
        builtin_ = std::make_unique<BuiltinView>(proc);
        builtin_->onRightClick = [this](const juce::MouseEvent& e) { showMenu(e); };
        builtin_->onCornerResize = [this](int phase, juce::Point<int> d) { cornerResize(phase, d); };
        addAndMakeVisible(*builtin_);
    }
    current_ = SkinSettings::kBuiltIn;
    if (notice.isNotEmpty()) builtin_->setNotice(notice);
    applyScale(scale_);
}

// A skin that is missing or fails to load: rack, then Built-in, saying why.
// (Deferred: this can be called from inside the failed skin's own callback.)
void PhyzoEditor::skinFailed(const juce::String& name, const juce::String& reason) {
    const juce::String why = "The skin \"" + name + "\" could not be shown: " + reason.trim();
    juce::MessageManager::callAsync([sp = juce::Component::SafePointer<PhyzoEditor>(this), name, why] {
        if (!sp) return;
        if (name != SkinSettings::kDefaultSkin && sp->skinNames().contains(SkinSettings::kDefaultSkin))
            sp->showSkin(SkinSettings::kDefaultSkin, why + juce::String::fromUTF8(" \xe2\x80\x94 showing \"rack\"."));
        else
            sp->showBuiltIn(why);
    });
}

juce::Point<int> PhyzoEditor::baseSize() const {
    if (rml_) return {juce::roundToInt(skin::kBodyWidthDp), juce::roundToInt(skin::kBodyHeightDp + (editorShown_ ? skin::kEditorHeightDp : 0.0f))};
    return {BuiltinView::kWidth, BuiltinView::kHeight};
}

// Dragging the bottom-right corner: follow whichever direction moved further, keeping the aspect ratio.
void PhyzoEditor::cornerResize(int phase, juce::Point<int> d) {
    if (phase == 0) { dragStartBounds_ = getBounds(); return; }
    const auto base = baseSize();
    const float aspect = float(base.x) / float(base.y);
    const int dy = juce::roundToInt(float(d.y) * aspect);
    const int dw = std::abs(d.x) >= std::abs(dy) ? d.x : dy;
    auto b = dragStartBounds_.withWidth(dragStartBounds_.getWidth() + dw);
    b.setHeight(juce::roundToInt(float(b.getWidth()) / aspect));
    constrainer_.setBoundsForComponent(this, b, false, false, true, true);   // 50-200 %, aspect ratio
}

// The editor unit (F-30): attached below the panel when the project wants it and the skin has one; the window grows by
// its height at the same scale. Only while it is shown does the editor read from the OS.
void PhyzoEditor::updateEditorUnit() {
    const bool show = proc.editorWanted() && rml_ && rml_->hasEditor();
    if (rml_) rml_->setEditorVisible(show);
    proc.setEditorActive(show);
    if (show != editorShown_) {
        editorShown_ = show;
        applyScale(scale_);
    }
}

// Resizable by the corner (or the host), locked to the skin's aspect ratio, 50-200 % of its 100 % size (winscale).
void PhyzoEditor::applyScale(float scale) {
    scale_ = winscale::clamp(scale);
    const auto base = baseSize();
    const bool file = rml_ != nullptr;
    constrainer_.setFixedAspectRatio(double(base.x) / double(base.y));
    constrainer_.setSizeLimits(winscale::windowLength(base.x, winscale::kMin, file), winscale::windowLength(base.y, winscale::kMin, file),
                               winscale::windowLength(base.x, winscale::kMax, file), winscale::windowLength(base.y, winscale::kMax, file));
    applying_ = true;
    setSize(winscale::windowLength(base.x, scale_, file), winscale::windowLength(base.y, scale_, file));
    applying_ = false;
    resized();
}

void PhyzoEditor::resized() {
    const auto base = baseSize();
    const bool file = rml_ != nullptr;
    // A resize by the user or the host: the width sets the scale (not when it is the width of the current scale,
    // so rounding does not move a menu step).
    if (!applying_ && getWidth() > 0 && getWidth() != winscale::windowLength(base.x, scale_, file)) {
        const float s = winscale::scaleForWidth(getWidth(), base.x, file);
        if (std::abs(s - scale_) > 1e-4f) { scale_ = s; scaleDirty_ = true; }
    }
    proc.editorScale = scale_;                       // saved with the project, on the new scale
    proc.editorScaleLegacy = false;
    if (builtin_) {
        builtin_->setTransform(juce::AffineTransform::scale(float(getWidth()) / float(base.x), float(getHeight()) / float(base.y)));
        builtin_->setBounds(0, 0, BuiltinView::kWidth, BuiltinView::kHeight);
    }
    if (rml_) {
        rml_->setZoom(float(getWidth()) / float(base.x));   // the dp ratio follows the window: art drawn at its size
        rml_->setBounds(getLocalBounds());
    }
}

void PhyzoEditor::showMenu(const juce::MouseEvent&) {
    const juce::StringArray names = skinNames();
    const bool fileSkin = rml_ != nullptr;
    juce::PopupMenu skins, zoom, dev, menu;
    skins.addItem(SkinSettings::kBuiltIn, true, current_ == SkinSettings::kBuiltIn, [this] {
        SkinSettings::setSkin(SkinSettings::kBuiltIn);
        showSkin(SkinSettings::kBuiltIn);
    });
    for (const juce::String& n : names)
        skins.addItem(n, true, current_ == n, [this, n] { SkinSettings::setSkin(n); showSkin(n); });
    for (int z : winscale::kSteps)
        zoom.addItem(juce::String(winscale::stepLabel(z, fileSkin)), true, winscale::isStep(scale_, z), [this, z] {
            applyScale(winscale::stepScale(z));
            scaleDirty_ = false;
            SkinSettings::setScale(scale_);
        });
    dev.addItem("RmlUi debugger", fileSkin, fileSkin && debugger_, [this] {
        debugger_ = !debugger_;
        if (rml_) rml_->setDebugger(debugger_);
    });
    dev.addItem("Write skin log", true, SkinSettings::skinLog(), [] { SkinSettings::setSkinLog(!SkinSettings::skinLog()); });
    // MIDI ports (approved mockup): global settings, applied to every open instance.
    juce::PopupMenu midi;
    const VirtualMidiPort& vp = proc.virtualPort();
    const bool portWanted = SkinSettings::virtualMidiPort();
    const juce::String portLabel = vp.isOpen() ? "Virtual port \"" + juce::String(vp.name()) + "\""
                                   : portWanted ? juce::String("Virtual port (unavailable)") : juce::String("Virtual port");
    midi.addItem(portLabel, true, vp.isOpen(), [this, portWanted] {
        SkinSettings::setVirtualMidiPort(!portWanted);
        PhyzoProcessor::applyMidiSettingsToAll();
        const VirtualMidiPort& p = proc.virtualPort();
        if (!portWanted && !p.isOpen()) {
            const juce::String why = "The virtual MIDI port could not be opened: " + juce::String(p.error());
            if (rml_) rml_->showMessage(why); else if (builtin_) builtin_->setNotice(why);
        }
    });
    midi.addItem("Send MIDI to host", true, SkinSettings::midiToHost(), [] {
        SkinSettings::setMidiToHost(!SkinSettings::midiToHost());
        PhyzoProcessor::applyMidiSettingsToAll();
    });
    menu.addSubMenu("Skin", skins);
    menu.addItem("Reload skin    F5", fileSkin, false, [this] { if (rml_) rml_->reload(); });
    menu.addSubMenu("Zoom", zoom);
    menu.addItem("Editor", fileSkin && rml_->hasEditor(), editorShown_, [this] {
        proc.setEditorWanted(!proc.editorWanted());
        updateEditorUnit();
    });
    menu.addSubMenu("MIDI", midi);
    menu.addSeparator();
    menu.addSubMenu("Developer", dev);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}
