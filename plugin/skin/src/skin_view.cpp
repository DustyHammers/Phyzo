#include "skin_view.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include "skin_elements.h"
#include "skin_lua.h"
#include "skin_system.h"
#include <RmlUi/Lua/IncludeLua.h>

namespace fs = std::filesystem;

namespace skin {
namespace {

constexpr double kRawPerDp = 1023.0 / 256.0;        // knob drag: full range over 256 dp
constexpr double kFineFactor = 0.1;                 // with Shift
constexpr size_t kLogLines = 300;

// Draws nothing (tests, and a view whose renderer is not ready).
class NullRender : public Rml::RenderInterface {
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dims, const Rml::String&) override { dims = {1, 1}; return 1; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};

int ledStateOf(LedMode m) { return m == LedMode::Beat ? 2 : int(m); }

template <class T> T* findUp(Rml::Element* e) {
    for (; e; e = e->GetParentNode())
        if (auto* t = dynamic_cast<T*>(e)) return t;
    return nullptr;
}

template <class F> void guarded(SkinView* v, F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        v->addLog(std::string("error: ") + e.what());
    } catch (...) {
        v->addLog("error: unexpected failure in the skin");
    }
}

}  // namespace

SkinView::SkinView(PanelPort& port, PluginInfo info, Rml::RenderInterface* render)
    : port_(port), info_(std::move(info)), render_(render) {
    if (!render_) { nullRender_ = std::make_unique<NullRender>(); render_ = nullRender_.get(); }
    char name[64];
    std::snprintf(name, sizeof name, "phyzo-%p", static_cast<void*>(this));
    contextName_ = name;
    guarded(this, [&] {
        ViewScope s(this);
        systemOk_ = acquireSystem(systemError_);
        if (!systemOk_) return;
        context_ = Rml::CreateContext(contextName_, {widthPx_, heightPx_}, render_);
        if (!context_) { systemError_ = "The skin view could not be created."; return; }
    });
}

SkinView::~SkinView() {
    guarded(this, [&] {
        ViewScope s(this);
        unload();
        if (context_) {
            contextClosing(context_);
            Rml::RemoveContext(contextName_);
            context_ = nullptr;
            Rml::ReleaseRenderManagers();
        }
        if (systemOk_) releaseSystem();
    });
}

std::vector<std::string> SkinView::log() const {
    std::lock_guard<std::recursive_mutex> lk(skinLock());
    return log_;
}

void SkinView::addLog(const std::string& line) {
    std::lock_guard<std::recursive_mutex> lk(skinLock());
    log_.push_back(line);
    if (log_.size() > kLogLines) log_.erase(log_.begin(), log_.begin() + long(log_.size() - kLogLines));
    if (onLog) onLog(onLogUser, line);
}

// ------------------------------------------------------------------ loading

