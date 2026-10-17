#include "PhyzoWindow.h"
#include "Diag.h"
#include "LibraryText.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SkinSettings.h"
#include "WindowScale.h"

namespace {
juce::String u8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
std::string str(const juce::String& s) { return s.toStdString(); }

juce::String homePath(const juce::File& f) {
    const juce::String h = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName();
    const juce::String p = f.getFullPathName();
    return p.startsWith(h) ? "~" + p.substring(h.length()) : p;
}

constexpr float kRowGap = 8, kHeadingH = 30, kLabelW = 190;

void drawHeading(juce::Graphics& g, const ui::Theme& t, const juce::String& text, juce::Rectangle<float> r) {
    ui::drawText(g, text, t.font(ui::dp::headingText, true), r, ui::colours::heading, juce::Justification::bottomLeft);
}

juce::Font mono(float px) {
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), px, juce::Font::plain).withPointHeight(px));
}

// A settings row: the label, a muted detail, a control at the right (a child owned by the page). With detailBelow, the
// detail goes under the label and wraps to as many lines as it needs (heightFor).
class SettingRow : public juce::Component {
public:
    SettingRow(const ui::Theme& t, juce::String label, bool detailBelow = false)
        : theme_(t), label_(std::move(label)), below_(detailBelow) {}
    void setDetail(const juce::String& d) { if (d != detail_) { detail_ = d; repaint(); } }
    void setLabel(const juce::String& l) { if (l != label_) { label_ = l; repaint(); } }
    const juce::String& detail() const { return detail_; }
    void setControl(juce::Component* c, float width, float height) { control_ = c; cw_ = width; ch_ = height; addAndMakeVisible(c); resized(); }
    void resized() override {
        if (control_) control_->setBounds(juce::Rectangle<float>(float(getWidth()) - 12 - cw_, (float(getHeight()) - ch_) / 2, cw_, ch_).toNearestInt());
    }
    // The row's height at `width` (detailBelow: the label line, then the detail's lines).
    float heightFor(float width) const {
        if (!below_ || detail_.isEmpty()) return ui::dp::fileRow;
        return 12 + kLabelLine + 2 + ui::wrappedHeight(detail_, theme_.font(ui::dp::fileText), textWidth(width)) + 12;
    }
    void paint(juce::Graphics& g) override {
        const auto r = getLocalBounds().toFloat();
        ui::drawRow(g, r.reduced(0.5f), ui::dp::fileRadius, false);
        auto inner = r.reduced(14, 0);
        if (control_) inner.removeFromRight(cw_ + 12);
        if (below_ && detail_.isNotEmpty()) {
            inner = inner.withTrimmedTop(12);
            ui::drawText(g, label_, theme_.font(ui::dp::buttonText), inner.removeFromTop(kLabelLine), ui::colours::text);
            ui::drawWrapped(g, detail_, theme_.font(ui::dp::fileText), inner.withTrimmedTop(2), ui::colours::muted);
            return;
        }
        ui::drawText(g, label_, theme_.font(ui::dp::buttonText), inner.removeFromLeft(kLabelW), ui::colours::text);
        ui::drawText(g, detail_, theme_.font(ui::dp::fileText), inner, ui::colours::muted);
    }

private:
    static constexpr float kLabelLine = 20;
    float textWidth(float width) const { return width - 28 - (control_ ? cw_ + 12 : 0); }
    const ui::Theme& theme_;
    juce::String label_, detail_;
    bool below_;
    juce::Component* control_ = nullptr;
    float cw_ = 0, ch_ = 0;
};

// Rows to choose one of (the skins): the current one in the selection colours.
class ChoiceList : public juce::Component {
public:
    explicit ChoiceList(const ui::Theme& t) : theme_(t) {}
    void set(const juce::StringArray& items, const juce::String& current) {
        if (items == items_ && current == current_) return;
        items_ = items; current_ = current; repaint();
    }
    const juce::StringArray& items() const { return items_; }
    std::function<void(const juce::String&)> onChoose;
    void choose(const juce::String& item) { if (items_.contains(item) && onChoose) onChoose(item); }
    void paint(juce::Graphics& g) override {
        const auto mouse = getMouseXYRelative();
        for (int i = 0; i < items_.size(); ++i) {
            const auto r = rowBounds(i);
            if (r.getY() > float(getHeight())) break;
            ui::drawRow(g, r, ui::dp::fileRadius, items_[i] == current_, isMouseOver() && r.contains(mouse.toFloat()));
            ui::drawText(g, items_[i], theme_.font(ui::dp::fileText), r.reduced(14, 0), ui::colours::text);
        }
    }
    void mouseUp(const juce::MouseEvent& e) override {
        for (int i = 0; i < items_.size(); ++i) if (rowBounds(i).contains(e.position)) { choose(items_[i]); return; }
    }
    void mouseMove(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }

private:
    juce::Rectangle<float> rowBounds(int i) const { return {0, float(i) * (ui::dp::fileRow + 6), float(getWidth()), ui::dp::fileRow}; }
    const ui::Theme& theme_;
    juce::StringArray items_;
    juce::String current_;
};
}  // namespace

// ================================================================== SYSTEM

class PhyzoWindow::SystemPage : public juce::Component {
public:
    explicit SystemPage(PhyzoWindow& w)
        : w_(w), version_(w.theme_, "Version"), os_(w.theme_, "OS ROM"), wave_(w.theme_, "Wave ROM"), data_(w.theme_, "Data folder"),
          log_(w.theme_, "Startup log"), debugger_(w.theme_, "RmlUi debugger"), skinLog_(w.theme_, "Write skin log"),
          showData_(w.theme_, "Show"), showLog_(w.theme_, "Show"), debuggerSwitch_(w.theme_), skinLogSwitch_(w.theme_) {
        setComponentID("system");
        for (auto* r : {&version_, &os_, &wave_, &data_, &log_, &debugger_, &skinLog_}) addAndMakeVisible(*r);
        showData_.setComponentID("showData");
        showLog_.setComponentID("showLog");
        debuggerSwitch_.setComponentID("debugger");
        skinLogSwitch_.setComponentID("skinLog");
        const float bw = ui::Button::idealWidth(w.theme_, "Show");
        data_.setControl(&showData_, bw, ui::dp::button - 6);
        log_.setControl(&showLog_, bw, ui::dp::button - 6);
        debugger_.setControl(&debuggerSwitch_, 40, 22);
        skinLog_.setControl(&skinLogSwitch_, 40, 22);
        showData_.onClick = [this] { w_.proc_.dataFolder().revealToUser(); };
        showLog_.onClick = [] { const juce::File f = phyzodiag::logFile(); (f.existsAsFile() ? f : f.getParentDirectory()).revealToUser(); };
        debuggerSwitch_.onChange = [this](bool on) { w_.editor_.setDebugger(on); };
        skinLogSwitch_.onChange = [](bool on) { SkinSettings::setSkinLog(on); };
        refresh();
    }
    void refresh() {
        version_.setDetail(juce::String("Phyzo ") + JucePlugin_VersionString + u8(libtext::kMid) +
                           juce::AudioProcessor::getWrapperTypeDescription(w_.proc_.wrapperType) + u8(libtext::kMid) + phyzodiag::arch());
        const romid::ScanResult& s = w_.proc_.romScan();
        auto name = [](const std::string& path) { return juce::File(u8(path)).getFileName(); };
        os_.setDetail(s.hasOs() ? "found: " + name(s.osPath) : "missing (or wrong checksum): copy it into " + homePath(w_.proc_.romFolder()));
        wave_.setDetail(s.hasWave() ? "found: " + name(s.wavePath) : "missing (or wrong checksum): copy it into " + homePath(w_.proc_.romFolder()));
        data_.setDetail(homePath(w_.proc_.dataFolder()));
        log_.setDetail(phyzodiag::logFile().getFileName());
        debugger_.setDetail(w_.editor_.fileSkin() ? "for file skins" : "for file skins (not Built-in)");
        debuggerSwitch_.setEnabled(w_.editor_.fileSkin());
        debuggerSwitch_.setOn(w_.editor_.fileSkin() && w_.editor_.debuggerOn());
        skinLog_.setDetail(homePath(w_.proc_.dataFolder().getChildFile("skin-log.txt")));
        skinLogSwitch_.setOn(SkinSettings::skinLog());
    }
    void resized() override {
        auto r = getLocalBounds().toFloat();
        r = r.withWidth(std::min(r.getWidth(), 900.0f));
        auto take = [&](float h) { auto x = r.removeFromTop(h); return x; };
        headings_[0] = take(kHeadingH);
        for (auto* row : {&version_, &os_, &wave_, &data_, &log_}) { row->setBounds(take(ui::dp::fileRow).toNearestInt()); take(kRowGap); }
        take(10);
        headings_[1] = take(kHeadingH);
        for (auto* row : {&debugger_, &skinLog_}) { row->setBounds(take(ui::dp::fileRow).toNearestInt()); take(kRowGap); }
    }
    void paint(juce::Graphics& g) override {
        drawHeading(g, w_.theme_, "THIS INSTANCE", headings_[0].withTrimmedBottom(8));
        drawHeading(g, w_.theme_, "DEVELOPER", headings_[1].withTrimmedBottom(8));
    }

private:
    PhyzoWindow& w_;
    SettingRow version_, os_, wave_, data_, log_, debugger_, skinLog_;
    ui::Button showData_, showLog_;
    ui::Switch debuggerSwitch_, skinLogSwitch_;
    juce::Rectangle<float> headings_[2];
};

