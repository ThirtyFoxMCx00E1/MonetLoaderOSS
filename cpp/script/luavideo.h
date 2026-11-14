#pragma once
#include <sol/sol.hpp>

namespace luavideo {
void reg(sol::state& s);
// 3.8.5-mp: the TV picture as a 3D mesh (RwIm3D in RenderEffects, with the depth buffer)
void init3d(void* gtasa_handle); // looks up RwIm3D in libGTASA
void set_hooked3d(bool on);      // the RenderEffects hook is in
bool available3d();
void frame_tick();               // CGame::Process: frees old rasters, drops last frame's draws
void render3d();                 // RenderEffects: draws the queued meshes
void restore_screens_if_idle();  // 3.8.7-mp: TV screen materials nobody updates any more get their texture back
}