bool SkinView::load(const std::string& folder, const std::string& name, std::string& error) {
    bool ok = false;
    guarded(this, [&] {
        ViewScope s(this);
        unload();
        if (!systemOk_ || !context_) { error = systemError_; return; }
        const fs::path dir = fs::u8path(folder).lexically_normal();
        const fs::path file = dir / fs::u8path(name + ".rml");
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) { error = "The file " + name + ".rml is missing from " + dir.u8string() + "."; return; }
        folder_ = dir.u8string();
        allowFolder(folder_);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            std::string ext = entry.path().extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
            if (ext == ".ttf" || ext == ".otf") loadFont(entry.path().u8string());
        }
        resetEnvironment(*this);
        installLuaApi(*this);
        knobSent_.fill(-1);
        down_.clear();
        for (int c = 0; c < 256; ++c) leds[size_t(c)] = port_.led(c);
        beatSeen_.fill(-1e9);
        segsKnown_ = false;
        segs_ = port_.segments();
        size_t logBefore = log_.size();
        document_ = context_->LoadDocument(file.u8string());
        if (document_) {
            std::string bad;                             // a document RmlUi could only partly read is a failed load
            if (document_->GetNumChildren() == 0) bad = name + ".rml has no content";
            for (size_t i = logBefore; i < log_.size() && bad.empty(); ++i)
                if (log_[i].find("parse error") != std::string::npos) bad = log_[i].substr(log_[i].find(": ") + 2);
            if (!bad.empty()) {
                addLog("error: " + bad);
                document_->Close();
                document_ = nullptr;
                logBefore = log_.size() - 1;
            }
        }
        if (!document_) {
            for (size_t i = logBefore; i < log_.size() && i < logBefore + 3; ++i) {
                std::string l = log_[i];
                for (const char* p : {"error: ", "warning: "}) if (l.rfind(p, 0) == 0) l = l.substr(std::strlen(p));
                error += (error.empty() ? "" : "; ") + l;
            }
            if (error.empty()) error = name + ".rml could not be read";
            unload();
            return;
        }
        document_->Show();
        dispatchShow(document_);
        // The editor unit: a second document below the body, if the skin has one.
        const fs::path efile = dir / fs::u8path(name + "-editor.rml");
        if (fs::is_regular_file(efile, ec)) {
            const size_t before = log_.size();
            editorDoc_ = context_->LoadDocument(efile.u8string());
            bool bad = editorDoc_ && editorDoc_->GetNumChildren() == 0;
            for (size_t i = before; i < log_.size() && !bad; ++i) bad = log_[i].find("parse error") != std::string::npos;
            if (bad && editorDoc_) { editorDoc_->Close(); editorDoc_ = nullptr; }
            if (!editorDoc_) {
                addLog("warning: " + name + "-editor.rml could not be read; the skin has no editor unit");
            } else {
                editorDoc_->SetProperty("position", "absolute");
                editorDoc_->SetProperty("left", "0dp");
                editorDoc_->SetProperty("top", std::to_string(int(kBodyHeightDp)) + "dp");
                editorApplied_ = false;
                if (editorVisible_) { editorDoc_->Show(); dispatchShow(editorDoc_); }
            }
        }
        nextWake_ = 0;                                   // draw the new document at once
        ok = true;
    });
    if (!ok && error.empty()) error = name + ".rml could not be read";
    return ok;
}

void SkinView::dispatchShow(Rml::Element* e) {
    for (int i = 0; i < e->GetNumChildren(); ++i) {
        Rml::Element* c = e->GetChild(i);
        if (c->HasAttribute("onshow")) c->DispatchEvent(Rml::EventId::Show, Rml::Dictionary());
        dispatchShow(c);
    }
}

void SkinView::unload() {
    drag_ = nullptr;
    if (mousePressed_) { mousePressed_->mouseRelease(); mousePressed_ = nullptr; }
    for (int raw : std::set<int>(held_)) setHeld(raw, false);
    if (editorDoc_) { editorDoc_->Close(); editorDoc_ = nullptr; }
    if (document_) {
        document_->Close();
        document_ = nullptr;
        if (context_) context_->Update();                 // closing is completed in the next update
    }
    dropLuaCallbacks(*this);
    dropEnvironment(*this);
}

void SkinView::setViewport(int widthPx, int heightPx, float dpRatio) {
    widthPx_ = std::max(1, widthPx); heightPx_ = std::max(1, heightPx); dpRatio_ = dpRatio > 0 ? dpRatio : 1;
    guarded(this, [&] {
        ViewScope s(this);
        if (!context_) return;
        context_->SetDimensions({widthPx_, heightPx_});
        context_->SetDensityIndependentPixelRatio(dpRatio_);
    });
}

// ------------------------------------------------------------------ per frame