// ================================================================== SKIN

class PhyzoWindow::SkinPage : public juce::Component {
public:
    explicit SkinPage(PhyzoWindow& w)
        : w_(w), list_(w.theme_), reload_(w.theme_, "Reload skin (F5)"), folder_(w.theme_, "Show skins folder"),
          editorRow_(w.theme_, "Editor unit"), editorSwitch_(w.theme_), zoom_(w.theme_) {
        setComponentID("skin");
        list_.setComponentID("skinList");
        reload_.setComponentID("reload");
        folder_.setComponentID("showSkins");
        editorSwitch_.setComponentID("editorUnit");
        zoom_.setComponentID("zoom");
        addAndMakeVisible(list_);
        addAndMakeVisible(reload_);
        addAndMakeVisible(folder_);
        addAndMakeVisible(editorRow_);
        addAndMakeVisible(zoom_);
        editorRow_.setControl(&editorSwitch_, 40, 22);
        juce::StringArray steps;
        for (int z : winscale::kSteps) steps.add(juce::String(z));
        zoom_.setItems(steps);
        list_.onChoose = [this](const juce::String& name) { w_.editor_.chooseSkin(name); };
        reload_.onClick = [this] { w_.editor_.reloadSkin(); };
        folder_.onClick = [this] { w_.proc_.skinsFolder().revealToUser(); };
        editorSwitch_.onChange = [this](bool on) { w_.editor_.setEditorUnit(on); };
        zoom_.onSelect = [this](int i) { w_.editor_.setZoomStep(winscale::kSteps[i]); };
        refresh();
    }
    void refresh() {
        juce::StringArray items(SkinSettings::kBuiltIn);
        items.addArray(w_.editor_.skinNames());
        list_.set(items, w_.editor_.currentSkin());
        reload_.setEnabled(w_.editor_.fileSkin());
        const bool unit = w_.editor_.hasEditorUnit();
        editorSwitch_.setEnabled(unit);
        editorSwitch_.setOn(unit && w_.editor_.editorUnitShown());
        editorRow_.setDetail(unit ? "the editor below the panel" : w_.editor_.fileSkin() ? "this skin has none" : "file skins only");
        const float limit = w_.editor_.maxScale();
        int sel = -1, i = 0;
        for (int z : winscale::kSteps) {
            zoom_.setItemEnabled(i, winscale::stepFits(z, limit));
            if (winscale::isStep(w_.editor_.scale(), z)) sel = i;
            ++i;
        }
        zoom_.setSelected(sel);
    }
    void resized() override {
        auto r = getLocalBounds().toFloat();
        auto left = r.removeFromLeft(std::min(420.0f, r.getWidth() * 0.4f));
        r.removeFromLeft(ui::dp::gap * 2);
        headings_[0] = left.removeFromTop(kHeadingH);
        list_.setBounds(left.toNearestInt());
        r = r.withWidth(std::min(r.getWidth(), 760.0f));
        headings_[1] = r.removeFromTop(kHeadingH);
        auto buttons = r.removeFromTop(ui::dp::button);
        const float w1 = ui::Button::idealWidth(w_.theme_, reload_.label()), w2 = ui::Button::idealWidth(w_.theme_, folder_.label());
        reload_.setBounds(buttons.removeFromLeft(w1).toNearestInt());
        buttons.removeFromLeft(10);
        folder_.setBounds(buttons.removeFromLeft(w2).toNearestInt());
        r.removeFromTop(16);
        editorRow_.setBounds(r.removeFromTop(ui::dp::fileRow).toNearestInt());
        r.removeFromTop(10);
        headings_[2] = r.removeFromTop(kHeadingH);
        zoom_.setBounds(r.removeFromTop(ui::dp::button).withWidth(std::min(r.getWidth(), std::max(zoom_.idealWidth(), 520.0f))).toNearestInt());
    }
    void paint(juce::Graphics& g) override {
        drawHeading(g, w_.theme_, "SKINS", headings_[0].withTrimmedBottom(8));
        drawHeading(g, w_.theme_, "THIS SKIN", headings_[1].withTrimmedBottom(8));
        drawHeading(g, w_.theme_, "ZOOM  %", headings_[2].withTrimmedBottom(8));
    }

private:
    PhyzoWindow& w_;
    ChoiceList list_;
    ui::Button reload_, folder_;
    SettingRow editorRow_;
    ui::Switch editorSwitch_;
    ui::Segmented zoom_;
    juce::Rectangle<float> headings_[3];
};

// ================================================================== MIDI

