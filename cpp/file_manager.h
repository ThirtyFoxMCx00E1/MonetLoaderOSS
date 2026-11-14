#pragma once
#include <filesystem>
#include <string>

namespace file_manager {
void init();
inline std::filesystem::path external_storage;
inline std::filesystem::path public_external_storage;
// MP Multiplayer: Android/data/com.rockstargames.gtasa/files/TYFMC/MP/moonloader
inline std::filesystem::path mp_moonloader_path() { return external_storage / "files" / "TYFMC" / "MP" / "moonloader"; }
inline std::filesystem::path internal_storage;
};