#pragma once
// MonetLoader for MP: runs PC MoonLoader scripts on TYFMC MP servers (no libsamp.so). See mp_lua_bridge.h.
namespace mp_bridge {
void process(); // every CGame::Process: finds MP, keeps players / peds in sync, applies emulated RPCs
bool active();
}