namespace {
// MIDI learn's bindings: control, MIDI (CC n / Note n), channel (Any / n), Forget per row.
class LearnTable : public juce::Component {
public:
    LearnTable(const ui::Theme& t, PhyzoProcessor& p) : theme_(t), proc_(p) { setComponentID("learnTable"); }
    struct Row { int control; juce::String name, midi, channel; };
    void refresh() {
        std::vector<Row> rows;
        const midilearn::PerfLabels labels = proc_.perfLabels();
        for (const midilearn::Binding& b : proc_.learn().bindings())
            rows.push_back({b.control, u8(midilearn::controlName(b.control, labels)),
                            juce::String(b.note ? "Note " : "CC ") + juce::String(b.number) + (b.fine ? " (14-bit)" : ""),
                            b.channel == 0 ? juce::String("Any") : juce::String(b.channel)});
        bool same = rows.size() == rows_.size();
        for (size_t i = 0; same && i < rows.size(); ++i)
            same = rows[i].control == rows_[i].control && rows[i].name == rows_[i].name && rows[i].midi == rows_[i].midi && rows[i].channel == rows_[i].channel;
        if (same) return;
        rows_ = std::move(rows);
        scroll_ = juce::jlimit(0, std::max(0, int(rows_.size()) - visibleRows()), scroll_);
        repaint();
    }
    const std::vector<Row>& rows() const { return rows_; }
    void forget(int control) { proc_.forgetLearn(control); refresh(); }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        const auto cols = columns(r.getWidth());
        auto head = r.removeFromTop(26);
        const juce::Font hf = theme_.font(ui::dp::headingText, true);
        ui::drawText(g, "CONTROL", hf, head.withX(cols[0]).withWidth(cols[1] - cols[0]), ui::colours::heading);
        ui::drawText(g, "MIDI", hf, head.withX(cols[1]).withWidth(cols[2] - cols[1]), ui::colours::heading);
        ui::drawText(g, "CHANNEL", hf, head.withX(cols[2]).withWidth(cols[3] - cols[2]), ui::colours::heading);
        r.removeFromTop(4);
        if (rows_.empty()) { ui::drawText(g, "No bindings", theme_.font(ui::dp::fileText), r.removeFromTop(kRow), ui::colours::muted); return; }
        const auto mouse = getMouseXYRelative().toFloat();
        for (int i = scroll_; i < int(rows_.size()); ++i) {
            const auto row = r.removeFromTop(kRow - 4);
            r.removeFromTop(4);
            if (row.getBottom() > float(getHeight())) break;
            ui::drawRow(g, row, 8, false);
            const Row& x = rows_[size_t(i)];
            const juce::Font f = theme_.font(ui::dp::fileText);
            ui::drawText(g, x.name, f, row.withX(cols[0] + 10).withWidth(cols[1] - cols[0] - 14), ui::colours::text);
            ui::drawText(g, x.midi, f, row.withX(cols[1]).withWidth(cols[2] - cols[1] - 4), ui::colours::text);
            ui::drawText(g, x.channel, f, row.withX(cols[2]).withWidth(cols[3] - cols[2] - 4), ui::colours::text);
            const auto fb = forgetBounds(row, cols);
            g.setColour(fb.contains(mouse) ? ui::colours::button.brighter(0.12f) : ui::colours::button);
            g.fillRoundedRectangle(fb, 6);
            g.setColour(ui::colours::buttonEdge);
            g.drawRoundedRectangle(fb, 6, 1.0f);
            ui::drawText(g, "Forget", theme_.font(ui::dp::fileText), fb, ui::colours::text, juce::Justification::centred);
        }
        if (scroll_ > 0 || scroll_ + visibleRows() < int(rows_.size()))
            ui::drawText(g, juce::String(scroll_ + 1) + u8(libtext::kEnDash) + juce::String(std::min(int(rows_.size()), scroll_ + visibleRows())) +
                                " of " + juce::String(int(rows_.size())),
                         theme_.font(ui::dp::headingText), head, ui::colours::muted, juce::Justification::bottomRight);
    }
    void mouseUp(const juce::MouseEvent& e) override {
        auto r = getLocalBounds().toFloat();
        const auto cols = columns(r.getWidth());
        r.removeFromTop(30);
        for (int i = scroll_; i < int(rows_.size()); ++i) {
            const auto row = r.removeFromTop(kRow - 4);
            r.removeFromTop(4);
            if (forgetBounds(row, cols).contains(e.position)) { forget(rows_[size_t(i)].control); return; }
        }
    }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& d) override {
        const int n = std::max(0, int(rows_.size()) - visibleRows());
        scroll_ = juce::jlimit(0, n, scroll_ + (d.deltaY < 0 ? 1 : -1));
        repaint();
    }
    void mouseMove(const juce::MouseEvent&) override { repaint(); }

private:
    static constexpr float kRow = 38;
    int visibleRows() const { return std::max(1, int((float(getHeight()) - 30) / kRow)); }
    static std::array<float, 4> columns(float w) { return {0, w * 0.42f, w * 0.66f, w * 0.82f}; }
    static juce::Rectangle<float> forgetBounds(juce::Rectangle<float> row, const std::array<float, 4>& cols) {
        return juce::Rectangle<float>(cols[3], row.getY() + 5, row.getRight() - cols[3] - 6, row.getHeight() - 10);
    }
    const ui::Theme& theme_;
    PhyzoProcessor& proc_;
    std::vector<Row> rows_;
    int scroll_ = 0;
};

juce::String syncExplanation(midiclock::Mode m) {
    switch (m) {
        case midiclock::Mode::Off: return "No MIDI clock: the host's MIDI goes to the synth as it comes.";
        case midiclock::Mode::WhilePlaying: return "MIDI clock at the host's tempo while its transport plays.";
        case midiclock::Mode::Always: return "MIDI clock at the host's tempo, also while its transport is stopped.";
    }
    return {};
}
}  // namespace

