# MonetLoader 3.9.1-mp
A Lua script loader for GTA: San Andreas (and SAMP) on Android.<br>
Main goal - compatibility with PC MoonLoader (as far as Mobile allows it).

## Source layout:
1. `cpp` - Main MonetLoader code.
   * `game` - GTA and SAMP stuff.
   * `gui` - ImGui, input and script rendering framework.
   * `hook` - MonetHook hooking library.
   * `script` - Main script runtime implementation, `script/modules` contains packaged into MonetLoader modules.
   * `utils` - Various utility.
2. `dist` - Lua redistributable libraries.
3. `example` - Example MonetLoader scripts.
4. `lib` - Third-party code.

## Installation:
1. Setup NDK environment (toolchain, ANDROID_ABI, ANDROID_PLATFORM, ANDROID_STL).
2. Load the root CMakeLists.txt, wait for it to configure and build.
3. Ship generated libmonetloader.so together with respective Lua binary from lib/lua/bin.
4. Copy data from `dist` and scripts into `Android/media/<package name>` at external storage.

## 3.9.1-mp (MP Multiplayer build)
Fixes `imgui.lua:7: undefined symbol: luaJIT_BC_mimgui_cdefs` when MonetLoader is loaded under another file name
(MP loads it as `libMP_monetloader.so` from the game's private folder):
* `MONET_LIB_PATH` global = the real path of the loaded library.
* `ffi.load("monetloader")` / `ffi.load("libmonetloader.so")` resolve to that library (fallback: `/proc/self/maps`).
* mimgui `cdefs` and the other `dist/lib/mimgui` Lua files are built into the library (`tools/embed_lua.py` ->
  `cpp/script/modules/mimgui/embedded_lua.h`) and served by a package loader, so mimgui works even if a file is missing.
* `dist/lib/mimgui/imgui.lua` and `dist/lib/monethook.lua` use the same `__monet_lib()` resolver.

Build (NDK r30): `cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a
-DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
Regenerate the embedded Lua after editing `dist/lib/mimgui`: `python3 tools/embed_lua.py`.
