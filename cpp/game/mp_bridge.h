#pragma once
// MonetLoader for MP: runs PC MoonLoader scripts on TYFMC MP servers (no libsamp.so). See mp_lua_bridge.h.
namespace mp_bridge {
void process(); // every CGame::Process: finds MP, keeps players / peds in sync, applies emulated RPCs
bool active();
bool try_attach(); // 3.8.1: find MP now (script load, isSampLoaded) - true when attached
// 3.8.4: MP's touches (game resolution, MP's own UI taps left out) for the scripts' onTouch
bool touch_feed(); // MP has MP_PollScriptTouch: onTouch comes from MP, not from MonetLoader's touch hook
bool poll_touch(int& type, int& num, int& x, int& y);

}