class PhyzoWindow::MidiPage : public juce::Component {
public:
    explicit MidiPage(PhyzoWindow& w)
        : w_(w), portRow_(w.theme_, "Virtual port", true), hostRow_(w.theme_, "Send MIDI to host", true), portSwitch_(w.theme_), hostSwitch_(w.theme_),
          sync_(w.theme_), table_(w.theme_, w.proc_), saveDefault_(w.theme_, "Save as default"), clearDefault_(w.theme_, "Clear default"),
          forgetAll_(w.theme_, "Forget all") {
        setComponentID("midi");
        portSwitch_.setComponentID("virtualPort");
        hostSwitch_.setComponentID("midiToHost");
        sync_.setComponentID("sync");
        saveDefault_.setComponentID("saveDefault");
        clearDefault_.setComponentID("clearDefault");
        forgetAll_.setComponentID("forgetAll");
        for (juce::Component* c : std::initializer_list<juce::Component*>{&portRow_, &hostRow_, &sync_, &table_, &saveDefault_, &clearDefault_, &forgetAll_})
            addAndMakeVisible(c);
        portRow_.setControl(&portSwitch_, 40, 22);
        hostRow_.setControl(&hostSwitch_, 40, 22);
        sync_.setItems({"Off", "While playing", "Always"});
        portSwitch_.onChange = [this](bool on) {
            SkinSettings::setVirtualMidiPort(on);
            PhyzoProcessor::applyMidiSettingsToAll();
            if (on && !w_.proc_.virtualPort().isOpen())
                w_.message("The virtual MIDI port could not be opened: " + juce::String(w_.proc_.virtualPort().error()));
            refresh();
        };
        hostSwitch_.onChange = [this](bool on) { SkinSettings::setMidiToHost(on); PhyzoProcessor::applyMidiSettingsToAll(); };
        sync_.onSelect = [this](int i) { w_.proc_.setSyncMode(midiclock::Mode(i)); refresh(); };
        saveDefault_.onClick = [this] { w_.proc_.saveLearnDefaults(); w_.message("Saved as the default for new instances"); refresh(); };
        clearDefault_.onClick = [this] { w_.proc_.clearLearnDefaults(); w_.message("Default bindings cleared"); refresh(); };
        forgetAll_.onClick = [this] { w_.proc_.forgetAllLearn(); refresh(); };
        refresh();
    }
    void refresh() {
        const VirtualMidiPort& vp = w_.proc_.virtualPort();
        const bool wanted = SkinSettings::virtualMidiPort();
        portRow_.setLabel(vp.isOpen() ? "Virtual port \"" + juce::String(vp.name()) + "\"" : wanted ? juce::String("Virtual port (unavailable)") : juce::String("Virtual port"));
        portRow_.setDetail("a MIDI in and out for other apps");
        portSwitch_.setOn(vp.isOpen());
        hostRow_.setDetail("the synth's MIDI output to the host's track");
        hostSwitch_.setOn(SkinSettings::midiToHost());
        sync_.setSelected(int(w_.proc_.syncMode()));
        const juce::String text = syncExplanation(w_.proc_.syncMode());
        if (text != syncText_) { syncText_ = text; resized(); repaint(); }
        table_.refresh();
        clearDefault_.setEnabled(SkinSettings::midiLearnDefaults().isNotEmpty());
        forgetAll_.setEnabled(!w_.proc_.learn().bindings().empty());
    }
    void resized() override {
        auto r = getLocalBounds().toFloat();
        auto left = r.removeFromLeft(std::min(560.0f, r.getWidth() * 0.42f));
        r.removeFromLeft(ui::dp::gap * 2);
        headings_[0] = left.removeFromTop(kHeadingH);
        portRow_.setBounds(left.removeFromTop(portRow_.heightFor(left.getWidth())).toNearestInt());
        left.removeFromTop(kRowGap);
        hostRow_.setBounds(left.removeFromTop(hostRow_.heightFor(left.getWidth())).toNearestInt());
        left.removeFromTop(18);
        headings_[1] = left.removeFromTop(kHeadingH);
        sync_.setBounds(left.removeFromTop(ui::dp::button).toNearestInt());
        // The explanation wraps to as many lines as it needs (never cut).
        syncLine_ = left.removeFromTop(6 + ui::wrappedHeight(syncText_, w_.theme_.font(ui::dp::fileText), left.getWidth()));
        headings_[2] = r.removeFromTop(kHeadingH);
        hint_ = r.removeFromBottom(26);
        auto buttons = r.removeFromBottom(ui::dp::button);
        r.removeFromBottom(10);
        table_.setBounds(r.toNearestInt());
        for (ui::Button* b : {&saveDefault_, &clearDefault_, &forgetAll_}) {
            const float bw = ui::Button::idealWidth(w_.theme_, b->label());
            b->setBounds(buttons.removeFromLeft(bw).toNearestInt());
            buttons.removeFromLeft(10);
        }
    }
    void paint(juce::Graphics& g) override {
        drawHeading(g, w_.theme_, "PORTS", headings_[0].withTrimmedBottom(8));
        drawHeading(g, w_.theme_, "SYNC TO HOST TEMPO", headings_[1].withTrimmedBottom(8));
        drawHeading(g, w_.theme_, "MIDI LEARN", headings_[2].withTrimmedBottom(8));
        ui::drawWrapped(g, syncText_, w_.theme_.font(ui::dp::fileText), syncLine_.withTrimmedTop(6), ui::colours::muted);
        ui::drawText(g, "right-click a knob or button to learn it", w_.theme_.font(ui::dp::fileText), hint_.withTrimmedTop(6), ui::colours::muted);
    }

private:
    PhyzoWindow& w_;
    SettingRow portRow_, hostRow_;
    ui::Switch portSwitch_, hostSwitch_;
    ui::Segmented sync_;
    LearnTable table_;
    ui::Button saveDefault_, clearDefault_, forgetAll_;
    juce::String syncText_;
    juce::Rectangle<float> headings_[3], syncLine_, hint_;
};

// ================================================================== LIBRARY

namespace {
using Group = library::Source::Group;

// One line of the FILES column: a group's heading or a file (SINGLES: one line for all of them).
struct FileItem {
    bool heading = false;
    Group group = Group::Banks;
    int source = -1;
    juce::String label, count, path;
    bool busy = false, bank = false;
};

std::vector<FileItem> fileItems(const std::vector<library::Service::SourceView>& sources) {
    std::vector<FileItem> out;
    int last = -1;
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto& v = sources[i];
        if (int(v.group) != last) {
            last = int(v.group);
            FileItem h; h.heading = true; h.group = v.group; h.label = u8(libtext::groupHeading(v.group));
            out.push_back(h);
        }
        FileItem f;
        f.group = v.group; f.source = int(i); f.label = u8(v.label); f.count = u8(libtext::count(v)); f.path = u8(v.path);
        f.busy = v.busy; f.bank = v.bank;
        out.push_back(f);
    }
    return out;
}

// An inline text edit over a row (Rename): Enter commits, Esc (or leaving it) cancels.
class InlineEdit : public juce::TextEditor {
public:
    explicit InlineEdit(const ui::Theme& t) : theme_(t) {
        ui::styleField(*this, t, ui::dp::fileText);
        onReturnKey = [this] { finish(true); };
        onEscapeKey = [this] { finish(false); };
        onFocusLost = [this] { finish(false); };
    }
    void begin(juce::Component& host, juce::Rectangle<int> r, const juce::String& text, std::function<void(const juce::String&)> commit) {
        commit_ = std::move(commit);
        host.addAndMakeVisible(this);
        ui::styleField(*this, theme_, ui::dp::fileText);
        setText(text, false);
        setBounds(r);
        grabKeyboardFocus();
        selectAll();
    }
    bool active() const { return isVisible() && getParentComponent() != nullptr; }
    void finish(bool commit) {
        if (!active()) return;
        auto f = std::move(commit_);
        commit_ = nullptr;
        const juce::String text = getText();
        setVisible(false);
        if (auto* p = getParentComponent()) p->removeChildComponent(this);
        if (commit && f) f(text);
    }

private:
    const ui::Theme& theme_;
    std::function<void(const juce::String&)> commit_;
};
}  // namespace

class PhyzoWindow::LibraryPage : public juce::Component {
public:
    explicit LibraryPage(PhyzoWindow& w)
        : w_(w), files_(*this), presets_(*this), memory_(*this), import_(w.theme_, "Import" + u8(libtext::kDots)), folder_(w.theme_, "Folder"),
          edit_(w.theme_) {
        setComponentID("library");
        import_.setComponentID("import");
        folder_.setComponentID("folder");
        addAndMakeVisible(files_);
        addAndMakeVisible(presets_);
        addAndMakeVisible(memory_);
        addAndMakeVisible(import_);
        addAndMakeVisible(folder_);
        import_.onClick = [this] { w_.proc_.chooseImport(); };
        folder_.onClick = [this] { w_.proc_.revealLibrary(); };
    }
    // Opened (the tab shown): rescan; the factory files are made the first time.
    void opened() {
        if (library::Service* s = w_.library()) s->open();
        poll(true);
    }
    void poll(bool force = false) {
        library::Service* s = w_.library();
        const uint64_t serial = s ? s->serial() : 0;
        if (force || serial != serial_ || s != service_) {
            serial_ = serial;
            service_ = s;
            sources_ = s ? s->sources() : std::vector<library::Service::SourceView>();
            items_ = fileItems(sources_);
            bool found = false;
            for (const FileItem& f : items_) found = found || (!f.heading && f.path == selected_);
            if (!found) for (const FileItem& f : items_) if (!f.heading) { selected_ = f.path; break; }
            refreshEntries();
            files_.repaint();
            presets_.repaint();
            memory_.repaint();
        }
        // The Compare LED and the playing preset: the info dot and the Memory column.
        const bool compare = w_.proc_.compareLit();
        const int playing = w_.proc_.playingPreset();
        if (compare != compare_ || playing != playing_) { compare_ = compare; playing_ = playing; presets_.repaint(); memory_.repaint(); }
        memory_.refresh();
    }
    int selectedSource() const {
        for (const FileItem& f : items_) if (!f.heading && f.path == selected_) return f.source;
        return -1;
    }
    void select(const juce::String& path) { selected_ = path; refreshEntries(); files_.repaint(); presets_.repaint(); }
    bool cancelEdit() { if (!edit_.active()) return false; edit_.finish(false); return true; }
    const std::vector<FileItem>& items() const { return items_; }

