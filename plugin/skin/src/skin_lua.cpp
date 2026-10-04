// The skin's Lua API: global tables `panel` and `plugin` in each view's environment.
//
//   panel.led(code) -> 0 off, 1 on, 2 flash        panel.onLed(code, fn(state))      panel.onBeat(code, fn())
//   panel.knob(cc) -> raw                          panel.setKnob(cc, raw)            panel.onKnob(cc, fn(raw))
//   panel.press(raw)  panel.release(raw)           panel.isHeld(raw)                 panel.onButton(raw, fn(down))
//   panel.display() -> b0, b1, b2, b3              panel.onDisplay(fn(b0, b1, b2, b3))
//   plugin.name, plugin.vendor, plugin.version
#include "skin_lua.h"
#include <RmlUi/Core/Log.h>
#include <RmlUi/Lua/IncludeLua.h>
#include <string>
#include "skin_system.h"
#include "skin_view.h"

namespace skin {
namespace {

SkinView& viewOf(lua_State* L) { return *static_cast<SkinView*>(lua_touserdata(L, lua_upvalueindex(1))); }

int intArg(lua_State* L, int i, int lo, int hi, const char* what) {
    const lua_Integer v = luaL_checkinteger(L, i);
    if (v < lo || v > hi) luaL_error(L, "%s must be %d to %d (got %d)", what, lo, hi, int(v));
    return int(v);
}

void addFn(lua_State* L, std::map<int, std::vector<int>>& m, int key) {
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    m[key].push_back(luaL_ref(L, LUA_REGISTRYINDEX));
}

int led(lua_State* L) { lua_pushinteger(L, viewOf(L).ledState(intArg(L, 1, 0, 255, "LED code"))); return 1; }
int onLed(lua_State* L) { addFn(L, viewOf(L).onLedFns, intArg(L, 1, 0, 255, "LED code")); return 0; }
int onBeat(lua_State* L) { addFn(L, viewOf(L).onBeatFns, intArg(L, 1, 0, 255, "LED code")); return 0; }
int knob(lua_State* L) { lua_pushinteger(L, viewOf(L).port().controlPosition(intArg(L, 1, 0, 25, "cc"))); return 1; }
int setKnob(lua_State* L) { viewOf(L).setKnob(intArg(L, 1, 0, 25, "cc"), intArg(L, 2, 0, 1023, "raw")); return 0; }
int onKnob(lua_State* L) { addFn(L, viewOf(L).onKnobFns, intArg(L, 1, 0, 25, "cc")); return 0; }
int press(lua_State* L) { viewOf(L).button(intArg(L, 1, 0, 0x7F, "button raw"), true); return 0; }
int release(lua_State* L) { viewOf(L).button(intArg(L, 1, 0, 0x7F, "button raw"), false); return 0; }
int isHeld(lua_State* L) { lua_pushboolean(L, viewOf(L).isHeld(intArg(L, 1, 0, 0x7F, "button raw"))); return 1; }
int onButton(lua_State* L) { addFn(L, viewOf(L).onButtonFns, intArg(L, 1, 0, 0x7F, "button raw")); return 0; }
int display(lua_State* L) {
    for (uint8_t b : viewOf(L).displayBytes()) lua_pushinteger(L, b);
    return 4;
}
int onDisplay(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    viewOf(L).onDisplayFns.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

int traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

}  // namespace

void installLuaApi(SkinView& v) {
    lua_State* L = luaState();
    const luaL_Reg fns[] = {{"led", led},         {"onLed", onLed},       {"onBeat", onBeat},     {"knob", knob},
                            {"setKnob", setKnob}, {"onKnob", onKnob},     {"press", press},       {"release", release},
                            {"isHeld", isHeld},   {"onButton", onButton}, {"display", display},   {"onDisplay", onDisplay},
                            {nullptr, nullptr}};
    lua_newtable(L);
    lua_pushlightuserdata(L, &v);
    luaL_setfuncs(L, fns, 1);
    lua_setglobal(L, "panel");

    lua_newtable(L);
    lua_pushstring(L, v.info().name.c_str()); lua_setfield(L, -2, "name");
    lua_pushstring(L, v.info().vendor.c_str()); lua_setfield(L, -2, "vendor");
    lua_pushstring(L, v.info().version.c_str()); lua_setfield(L, -2, "version");
    lua_setglobal(L, "plugin");
}

void callLua(const std::vector<int>& fns, const std::vector<lua_Integer>& args) {
    lua_State* L = luaState();
    if (!L) return;
    const std::vector<int> copy = fns;               // a callback may register more callbacks
    for (int ref : copy) {
        if (SkinView* v = SkinView::current()) v->countLuaCall();
        const int base = lua_gettop(L);
        lua_pushcfunction(L, traceback);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        for (lua_Integer a : args) lua_pushinteger(L, a);
        if (lua_pcall(L, int(args.size()), 0, base + 1) != LUA_OK)
            Rml::Log::Message(Rml::Log::LT_ERROR, "Lua: %s", lua_tostring(L, -1));
        lua_settop(L, base);
    }
}

void callLuaBool(const std::vector<int>& fns, bool arg) {
    lua_State* L = luaState();
    if (!L) return;
    const std::vector<int> copy = fns;
    for (int ref : copy) {
        if (SkinView* v = SkinView::current()) v->countLuaCall();
        const int base = lua_gettop(L);
        lua_pushcfunction(L, traceback);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_pushboolean(L, arg);
        if (lua_pcall(L, 1, 0, base + 1) != LUA_OK)
            Rml::Log::Message(Rml::Log::LT_ERROR, "Lua: %s", lua_tostring(L, -1));
        lua_settop(L, base);
    }
}

void dropLuaCallbacks(SkinView& v) {
    lua_State* L = luaState();
    auto drop = [&](std::vector<int>& refs) { if (L) for (int r : refs) luaL_unref(L, LUA_REGISTRYINDEX, r); refs.clear(); };
    for (auto* m : {&v.onLedFns, &v.onBeatFns, &v.onKnobFns, &v.onButtonFns}) {
        for (auto& kv : *m) drop(kv.second);
        m->clear();
    }
    drop(v.onDisplayFns);
}

}  // namespace skin
