#pragma once
// 4.0.0-mp: PC MoonLoader compatibility layer.
//
//  * PC address translation: every PC GTA:SA 1.0 US address that plugin-sdk-sa names (6000+ variables, arrays and
//    functions, generated into pc_addr_map.inc by tools/gen_pc_addr_map.py) is resolved at runtime against the real
//    libGTASA.so exported symbols (demangled ELF .dynsym), so PC scripts that do memory.getint32(0xB7CB84),
//    readMemory(0xBA6748 + ...), callFunction(0x56E210, 1, 1, 0) reach the matching mobile object / function.
//    PC-range addresses that have no mobile counterpart never crash: reads give 0, writes are skipped, calls refuse,
//    each logged once with the plugin-sdk name.
//  * PC-only MoonLoader functions that MonetLoader lacked: callFunction, callMethod, getModuleHandle,
//    getModuleProcAddress, loadDynamicLibrary, freeDynamicLibrary, getDynamicLibraryProcedure, getGameGlobal,
//    setGameGlobal, getGameGlobalPtr, isKeyDown, wasKeyPressed, wasKeyReleased, setVirtualKeyDown,
//    isGameWindowForeground, getD3DDevicePtr.
#include <cstddef>
#include <cstdint>
#include <string>
#include <sol/sol.hpp>

namespace lua::pccompat {
struct entry {
  std::uint32_t pc;
  char type; // 'V' variable, 'A' array, 'F' function
  std::uint8_t kind; // 0 primitive, 1 pointer, 2 struct
  std::uint8_t size; // PC element size (0 unknown)
  const char* params; // per-argument kinds: i int/pointer, f float, d double, s struct by value, ? unknown
  const char* name;
};

struct result {
  std::uintptr_t address = 0;
  bool ok = false; // false: PC address without a mobile counterpart
  bool widen = false; // PC 4-byte pointer that is 8 bytes on arm64 - read/write sizeof(void*)
  bool pc = false; // the input was a PC address
  const entry* e = nullptr;
};

bool is_pc_address(std::uintptr_t address);
result translate(std::uintptr_t address, std::size_t access_size);
// translate in place for a memory access; false = PC address without counterpart (already logged once)
bool fix(std::uintptr_t& address, std::size_t size, bool* widen = nullptr);
std::uintptr_t resolve_name(const char* qualified_name); // "CWorld::Players", "FindPlayerPed" -> mobile address or 0
const entry* find_entry(std::uintptr_t pc_address);

void frame_tick(); // once per game frame: key edge states
void set_virtual_key(int vk, bool down);

void set_root(const std::string& monetloader_root);
void register_functions(sol::state& state); // global PC MoonLoader functions
int luaopen_pccompat(lua_State* L); // require 'pccompat'
}

extern "C" __attribute__((visibility("default"))) void monetloader_set_vk(int vk, int down); // MP / hardware keys