    void resized() override {
        auto r = getLocalBounds().toFloat();
        auto left = r.removeFromLeft(220);
        r.removeFromLeft(ui::dp::gap);
        auto right = r.removeFromRight(300);
        r.removeFromRight(ui::dp::gap);
        auto bottom = left.removeFromBottom(ui::dp::button + 30);
        left.removeFromBottom(8);
        files_.setBounds(left.toNearestInt());
        folderLine_ = bottom.removeFromTop(26);
        const float half = (bottom.getWidth() - 8) / 2;
        import_.setBounds(bottom.removeFromLeft(half).toNearestInt());
        bottom.removeFromLeft(8);
        folder_.setBounds(bottom.toNearestInt());
        presets_.setBounds(r.toNearestInt());
        memory_.setBounds(right.toNearestInt());
    }
    void paint(juce::Graphics& g) override {
        ui::drawText(g, homePath(w_.proc_.dataFolder().getChildFile("Library")), w_.theme_.font(ui::dp::headingText), folderLine_.withTrimmedBottom(6),
                     ui::colours::muted);
        if (!w_.library())
            ui::drawText(g, "The library needs the ROMs (see SYSTEM).", w_.theme_.font(ui::dp::fileText),
                         presets_.getBounds().toFloat().removeFromTop(60), ui::colours::muted);
    }

    // ---- FILES
    class Files : public juce::Component {
    public:
        explicit Files(LibraryPage& p) : p_(p) { setComponentID("files"); }
        juce::Rectangle<float> bounds(size_t i) const {
            float y = -scroll_;
            for (size_t k = 0; k < p_.items_.size(); ++k) {
                const float h = p_.items_[k].heading ? kHeadingH : ui::dp::fileRow + 6;
                if (k == i) return {0, y, float(getWidth()), p_.items_[k].heading ? kHeadingH : ui::dp::fileRow};
                y += h;
            }
            return {};
        }
        float contentHeight() const {
            float h = 0;
            for (const FileItem& f : p_.items_) h += f.heading ? kHeadingH : ui::dp::fileRow + 6;
            return h;
        }
        void paint(juce::Graphics& g) override {
            const ui::Theme& t = p_.w_.theme_;
            const auto mouse = getMouseXYRelative().toFloat();
            for (size_t i = 0; i < p_.items_.size(); ++i) {
                const FileItem& f = p_.items_[i];
                const auto r = bounds(i);
                if (r.getBottom() < 0 || r.getY() > float(getHeight())) continue;
                if (f.heading) { drawHeading(g, t, f.label, r.withTrimmedBottom(8)); continue; }
                ui::drawRow(g, r, ui::dp::fileRadius, f.path == p_.selected_, isMouseOver() && r.contains(mouse));
                auto inner = r.reduced(10, 0);
                const auto tag = inner.removeFromLeft(4).withSizeKeepingCentre(4, 18);
                g.setColour(t.accent);
                g.fillRoundedRectangle(tag, 2);
                inner.removeFromLeft(10);
                const juce::Font m = mono(ui::dp::fileText - 1);
                const float cw = ui::textWidth(m, f.count) + 4;
                ui::drawText(g, f.count, m, inner.removeFromRight(cw), ui::colours::muted, juce::Justification::centredRight);
                inner.removeFromRight(8);
                ui::drawText(g, f.label, t.font(ui::dp::fileText), inner, ui::colours::text);
            }
        }
        void mouseDown(const juce::MouseEvent& e) override {
            for (size_t i = 0; i < p_.items_.size(); ++i) {
                const FileItem& f = p_.items_[i];
                if (f.heading || !bounds(i).contains(e.position)) continue;
                p_.select(f.path);
                if (e.mods.isPopupMenu()) p_.fileMenu(f, bounds(i).toNearestInt());
                return;
            }
        }
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& d) override {
            const float most = std::max(0.0f, contentHeight() - float(getHeight()));
            scroll_ = juce::jlimit(0.0f, most, scroll_ - d.deltaY * 240);
            repaint();
        }
        void mouseMove(const juce::MouseEvent&) override { repaint(); }
        void mouseExit(const juce::MouseEvent&) override { repaint(); }