void SkinView::update(double now) {
    guarded(this, [&] {
        ViewScope s(this);
        now_ = now;
        ++frames_;
        changed_ = false;
        const uint64_t lua0 = luaCalls_.load();
        if (!context_) return;
        if (document_) {
            // The synth's panel state as it is now (the engine keeps only the latest state: any number of panel
            // messages since the last frame arrive here as one change).
            pollLeds();
            pollDisplay();
            if (applyLedClasses()) changed_ = true;
            if (applyEditor()) changed_ = true;
            // Frame events only for a handler with code in it: an absent, empty or blank onframe asks for no frames.
            const std::string onframe = document_->GetAttribute<Rml::String>("onframe", "");
            hasFrameHandler_ = onframe.find_first_not_of(" \t\r\n") != std::string::npos;
            if (hasFrameHandler_ && now_ - lastFrameEvent_ >= 1.0 / kFrameEventHz - 0.001) {
                lastFrameEvent_ = now_;
                if (document_) document_->DispatchEvent("frame", Rml::Dictionary());
            }
        }
        context_->Update();
        if (luaCalls_.load() != lua0) changed_ = true;
        if (changed_) ++changedFrames_;
        computeNextWake();
    });
}

SkinView::Stats SkinView::stats() const { return {frames_.load(), changedFrames_.load(), luaCalls_.load()}; }

void SkinView::computeNextWake() {
    double wake = std::numeric_limits<double>::infinity();
    Wake source = Wake::None;
    auto earlier = [&](double t, Wake w) { if (t < wake) { wake = t; source = w; } };
    if (document_) {
        auto ledWake = [&](int code) {
            if (code < 0 || code > 255) return;
            const LedReport& r = leds[size_t(code)];
            if (r.mode == LedMode::Flash) {
                const double half = 0.5 / kFlashHz;
                earlier((std::floor(now_ / half) + 1) * half, Wake::Flash);
            } else if (r.mode == LedMode::Beat) {
                double litMs = kBeatLitMs;
                if (r.beatIntervalMs > 0) litMs = std::min(litMs, double(r.beatIntervalMs) / 2);
                const double off = beatSeen_[size_t(code)] + litMs / 1000.0;
                if (off > now_) earlier(off, Wake::Beat);
            }
        };
        for (LedElement* e : leds_) if (!e->audioClip()) ledWake(e->code());
        for (PButtonElement* b : buttons_) ledWake(b->ledCode());
        if (hasFrameHandler_) earlier(lastFrameEvent_ + 1.0 / kFrameEventHz, Wake::Frame);
        if (editorFocus_) {                              // the focused value blinks at the flash rate
            const double half = 0.5 / kFlashHz;
            earlier((std::floor(now_ / half) + 1) * half, Wake::Flash);
        }
    }
    if (context_) {
        const double d = context_->GetNextUpdateDelay();         // animations, transitions (0: next frame)
        if (d >= 0 && d < 1e9) earlier(now_ + d, Wake::Animation);
    }
    nextWake_ = wake;
    wakeSource_ = source;
}

void SkinView::render() {
    guarded(this, [&] {
        ViewScope s(this);
        if (context_) context_->Render();
    });
}

void SkinView::pollLeds() {
    for (int c = 0; c < 256; ++c) {
        const LedReport r = port_.led(c);
        LedReport& old = leds[size_t(c)];
        const bool beat = r.beats != old.beats;
        const bool changed = ledStateOf(r.mode) != ledStateOf(old.mode);
        old = r;
        if (beat) {
            beatSeen_[size_t(c)] = now_;
            auto it = onBeatFns.find(c);
            if (it != onBeatFns.end()) callLua(it->second, {});
        }
        if (changed) {
            auto it = onLedFns.find(c);
            if (it != onLedFns.end()) callLua(it->second, {ledStateOf(r.mode)});
        }
    }
}

