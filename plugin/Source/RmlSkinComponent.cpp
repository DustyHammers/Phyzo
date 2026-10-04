#include "RmlSkinComponent.h"
#include <RmlUi/Core/Input.h>
#include "gl/RmlGLRenderer.h"
#include "skin_system.h"
#include "skin_view.h"

#if JUCE_MAC
bool phyzoWindowVisible(void* nsView);           // MacWindow.mm
#endif

namespace {
constexpr float kWheelNotch = 0.14f;      // JUCE wheel delta of one mouse-wheel notch (about)

// JUCE image -> premultiplied RGBA bytes, top row first.
bool decodeImage(const std::vector<uint8_t>& file, std::vector<uint8_t>& rgba, int& w, int& h) {
    juce::Image img = juce::ImageFileFormat::loadFrom(file.data(), file.size());
    if (!img.isValid()) return false;
    img = img.convertedToFormat(juce::Image::ARGB);
    w = img.getWidth(); h = img.getHeight();
    rgba.resize(size_t(w) * size_t(h) * 4);
    const juce::Image::BitmapData bd(img, juce::Image::BitmapData::readOnly);
    uint8_t* out = rgba.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const auto* p = reinterpret_cast<const juce::PixelARGB*>(bd.getPixelPointer(x, y));   // premultiplied
            *out++ = p->getRed(); *out++ = p->getGreen(); *out++ = p->getBlue(); *out++ = p->getAlpha();
        }
    return true;
}
}  // namespace

RmlSkinComponent::RmlSkinComponent(PhyzoProcessor& p) : proc_(p), logFile_(p.skinsFolder().getSiblingFile("skin-log.txt")) {
    setOpaque(false);
    setWantsKeyboardFocus(true);
    juce::OpenGLPixelFormat pf;
    pf.stencilBufferBits = 8;
    gl_.setPixelFormat(pf);
    gl_.setOpenGLVersionRequired(juce::OpenGLContext::openGL3_2);
    gl_.setRenderer(this);
    gl_.setComponentPaintingEnabled(true);      // the message banner is painted over the skin
    gl_.setContinuousRepainting(false);            // frames only when something changed (timerCallback)
    gl_.attachTo(*this);
    startTimerHz(60);
}

RmlSkinComponent::~RmlSkinComponent() {
    stopTimer();
    gl_.detach();                               // closes the context: openGLContextClosing runs on the GL thread
}

void RmlSkinComponent::post(std::function<void()> f) {
    {
        std::lock_guard<std::mutex> lk(queueMtx_);
        queue_.push_back(std::move(f));
    }
    frameWanted_ = true;                           // input, loading, debugger: draw the result
}

void RmlSkinComponent::loadSkin(const juce::File& folder, const juce::String& name) {
    const std::string f = folder.getFullPathName().toStdString(), n = name.toStdString();
    post([this, f, n] { folder_ = f; name_ = n; startLoad(); });
}

void RmlSkinComponent::reload() { post([this] { startLoad(); }); }

void RmlSkinComponent::setDebugger(bool on) {
    debugger_ = on;
    post([this, on] { if (view_) view_->setDebuggerVisible(on); });
}

void RmlSkinComponent::showMessage(const juce::String& text, int seconds) {
    message_ = text;
    messageUntil_ = juce::Time::getMillisecondCounter() + juce::uint32(seconds * 1000);
    repaint();
}

void RmlSkinComponent::setPersistentMessage(const juce::String& text) {
    if (text == persistent_) return;
    persistent_ = text;
    repaint();
}

bool RmlSkinComponent::visibleOnScreen() const {
    if (!isShowing()) return false;
    auto* peer = getPeer();
    if (peer == nullptr || peer->isMinimised()) return false;
#if JUCE_MAC
    return phyzoWindowVisible(peer->getNativeHandle());
#else
    return true;
#endif
}

// 60 times a second on the message thread: a frame only when the panel changed, input arrived or the skin's next
// wake time came, and never while the window cannot be seen.
void RmlSkinComponent::timerCallback() {
    if (message_.isNotEmpty() && juce::Time::getMillisecondCounter() > messageUntil_) {
        message_.clear();
        repaint();
    }
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    logRates(now);
    if (!visibleOnScreen()) return;
    const uint32_t serial = proc_.engine().uiSerial();
    if (frameWanted_.exchange(false) || serial != lastSerial_ || now >= nextWake_.load()) {
        lastSerial_ = serial;
        gl_.triggerRepaint();
    }
}