    private:
        LibraryPage& p_;
        float scroll_ = 0;
    };

    // ---- PRESETS: two columns of 16 ("nn  name"), 32 a page; a click sends the stored message into Compare.
    class Presets : public juce::Component {
    public:
        explicit Presets(LibraryPage& p) : p_(p) { setComponentID("presets"); }
        static constexpr int kPerPage = 32, kPerColumn = 16;
        int pages() const { return std::max(1, (int(p_.entries_.size()) + kPerPage - 1) / kPerPage); }
        juce::Rectangle<float> cell(int index) const {
            const int k = index - page_ * kPerPage;
            if (k < 0 || k >= kPerPage) return {};
            const float colW = (float(getWidth()) - ui::dp::gap) / 2;
            return {float(k / kPerColumn) * (colW + ui::dp::gap), kHeadingH + float(k % kPerColumn) * ui::dp::cell, colW, ui::dp::cell - 2};
        }
        void paint(juce::Graphics& g) override {
            const ui::Theme& t = p_.w_.theme_;
            page_ = juce::jlimit(0, pages() - 1, page_);
            const auto head = getLocalBounds().toFloat().removeFromTop(kHeadingH).withTrimmedBottom(8);
            const int src = p_.selectedSource();
            const library::Service::SourceView* v = src >= 0 && size_t(src) < p_.sources_.size() ? &p_.sources_[size_t(src)] : nullptr;
            ui::drawText(g, v ? u8(v->label) : juce::String(), t.font(ui::dp::buttonText), head.withTrimmedRight(120), ui::colours::heading,
                         juce::Justification::bottomLeft);
            if (pages() > 1)
                ui::drawText(g, "page " + juce::String(page_ + 1) + " of " + juce::String(pages()), t.font(ui::dp::headingText), head,
                             ui::colours::muted, juce::Justification::bottomRight);
            if (p_.entries_.empty() && v != nullptr) {
                const juce::String what = v->busy ? "Reading" + u8(libtext::kDots) + "  " + u8(libtext::count(*v)) : juce::String("No presets");
                ui::drawText(g, what, t.font(ui::dp::cellText), {0, kHeadingH, float(getWidth()), ui::dp::cell}, ui::colours::muted);
            }
            const auto mouse = getMouseXYRelative().toFloat();
            for (int i = page_ * kPerPage; i < std::min(int(p_.entries_.size()), (page_ + 1) * kPerPage); ++i) {
                const auto& e = p_.entries_[size_t(i)];
                const auto r = cell(i);
                const bool hover = isMouseOver() && r.contains(mouse);
                if (hover) { g.setColour(ui::colours::row); g.fillRoundedRectangle(r, 6); }
                auto inner = r.reduced(8, 0);
                const bool marked = p_.compare_ && p_.sentKey_.isNotEmpty() && p_.sentKey_ == p_.entryKey(i);
                if (marked) ui::drawDot(g, {inner.getRight() - 4, inner.getCentreY()}, 3.5f, ui::colours::info);
                if (marked) inner.removeFromRight(14);
                const juce::Font m = mono(ui::dp::cellText - 1);
                ui::drawText(g, libtext::twoDigits(i + 1), m, inner.removeFromLeft(28), ui::colours::muted);
                inner.removeFromLeft(6);
                ui::drawText(g, u8(e.name), t.font(ui::dp::cellText), inner, e.accepted && e.ready ? ui::colours::text : ui::colours::muted);
            }
        }
        void mouseDown(const juce::MouseEvent& e) override {
            for (int i = page_ * kPerPage; i < std::min(int(p_.entries_.size()), (page_ + 1) * kPerPage); ++i) {
                if (!cell(i).contains(e.position)) continue;
                if (e.mods.isPopupMenu()) p_.presetMenu(i, cell(i).toNearestInt());
                else p_.send(i);
                return;
            }
        }
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& d) override {
            page_ = juce::jlimit(0, pages() - 1, page_ + (d.deltaY < 0 ? 1 : -1));
            repaint();
        }
        void mouseMove(const juce::MouseEvent&) override { repaint(); }
        void mouseExit(const juce::MouseEvent&) override { repaint(); }
        void reset() { page_ = 0; }

    private:
        LibraryPage& p_;
        int page_ = 0;
    };

    // ---- MEMORY: the synth's two banks.
    class Memory : public juce::Component {
    public:
        explicit Memory(LibraryPage& p)
            : p_(p), load_{ui::Button(p.w_.theme_, "Load" + u8(libtext::kDots)), ui::Button(p.w_.theme_, "Load" + u8(libtext::kDots))},
              export_{ui::Button(p.w_.theme_, "Save as file" + u8(libtext::kDots)), ui::Button(p.w_.theme_, "Save as file" + u8(libtext::kDots))} {
            setComponentID("memory");
            for (int b = 0; b < 2; ++b) {
                load_[b].setComponentID("load" + juce::String(b + 1));
                export_[b].setComponentID("export" + juce::String(b + 1));
                addAndMakeVisible(load_[b]);
                addAndMakeVisible(export_[b]);
                load_[b].onClick = [this, b] { p_.loadMenu(b, load_[b]); };
                export_[b].onClick = [this, b] { p_.w_.proc_.exportBank(b); };
            }
        }
        void refresh() {
            const bool idle = p_.w_.library() != nullptr && !(p_.w_.library()->bankJob().active);
            for (int b = 0; b < 2; ++b) { load_[b].setEnabled(idle); export_[b].setEnabled(p_.w_.library() != nullptr); }
        }
        juce::Rectangle<float> bankBounds(int b) const {
            return {0, kHeadingH + float(b) * (ui::dp::bankRow + ui::dp::button + 28), float(getWidth()), ui::dp::bankRow};
        }
        void resized() override {
            for (int b = 0; b < 2; ++b) {
                auto r = bankBounds(b);
                auto buttons = juce::Rectangle<float>(r.getX(), r.getBottom() + 8, r.getWidth(), ui::dp::button);
                const float half = (buttons.getWidth() - 8) / 2;
                load_[b].setBounds(buttons.removeFromLeft(half).toNearestInt());
                buttons.removeFromLeft(8);
                export_[b].setBounds(buttons.toNearestInt());
            }
        }
        void paint(juce::Graphics& g) override {
            const ui::Theme& t = p_.w_.theme_;
            drawHeading(g, t, "MEMORY" + u8(libtext::kMid) + "2 BANKS", getLocalBounds().toFloat().removeFromTop(kHeadingH).withTrimmedBottom(8));
            const int playing = p_.playing_;
            for (int b = 0; b < 2; ++b) {
                const auto r = bankBounds(b);
                ui::drawRow(g, r, ui::dp::fileRadius, false);
                auto inner = r.reduced(14, 10);
                const auto ledArea = inner.removeFromLeft(14);
                ui::drawLed(g, {ledArea.getCentreX(), r.getCentreY()}, 4.0f, libtext::inBank(playing, b));   // centred on the row
                inner.removeFromLeft(8);
                auto top = inner.removeFromTop(inner.getHeight() / 2);
                const juce::String name = u8(libtext::bankName(b));
                const float nw = ui::textWidth(t.font(ui::dp::buttonText, true), name) + 12;
                ui::drawText(g, name, t.font(ui::dp::buttonText, true), top.removeFromLeft(nw), ui::colours::text);
                ui::drawText(g, u8(libtext::bankTitle(b, str(p_.w_.proc_.lastLoaded(b)))), t.font(ui::dp::buttonText), top, ui::colours::text);
                ui::drawText(g, u8(libtext::bankDetail(b, playing)), t.font(ui::dp::fileText), inner, ui::colours::muted);
            }
        }

    private:
        LibraryPage& p_;
        ui::Button load_[2], export_[2];
    };