bool SkinView::ledLit(int code) const {
    if (code < 0 || code > 255) return false;
    const LedReport& r = leds[size_t(code)];
    switch (r.mode) {
    case LedMode::Off: return false;
    case LedMode::On: return true;
    case LedMode::Flash: {
        const double period = 1.0 / kFlashHz;
        return std::fmod(now_, period) < period / 2;
    }
    case LedMode::Beat: {
        double litMs = kBeatLitMs;
        if (r.beatIntervalMs > 0) litMs = std::min(litMs, double(r.beatIntervalMs) / 2);
        return now_ - beatSeen_[size_t(code)] < litMs / 1000.0;
    }
    }
    return false;
}

int SkinView::ledState(int code) { return code >= 0 && code < 256 ? ledStateOf(leds[size_t(code)].mode) : 0; }

// Sets class `lit` where it changed; true if any did.
bool SkinView::applyLedClasses() {
    const bool clip = port_.audioClip();
    bool any = false;
    auto set = [&](Rml::Element* e, bool lit) { if (e->IsClassSet("lit") != lit) { e->SetClass("lit", lit); any = true; } };
    for (LedElement* e : leds_) set(e, e->audioClip() ? clip : ledLit(e->code()));
    for (PButtonElement* b : buttons_)
        if (b->ledCode() >= 0) set(b, ledLit(b->ledCode()));
    return any;
}

void SkinView::pollDisplay() {
    const std::array<uint8_t, 4> s = port_.segments();
    if (segsKnown_ && s == segs_) return;
    segs_ = s;
    segsKnown_ = true;
    changed_ = true;
    for (VfDigitElement* d : digits_) d->setByte(segs_[size_t(d->pos())]);
    callLua(onDisplayFns, {segs_[0], segs_[1], segs_[2], segs_[3]});
}

// ------------------------------------------------------------------ synth actions

void SkinView::knobSent(int cc, int raw) {
    if (cc >= 0 && cc < 26) {                     // a value already sent for this cc: nothing to send or report
        if (knobSent_[size_t(cc)] == raw) return;
        knobSent_[size_t(cc)] = raw;
    }
    port_.sendControl(cc, raw);
    auto it = onKnobFns.find(cc);
    if (it != onKnobFns.end()) callLua(it->second, {raw});
}

void SkinView::setKnob(int cc, int raw) {
    raw = std::clamp(raw, 0, 1023);
    for (KnobElement* k : knobs_)
        if (k->cc() == cc && k->IsVisible(true)) k->show(raw);
    knobSent(cc, raw);
}

void SkinView::button(int raw, bool down) {
    if ((down_.count(raw) != 0) == down) return;  // only press and release edges
    if (down) down_.insert(raw); else down_.erase(raw);
    port_.sendButton(raw, down);
    auto it = onButtonFns.find(raw);
    if (it != onButtonFns.end()) callLuaBool(it->second, down);
}

void SkinView::setHeld(int raw, bool held) {
    if (held == isHeld(raw)) return;
    if (held) held_.insert(raw); else held_.erase(raw);
    for (PButtonElement* b : buttons_)
        if (b->raw() == raw) b->SetClass("held", held);
    button(raw, held);
}

void SkinView::beginKnobDrag(KnobElement* k) {
    drag_ = k;
    dragStartY_ = mouseY_;
    dragStartRaw_ = k->raw();
}

// ------------------------------------------------------------------ input

void SkinView::mouseMove(int x, int y, int mods) {
    guarded(this, [&] {
        ViewScope s(this);
        mouseX_ = x; mouseY_ = y;
        if (drag_) {
            const bool fine = (mods & ModShift) != 0;
            if (fine != dragFine_) { dragFine_ = fine; dragStartY_ = y; dragStartRaw_ = drag_->raw(); }   // no jump
            const double perPx = kRawPerDp * (fine ? kFineFactor : 1.0) / double(dpRatio_);
            drag_->move(dragStartRaw_ + int(std::lround(double(dragStartY_ - y) * perPx)));
        }
        if (context_) context_->ProcessMouseMove(x, y, mods);
    });
}