// The skin log gets one line per second in which the panel was active: the OS's panel messages, panel changes seen
// by the window (engine UI counter), frames drawn, frames with a change, Lua callbacks.
void RmlSkinComponent::logRates(double now) {
    if (now - rateStart_ < 1.0) return;
    const uint64_t frames = framesDrawn_.load(), messages = proc_.engine().panelMessages(), lua = luaCalls_.load(),
                   changed = changedFrames_.load();
    const uint32_t serial = proc_.engine().uiSerial();
    if (rateStart_ > 0 && (frames != rateFrames_ || messages != rateMessages_)) {
        char b[200];
        std::snprintf(b, sizeof b, "ui rates (%.1f s): %llu panel messages, %u panel changes, %llu frames drawn (%llu with a change), %llu Lua callbacks%s",
                      now - rateStart_, (unsigned long long)(messages - rateMessages_), serial - rateSerial_,
                      (unsigned long long)(frames - rateFrames_), (unsigned long long)(changed - rateChanged_),
                      (unsigned long long)(lua - rateLua_), visibleOnScreen() ? "" : " (window not visible)");
        writeLog(this, b);
    }
    rateStart_ = now; rateFrames_ = frames; rateMessages_ = messages; rateLua_ = lua; rateChanged_ = changed; rateSerial_ = serial;
}

// Skin log: ~/Documents/Phyzo/skin-log.txt (warnings, Lua errors and Log.Message output of the loaded skin).
void RmlSkinComponent::writeLog(void* self, const std::string& line) {
    auto* c = static_cast<RmlSkinComponent*>(self);
    if (c->logFile_.getSize() > (1 << 20)) c->logFile_.replaceWithText({});
    c->logFile_.appendText(juce::String::fromUTF8(line.c_str()) + "\n", false, false, "\n");
}

// GL thread: tell the message thread how loading went.
void RmlSkinComponent::startLoad() {
    if (!view_ || name_.empty()) return;
    writeLog(this, "--- " + juce::Time::getCurrentTime().toString(true, true).toStdString() + ": loading skin \"" + name_ + "\" from " + folder_);
    std::string err;
    const bool ok = view_->load(folder_, name_, err);
    if (ok && debugger_) view_->setDebuggerVisible(true);
    juce::Component::SafePointer<RmlSkinComponent> self(this);
    juce::MessageManager::callAsync([self, ok, err] {
        if (self && self->onLoaded) self->onLoaded(ok, juce::String::fromUTF8(err.c_str()));
    });
}

// ------------------------------------------------------------------ GL thread

void RmlSkinComponent::newOpenGLContextCreated() {
    std::string message;
    juce::String error;
    if (!RmlGLRenderer::loadGL(message)) error = "OpenGL could not be started: " + juce::String(message);
    else {
        renderer_ = std::make_unique<RmlGLRenderer>(decodeImage);
        if (!renderer_->ok()) { error = "The OpenGL skin renderer could not be started."; renderer_.reset(); }
    }
    if (error.isNotEmpty()) {
        juce::Component::SafePointer<RmlSkinComponent> self(this);
        juce::MessageManager::callAsync([self, error] { if (self && self->onLoaded) self->onLoaded(false, error); });
        return;
    }
    skin::PluginInfo info{"Phyzo", "DHammers", JucePlugin_VersionString};
    view_ = std::make_unique<skin::SkinView>(proc_.panelPort(), info, renderer_->rml());
    view_->onLog = &RmlSkinComponent::writeLog;
    view_->onLogUser = this;
}

void RmlSkinComponent::renderOpenGL() {
    if (!view_) return;
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lk(queueMtx_);
        work.swap(queue_);
    }
    for (auto& f : work) f();
    const float scale = float(gl_.getRenderingScale());
    scale_ = scale;
    const int w = std::max(1, juce::roundToInt(float(width_.load()) * scale));
    const int h = std::max(1, juce::roundToInt(float(height_.load()) * scale));
    const float dp = zoom_.load() * scale;
    if (w != viewW_ || h != viewH_ || std::abs(dp - viewDp_) > 1e-4f) {
        viewW_ = w; viewH_ = h; viewDp_ = dp;
        view_->setViewport(w, h, dp);
    }
    view_->update(juce::Time::getMillisecondCounterHiRes() / 1000.0);
    nextWake_ = view_->nextWake();
    const skin::SkinView::Stats st = view_->stats();
    luaCalls_ = st.luaCalls; changedFrames_ = st.changedFrames;
    std::lock_guard<std::recursive_mutex> lk(skin::skinLock());      // RmlUi's shared state, also used by the renderer
    renderer_->beginFrame(w, h);
    view_->render();
    renderer_->endFrame();
    ++framesDrawn_;
}

void RmlSkinComponent::openGLContextClosing() {
    std::lock_guard<std::recursive_mutex> lk(skin::skinLock());
    view_.reset();
    renderer_.reset();
}