private:
    friend class PhyzoWindow;
    juce::String entryKey(int i) const {
        if (i < 0 || size_t(i) >= entries_.size()) return {};
        const int src = selectedSource();
        const bool singles = src >= 0 && size_t(src) < sources_.size() && sources_[size_t(src)].group == Group::Singles;
        return u8(entries_[size_t(i)].path) + (singles ? juce::String() : "#" + juce::String(i));
    }
    void refreshEntries() {
        const int src = selectedSource();
        if (src != shownSource_) { presets_.reset(); shownSource_ = src; }
        entries_ = service_ && src >= 0 ? service_->entries(src) : std::vector<library::Service::EntryView>();
    }
    // A click on a preset: its stored message into the running machine's MIDI input (Compare), nothing else.
    void send(int i) {
        if (i < 0 || size_t(i) >= entries_.size()) return;
        const auto& e = entries_[size_t(i)];
        if (!e.ready) return;
        if (!e.accepted) { w_.message(u8(libtext::notAccepted(i, e.name))); return; }
        juce::String why;
        if (!w_.proc_.sendLibraryPreset(selectedSource(), i, why)) { if (why.isNotEmpty()) w_.message(why); return; }
        sentKey_ = entryKey(i);
        presets_.repaint();
    }
    void showMenu(juce::PopupMenu& m, juce::Rectangle<int> area, juce::Component& on) {
        m.setLookAndFeel(&w_.menus_);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&on).withTargetScreenArea(on.localAreaToGlobal(area)));
    }
    void renameAt(juce::Component& host, juce::Rectangle<int> r, const juce::String& path, const juce::String& name) {
        edit_.begin(host, r, name, [this, path](const juce::String& text) {
            if (library::Service* s = w_.library()) {
                const std::string msg = s->rename(str(path), str(text));
                if (!msg.empty()) w_.message(u8(msg));
                if (selected_ == path) {           // follow the renamed file
                    const juce::File f(path);
                    selected_ = f.getSiblingFile(u8(library::safeFileName(str(text.trim()))) + f.getFileExtension()).getFullPathName();
                }
            }
        });
    }
    void fileMenu(const FileItem& f, juce::Rectangle<int> r) {
        juce::PopupMenu m;
        const juce::String path = f.path, label = f.label;
        const bool own = f.group == Group::Banks || f.group == Group::Backups;
        juce::Component::SafePointer<Files> files(&files_);
        if (own) {
            m.addItem("Rename" + u8(libtext::kDots), [this, files, r, path] {
                if (files) renameAt(*files, r.reduced(36, 6), path, juce::File(path).getFileNameWithoutExtension());
            });
            m.addItem("Delete", [this, path, label] { w_.confirmDelete(path, label); });
        }
        m.addItem("Show in Finder", [path] { juce::File(path).revealToUser(); });
        showMenu(m, r, files_);
    }
    void presetMenu(int i, juce::Rectangle<int> r) {
        const int src = selectedSource();
        if (src < 0 || size_t(src) >= sources_.size() || sources_[size_t(src)].group != Group::Singles) return;
        const juce::String path = u8(entries_[size_t(i)].path), name = u8(entries_[size_t(i)].name);
        juce::PopupMenu m;
        juce::Component::SafePointer<Presets> presets(&presets_);
        m.addItem("Rename" + u8(libtext::kDots), [this, presets, r, path, name] { if (presets) renameAt(*presets, r.withTrimmedLeft(30), path, name); });
        m.addItem("Delete", [this, path, name] { w_.confirmDelete(path, name); });
        m.addItem("Show in Finder", [path] { juce::File(path).revealToUser(); });
        showMenu(m, r, presets_);
    }
    // Load...: every bank file (factory, banks, backups), then Other file...
    void loadMenu(int bank, juce::Component& button) {
        juce::PopupMenu m;
        m.addSectionHeader("Load into " + u8(libtext::bankName(bank)));
        for (const auto& v : sources_)
            if (v.bank && !v.busy) {
                const juce::String path = u8(v.path), label = u8(v.label);
                m.addItem(label, [this, path, label, bank] { w_.confirmLoad(path, label, bank); });
            }
        m.addSeparator();
        m.addItem("Other file" + u8(libtext::kDots), [this, bank] { chooseOther(bank); });
        showMenu(m, button.getLocalBounds(), button);
    }
    void chooseOther(int bank) {
        chooser_ = std::make_unique<juce::FileChooser>("Load a bank into " + u8(libtext::bankName(bank)), w_.proc_.dataFolder().getChildFile("Library"), "*.syx");
        juce::Component::SafePointer<LibraryPage> self(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [self, bank](const juce::FileChooser& fc) {
            if (!self) return;
            const juce::File f = fc.getResult();
            if (f == juce::File()) return;
            juce::MemoryBlock data;
            const bool read = f.loadFileAsData(data);
            const std::vector<uint8_t> bytes(static_cast<const uint8_t*>(data.getData()), static_cast<const uint8_t*>(data.getData()) + data.getSize());
            if (!read || !library::Service::oneBank(bytes)) { self->w_.message(f.getFileNameWithoutExtension() + " has no bank"); return; }
            self->w_.confirmLoad(f.getFullPathName(), f.getFileNameWithoutExtension(), bank);
        });
    }

    PhyzoWindow& w_;
    Files files_;
    Presets presets_;
    Memory memory_;
    ui::Button import_, folder_;
    InlineEdit edit_;
    std::unique_ptr<juce::FileChooser> chooser_;
    library::Service* service_ = nullptr;
    uint64_t serial_ = ~uint64_t(0);
    std::vector<library::Service::SourceView> sources_;
    std::vector<FileItem> items_;
    std::vector<library::Service::EntryView> entries_;
    juce::String selected_, sentKey_;
    int shownSource_ = -2;
    bool compare_ = false;
    int playing_ = -1;
    juce::Rectangle<float> folderLine_;
};

// ================================================================== the window

PhyzoWindow::PhyzoWindow(PhyzoEditor& editor, PhyzoProcessor& proc, const ui::Theme& theme, ui::MenuLookAndFeel& menus)
    : editor_(editor), proc_(proc), theme_(theme), menus_(menus), frame_(theme, "PHYZO", {"SYSTEM", "LIBRARY", "SKIN", "MIDI"}) {
    setComponentID("phyzoWindow");
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(false);          // a click on the backdrop leaves an open box its focus (cancelled first)
    addAndMakeVisible(frame_);
    system_ = std::make_unique<SystemPage>(*this);
    skin_ = std::make_unique<SkinPage>(*this);
    midi_ = std::make_unique<MidiPage>(*this);
    library_ = std::make_unique<LibraryPage>(*this);
    for (juce::Component* p : {static_cast<juce::Component*>(system_.get()), static_cast<juce::Component*>(library_.get()),
                               static_cast<juce::Component*>(skin_.get()), static_cast<juce::Component*>(midi_.get())})
        frame_.addChildComponent(p);
    frame_.tabs().onSelect = [this](int t) { showTab(t); };
    frame_.closeButton().onClick = [this] { editor_.closeWindow(); };
    frame_.onBodyResized = [this] { resized(); };
    frame_.footer().onFieldReturn = [this] { if (ask_ == Ask::SaveName) doSave(); };
    frame_.footer().onFieldEscape = [this] { ask(Ask::None); };
    library::Service* s = library();
    for (library::Service::SaveDraft d; s && s->takeSaveDraft(d);) {}   // drafts from before this window: old news
    showTab(kSystem);
}

void PhyzoWindow::visibilityChanged() {
    if (isVisible()) startTimerHz(10);
    else stopTimer();
}

PhyzoWindow::~PhyzoWindow() { stopTimer(); }

library::Service* PhyzoWindow::library() { return proc_.libraryService(); }

juce::Component* PhyzoWindow::page(int tab) {
    switch (tab) {
        case kSystem: return system_.get();
        case kLibrary: return library_.get();
        case kSkin: return skin_.get();
        case kMidi: return midi_.get();
    }
    return nullptr;
}

void PhyzoWindow::showTab(int tab) {
    tab_ = juce::jlimit(0, 3, tab);
    frame_.tabs().setSelected(tab_);
    for (int t = 0; t < 4; ++t) page(t)->setVisible(t == tab_);
    refresh();
    if (tab_ == kLibrary) library_->opened();        // rescan when the tab opens
    phyzodiag::log("window: " + frame_.tabs().tabs()[tab_]);
}

void PhyzoWindow::refresh() {
    system_->refresh();
    skin_->refresh();
    midi_->refresh();
    library_->poll(true);
    showFooter();
    repaint();
}

void PhyzoWindow::place(juce::Rectangle<int> area, float pointsPerDp) {
    pointsPerDp_ = pointsPerDp > 0 ? pointsPerDp : 1.0f;
    setBounds(area);
    placement_ = ui::placeCentred(getLocalBounds(), pointsPerDp_, {kLayoutW, kLayoutH}, kDrawnWidth, ui::dp::inset);
    frame_.setTransform(juce::AffineTransform::scale(placement_.scale).translated(placement_.bounds.getX(), placement_.bounds.getY()));
    menus_.setScale(placement_.scale);
    repaint();
}

void PhyzoWindow::resized() {
    frame_.setBounds(0, 0, int(kLayoutW), int(kLayoutH));
    const auto body = frame_.bodyBounds();
    for (int t = 0; t < 4; ++t) page(t)->setBounds(body);
}

