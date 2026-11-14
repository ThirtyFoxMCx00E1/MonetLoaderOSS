#include "file_manager.h"
#include "utils/android.h"
#include <filesystem>
#include <string>
#include <system_error>

void file_manager::init()
{
  external_storage = std::filesystem::path { andutils::jni::get_external_storage() }.parent_path();
  public_external_storage = std::filesystem::path { andutils::jni::get_external_media() };

  // Initialize internal storage path
  internal_storage = "/data/data/" + andutils::get_package_name() + "/";

  // MP Multiplayer: use the folder that really has files/TYFMC/MP (MP logs it as "Data folder: ...")
  std::error_code ec;
  mp_root = external_storage;
  const std::string pkg = andutils::get_package_name();
  const std::filesystem::path cands[] = {
    external_storage,
    std::filesystem::path { "/storage/emulated/0/Android_unprotected/data" } / pkg,
    std::filesystem::path { "/sdcard/Android_unprotected/data" } / pkg,
  };
  for (const auto& c : cands) {
    if (std::filesystem::exists(c / "files" / "TYFMC" / "MP", ec)) {
      mp_root = c;
      break;
    }
  }
}