// ------------------------------------------------------------------ message thread: input

void RmlSkinComponent::paint(juce::Graphics& g) {
    // Resize grip (the editor's own corner resizer sits here but is hidden by the OpenGL view).
    g.setColour(juce::Colours::white.withAlpha(0.35f));
    for (int i = 1; i <= 3; ++i) {
        const float d = float(i) * 4.0f;
        g.drawLine(float(getWidth()) - d, float(getHeight()) - 2.0f, float(getWidth()) - 2.0f, float(getHeight()) - d, 1.0f);
    }
    auto bounds = getLocalBounds();
    for (const juce::String* text : {&persistent_, &message_}) {
        if (text->isEmpty()) continue;
        const auto area = bounds.removeFromTop(64).reduced(12, 8);
        g.setColour(juce::Colour(0xe0181818));
        g.fillRoundedRectangle(area.toFloat(), 6);
        g.setColour(juce::Colour(0xffff3020));
        g.drawRoundedRectangle(area.toFloat(), 6, 1);
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        g.drawFittedText(*text, area.reduced(10, 4), juce::Justification::centredLeft, 3);
    }
}

void RmlSkinComponent::resized() { width_ = getWidth(); height_ = getHeight(); frameWanted_ = true; }

int RmlSkinComponent::toPx(float v) const { return juce::roundToInt(v * scale_.load()); }

int RmlSkinComponent::mods(const juce::ModifierKeys& m) {
    return (m.isCtrlDown() ? skin::ModCtrl : 0) | (m.isShiftDown() ? skin::ModShift : 0) |
           (m.isAltDown() ? skin::ModAlt : 0) | (m.isCommandDown() ? skin::ModMeta : 0);
}

void RmlSkinComponent::mouseMove(const juce::MouseEvent& e) {
    const bool corner = onCornerResize && e.x >= getWidth() - kCornerSize && e.y >= getHeight() - kCornerSize;
    setMouseCursor(corner ? juce::MouseCursor::BottomRightCornerResizeCursor : juce::MouseCursor::NormalCursor);
    const int x = toPx(e.position.x), y = toPx(e.position.y), m = mods(e.mods);
    post([this, x, y, m] { view_->mouseMove(x, y, m); });
}

void RmlSkinComponent::mouseDrag(const juce::MouseEvent& e) {
    if (cornerDrag_) { if (onCornerResize) onCornerResize(1, e.getScreenPosition() - cornerStart_); return; }
    mouseMove(e);
}

void RmlSkinComponent::mouseDown(const juce::MouseEvent& e) {
    const int bannerBottom = (persistent_.isNotEmpty() ? 64 : 0) + (message_.isNotEmpty() ? 64 : 0);
    if (message_.isNotEmpty() && e.position.y < bannerBottom && e.position.y >= bannerBottom - 64) { showMessage({}); return; }   // click hides it
    if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(e); return; }      // right-click never presses
    if (onCornerResize && e.x >= getWidth() - kCornerSize && e.y >= getHeight() - kCornerSize) {
        cornerDrag_ = true;
        cornerStart_ = e.getScreenPosition();
        onCornerResize(0, {});
        return;
    }
    grabKeyboardFocus();
    const int x = toPx(e.position.x), y = toPx(e.position.y), m = mods(e.mods);
    const int button = e.mods.isMiddleButtonDown() ? 2 : 0;
    post([this, x, y, m, button] { view_->mouseMove(x, y, m); view_->mouseDown(button, m); });
}

void RmlSkinComponent::mouseUp(const juce::MouseEvent& e) {
    if (cornerDrag_) { cornerDrag_ = false; if (onCornerResize) onCornerResize(2, e.getScreenPosition() - cornerStart_); return; }
    if (e.mods.isPopupMenu()) return;
    const int m = mods(e.mods);
    const int button = e.mods.isMiddleButtonDown() ? 2 : 0;
    post([this, m, button] { view_->mouseUp(button, m); });
}

void RmlSkinComponent::mouseExit(const juce::MouseEvent&) { post([this] { view_->mouseLeave(); }); }

void RmlSkinComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    const float notches = -w.deltaY / kWheelNotch;          // RmlUi: positive = towards the user
    const int m = mods(e.mods);
    post([this, notches, m] { view_->mouseWheel(notches, m); });
}

bool RmlSkinComponent::keyPressed(const juce::KeyPress& k) {
    if (k.getKeyCode() == juce::KeyPress::F5Key) { reload(); return true; }
    if (k == juce::KeyPress::escapeKey) { post([this] { view_->releaseHeldButtons(); }); return true; }
    return false;
}
