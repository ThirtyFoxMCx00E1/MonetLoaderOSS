#pragma once
#include <filesystem>
#include <string>

namespace file_manager {
void init();
inline std::filesystem::path external_storage;
inline std::filesystem::path public_external_storage;
// MP Multiplayer: Android/data/com.rockstargames.gtasa/files/TYFMC/MP/moonloader
// MP data root (folder that holds files/TYFMC/MP). Some phones/APKs keep it under Android_unprotected/data/<pkg>
// instead of the folder getExternalFilesDir() gives, so file_manager::init() picks the one where MP's folder is.
inline std::filesystem::path mp_root;
inline std::filesystem::path mp_moonloader_path() { return (mp_root.empty() ? external_storage : mp_root) / "files" / "TYFMC" / "MP" / "moonloader"; }
inline std::filesystem::path internal_storage;
};