void PhyzoWindow::paint(juce::Graphics& g) {
    ui::drawBackdrop(g, getLocalBounds().toFloat(), placement_.bounds, pointsPerDp_ * placement_.fit);
}

// The plugin area's bottom-right corner stays with the skin (its resize grip).
bool PhyzoWindow::hitTest(int x, int y) {
    return !(x >= getWidth() - kGripSize && y >= getHeight() - kGripSize);
}

// A click on the backdrop: as Esc (an open box or question cancelled first, then the window closed).
void PhyzoWindow::mouseDown(const juce::MouseEvent&) {
    if (cancelEditing()) grabKeyboardFocus();
    else editor_.closeWindow();
}

bool PhyzoWindow::keyPressed(const juce::KeyPress& k) {
    if (k.getKeyCode() == juce::KeyPress::F5Key) { editor_.reloadSkin(); return true; }
    if (k == juce::KeyPress::escapeKey) {
        if (!cancelEditing()) editor_.closeWindow();
        return true;                                 // the window was open: Esc is ours
    }
    return false;
}

void PhyzoWindow::renameForTest(const juce::String& path, const juce::String& name) {
    library_->renameAt(*library_, {10, 10, 200, 30}, path, name);
}
bool PhyzoWindow::renamingForTest() const { return library_->edit_.active(); }

bool PhyzoWindow::cancelEditing() {
    if (library_->cancelEdit()) return true;
    if (ask_ != Ask::None) { ask(Ask::None); return true; }
    return false;
}

// Shown at once unless the footer asks something (then after it, while the message is still recent).
void PhyzoWindow::message(const juce::String& text) {
    message_ = text;
    messageAt_ = juce::Time::getMillisecondCounter();
    shownFooter_ = "\n";                            // shown again even if it is the same text
    if (ask_ == Ask::None) showFooter();
}

// ---- footer: a question (Save name, Delete, Load) wins; then a running bank job; then the latest message (8 s);
// then progress (making the factory files, reading a file).
void PhyzoWindow::showFooter() {
    ui::Footer& f = frame_.footer();
    library::Service* s = library();
    switch (ask_) {
        case Ask::SaveWait:
            f.show({{"Reading the current preset" + u8(libtext::kDots), false, true}}, {{"Cancel", [this] { ask(Ask::None); }}});
            shownFooter_.clear();
            return;
        case Ask::SaveName:
            f.show({{u8(libtext::savePrefix(draft_.preset)) + " ", false, false}, {u8(draft_.osName).trim(), true, false}, {u8(libtext::kSaveSuffix), false, true}},
                   {{"Save", [this] { doSave(); }, true}, {"Cancel", [this] { ask(Ask::None); }}});
            if (auto* field = f.field()) { field->grabKeyboardFocus(); field->selectAll(); }
            shownFooter_.clear();
            return;
        case Ask::Delete:
            f.show({{u8(libtext::deleteConfirm(str(askName_))), false, false}},
                   {{"Delete", [this] {
                         const juce::String path = askPath_;
                         ask_ = Ask::None;
                         if (library::Service* svc = library()) message(u8(svc->remove(str(path))));
                     }, true},
                    {"Cancel", [this] { ask(Ask::None); }}});
            shownFooter_.clear();
            return;
        case Ask::Load:
            f.show({{u8(libtext::loadConfirm(str(askName_), askBank_, proc_.playingPreset())), false, false}},
                   {{"Load", [this] {
                         ask_ = Ask::None;
                         if (library::Service* svc = library()) svc->loadBank(str(askPath_), askBank_);
                         showFooter();
                     }, true},
                    {"Cancel", [this] { ask(Ask::None); }}});
            shownFooter_.clear();
            return;
        case Ask::None: break;
    }
    if (s && s->bankJob().active) {
        const library::Service::BankJob j = s->bankJob();
        const juce::String text = u8(libtext::bankJob(j));
        const bool cancellable = j.phase == library::Service::BankJob::Phase::Queued || j.phase == library::Service::BankJob::Phase::Backup;
        if (text + (cancellable ? "|c" : "") != shownFooter_) {
            shownFooter_ = text + (cancellable ? "|c" : "");
            std::vector<ui::Footer::Action> actions;
            if (cancellable) actions.push_back({"Cancel", [this] { if (library::Service* svc = library()) svc->cancelBankBackup(); }});
            f.show({{text, false, false}}, actions);
        }
        bankJobShown_ = true;
        return;
    }
    juce::String text;
    bool muted = false;
    if (message_.isNotEmpty() && juce::Time::getMillisecondCounter() - messageAt_ < juce::uint32(kMessageMs)) text = message_;
    else if (s) {
        for (const auto& v : s->sources())           // progress: the factory files first, then any file being read
            if (v.busy && v.group != library::Source::Group::Singles) {
                text = (v.group == library::Source::Group::Factory ? "Making " : "Reading ") + u8(v.label) + u8(libtext::kDots) + "  " + u8(libtext::count(v));
                muted = true;
                break;
            }
    }
    if (text + (muted ? "|m" : "") == shownFooter_ && !bankJobShown_) return;
    bankJobShown_ = false;
    shownFooter_ = text + (muted ? "|m" : "");
    f.show(text.isEmpty() ? std::vector<ui::Footer::Part>() : std::vector<ui::Footer::Part>{{text, false, muted}});
}

void PhyzoWindow::startSave() {
    if (tab_ != kLibrary) showTab(kLibrary);
    library::Service* s = library();
    if (!s) { message("NOT SAVED  THE SYNTH IS NOT RUNNING"); return; }
    if (proc_.compareLit()) { ask_ = Ask::None; message(libtext::kUnsaved); return; }   // and nothing more
    for (library::Service::SaveDraft d; s->takeSaveDraft(d);) {}
    s->prepareSave();
    ask(Ask::SaveWait);
}

void PhyzoWindow::doSave() {
    library::Service* s = library();
    juce::TextEditor* field = frame_.footer().field();
    if (!s || !field) { ask(Ask::None); return; }
    s->saveAs(draft_, str(field->getText()));
    ask_ = Ask::None;
    message_.clear();
    showFooter();
}

void PhyzoWindow::confirmDelete(const juce::String& path, const juce::String& name) {
    askPath_ = path;
    askName_ = name;
    ask(Ask::Delete);
}

void PhyzoWindow::confirmLoad(const juce::String& path, const juce::String& file, int bank) {
    askPath_ = path;
    askName_ = file;
    askBank_ = bank;
    ask(Ask::Load);
}

void PhyzoWindow::timerCallback() {
    for (juce::String m; proc_.takeWindowMessage(m);) message(m);
    if (library::Service* s = library()) {
        library::Service::SaveDraft d;
        if (ask_ == Ask::SaveWait && s->takeSaveDraft(d)) {
            ask_ = Ask::None;
            if (d.compare) message(libtext::kUnsaved);
            else if (!d.error.empty() || d.message.empty()) message(u8(d.error.empty() ? std::string("NOT SAVED") : d.error));
            else { draft_ = d; ask(Ask::SaveName); }
        }
    }
    if (tab_ == kLibrary) library_->poll();
    if (tab_ == kSkin) skin_->refresh();
    if (tab_ == kMidi) midi_->refresh();
    if (tab_ == kSystem) system_->refresh();
    if (ask_ == Ask::None) showFooter();
}