void SkinView::mouseDown(int button, int mods) {
    guarded(this, [&] {
        ViewScope s(this);
        if (!context_) return;
        if (button == 0 && document_) {
            Rml::Element* hover = context_->GetHoverElement();
            if (KnobElement* k = findUp<KnobElement>(hover)) {
                dragFine_ = (mods & ModShift) != 0;
                beginKnobDrag(k);
            } else if (PButtonElement* b = findUp<PButtonElement>(hover)) {
                b->mouseDown((mods & ModAlt) != 0);
            } else if (LegendElement* l = findUp<LegendElement>(hover)) {
                if (l->clickable() && port_.editor()) port_.editor()->editorCommand(l->action(), l->value());
            }
        }
        context_->ProcessMouseButtonDown(button, mods);
    });
}

void SkinView::mouseUp(int button, int mods) {
    guarded(this, [&] {
        ViewScope s(this);
        if (context_) context_->ProcessMouseButtonUp(button, mods);
        if (button == 0) {
            drag_ = nullptr;
            if (mousePressed_) { PButtonElement* b = mousePressed_; mousePressed_ = nullptr; b->mouseRelease(); }
        }
    });
}

void SkinView::mouseWheel(float deltaY, int mods) {
    guarded(this, [&] {
        ViewScope s(this);
        if (!context_) return;
        if (document_)
            if (KnobElement* k = findUp<KnobElement>(context_->GetHoverElement())) {
                k->wheel(-deltaY, (mods & ModShift) != 0);
                return;
            }
        context_->ProcessMouseWheel(deltaY, mods);
    });
}

void SkinView::mouseLeave() {
    guarded(this, [&] {
        ViewScope s(this);
        if (context_) context_->ProcessMouseLeave();
    });
}

void SkinView::releaseHeldButtons() {
    guarded(this, [&] {
        ViewScope s(this);
        for (int raw : std::set<int>(held_)) setHeld(raw, false);
    });
}

void SkinView::keyDown(int rmlKey, int mods) {
    guarded(this, [&] {
        ViewScope s(this);
        if (context_) context_->ProcessKeyDown(Rml::Input::KeyIdentifier(rmlKey), mods);
    });
}

void SkinView::keyUp(int rmlKey, int mods) {
    guarded(this, [&] {
        ViewScope s(this);
        if (context_) context_->ProcessKeyUp(Rml::Input::KeyIdentifier(rmlKey), mods);
    });
}

void SkinView::setDebuggerVisible(bool on) {
    guarded(this, [&] {
        ViewScope s(this);
        if (!context_) return;
        showDebugger(context_, on);
        debugger_ = on;
    });
}

std::string SkinView::evalForTest(const std::string& code) {
    std::string out;
    guarded(this, [&] {
        ViewScope s(this);
        lua_State* L = luaState();
        if (!L) { out = "error: no Lua"; return; }
        const int base = lua_gettop(L);
        if (luaL_loadstring(L, ("return " + code).c_str()) != LUA_OK) {
            lua_settop(L, base);
            if (luaL_loadstring(L, code.c_str()) != LUA_OK) { out = std::string("error: ") + lua_tostring(L, -1); lua_settop(L, base); return; }
        }
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) out = std::string("error: ") + lua_tostring(L, -1);
        else { out = luaL_tolstring(L, -1, nullptr); }
        lua_settop(L, base);
    });
    return out;
}

// ------------------------------------------------------------------ element registry

void SkinView::registerElement(KnobElement* e) { knobs_.insert(e); }
void SkinView::registerElement(PButtonElement* e) { buttons_.insert(e); }
void SkinView::registerElement(LedElement* e) { leds_.insert(e); }
void SkinView::registerElement(VfDigitElement* e) { digits_.insert(e); }
void SkinView::unregisterElement(KnobElement* e) { knobs_.erase(e); if (drag_ == e) drag_ = nullptr; }
void SkinView::unregisterElement(PButtonElement* e) { buttons_.erase(e); if (mousePressed_ == e) mousePressed_ = nullptr; }
void SkinView::unregisterElement(LedElement* e) { leds_.erase(e); }
void SkinView::unregisterElement(VfDigitElement* e) { digits_.erase(e); }

// ------------------------------------------------------------------ editor unit

void legendState(const EditorState& s, const std::string& action, const std::string& value, bool& lit, bool& avail) {
    lit = avail = false;
    if (action == "area") { avail = s.areas.count(value) != 0; lit = avail && s.area == value; }
    else if (action == "section") { avail = s.sections.count(value) != 0; lit = avail && s.section == value; }
    else if (action == "target" && value.size() == 2) {
        const int n = value[1] - '1';
        if (value[0] == 'S') { avail = s.soundTargets && n >= 0 && n < 4; lit = avail && s.sound == n; }
        if (value[0] == 'O') { avail = s.oscTargets && n >= 0 && n < 2; lit = avail && s.osc == n; }
    } else if (action == "page") {
        const int n = std::atoi(value.c_str());
        avail = n >= 1 && n <= s.pages;
        lit = avail && s.page == n;
    } else if (action == "page_prev") { lit = avail = s.prev; }
    else if (action == "page_next") { lit = avail = s.next; }
    else if (action == "toggle_follow") { avail = true; lit = s.follow; }
    else if (action == "indicator") { lit = (value == "DATA" && s.data) || (value == "EDITED" && s.edited); }
    else if (action == "label") { lit = s.pages > 0; }
}

void SkinView::setEditorVisible(bool on) {
    guarded(this, [&] {
        ViewScope s(this);
        editorVisible_ = on;
        editorApplied_ = false;
        if (!editorDoc_) return;
        if (on) { editorDoc_->Show(); dispatchShow(editorDoc_); }
        else editorDoc_->Hide();
        nextWake_ = 0;
    });
}

bool SkinView::applyEditor() {
    EditorPort* ep = port_.editor();
    if (!editorDoc_ || !editorVisible_ || !ep) { editorFocus_ = false; return false; }
    const uint32_t serial = ep->editorSerial();
    auto blinkPhase = [&] { return (int(std::floor(now_ * 2 * kFlashHz)) & 1) != 0; };
    if (editorApplied_ && serial == editorSerial_ && (editorFocus_ && blinkPhase()) == editorBlinkOff_) return false;
    EditorState st;
    ep->editorState(st);
    editorFocus_ = st.focusRow >= 0 && st.focusLen > 0;
    const bool blinkOff = editorFocus_ && blinkPhase();
    const bool changedState = !editorApplied_ || serial != editorSerial_;
    editorApplied_ = true;
    editorSerial_ = serial;
    editorBlinkOff_ = blinkOff;
    for (VfdRowElement* r : rows_) {
        const int row = r->row();
        r->setRow(st.rows[size_t(row)], st.underline[size_t(row)], st.dot[size_t(row)], st.focusRow == row ? st.focusCol : 0,
                  st.focusRow == row ? st.focusLen : 0, blinkOff);
    }
    for (LegendElement* l : legends_) {
        bool lit, avail;
        legendState(st, l->action(), l->value(), lit, avail);
        if (l->IsClassSet("lit") != lit) l->SetClass("lit", lit);
        if (l->IsClassSet("avail") != avail) l->SetClass("avail", avail);
    }
    if (changedState) callLua(onEditorFns, {});
    return true;
}

void SkinView::registerElement(VfdRowElement* e) { rows_.insert(e); editorApplied_ = false; }
void SkinView::registerElement(LegendElement* e) { legends_.insert(e); editorApplied_ = false; }
void SkinView::unregisterElement(VfdRowElement* e) { rows_.erase(e); }
void SkinView::unregisterElement(LegendElement* e) { legends_.erase(e); }

}  // namespace skin
