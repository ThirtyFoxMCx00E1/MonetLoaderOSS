#include "compat.h"
#include "file_manager.h"
#include "logger.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/error/error.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/ostreamwrapper.h"
#include "rapidjson/prettywriter.h"
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace {
void set_default_profile(rapidjson::Document& doc)
{
  // MP: the document must be an object before members are added. The old code wrote doc["key"] on an empty
  // (null) document: rapidjson has no such member, so nothing was stored, the profile was "invalid", and
  // gtasa_name = doc["gtasa_name"].GetString() read a null string -> SIGSEGV in strlen on the first start.
  doc.SetObject();
  auto& a = doc.GetAllocator();
  doc.AddMember("gtasa_name", rapidjson::Value("libGTASA.so", a), a);
  doc.AddMember("compat_scripts", rapidjson::Value(rapidjson::kArrayType), a);
#if INTPTR_MAX == INT64_MAX
  doc.AddMember("profile_name", rapidjson::Value("default GTA:2.1 SAMP:NULL", a), a);
  doc.AddMember("samp_name", rapidjson::Value("", a), a);
  doc.AddMember("receiveignorerpc_pattern", rapidjson::Value("", a), a);
  doc.AddMember("cnetgame_ctor_pattern", rapidjson::Value("", a), a);
  doc.AddMember("rakclientinterface_netgame_offset", 0, a);
  doc.AddMember("use_samp_touch_workaround", false, a);
  doc.AddMember("nveventinsertnewest_offset", 0, a);
#else
  doc.AddMember("profile_name", rapidjson::Value("default GTA:2.0 SAMP:ARZ-15.0.8", a), a);
  doc.AddMember("samp_name", rapidjson::Value("libsamp.so", a), a);
  doc.AddMember("receiveignorerpc_pattern", rapidjson::Value("F0B503AF2DE900????B004460068C16A20468847", a), a);
  doc.AddMember("cnetgame_ctor_pattern", rapidjson::Value("F0B503AF2DE9000788B00D46????9146????0446002079447A44", a), a);
  doc.AddMember("rakclientinterface_netgame_offset", 528, a);
  doc.AddMember("use_samp_touch_workaround", true, a);
  doc.AddMember("nveventinsertnewest_offset", 0x27C4F0, a);
#endif
}
}

void compat::init()
{
  std::filesystem::path path = file_manager::mp_moonloader_path() / "config" / "monetloader_compat.json";

  // set default profile

  rapidjson::Document doc {};
  set_default_profile(doc);

  // try to load user profile

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    // if profile does not exists, write default one
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream f { path };
    if (f) {
      rapidjson::OStreamWrapper f_wrap { f };
      rapidjson::PrettyWriter<rapidjson::OStreamWrapper> out { f_wrap };
      doc.Accept(out);
    }

    f.clear();
    f.close();
  } else {
    // load user profile

    std::ifstream f { path };
    if (f) {
      rapidjson::IStreamWrapper f_wrap { f };
      rapidjson::ParseResult result = doc.ParseStream(f_wrap);
      if (result.Code() != rapidjson::kParseErrorNone) {
        logger::log<logger::LV_ERROR, false>(nullptr,
            "{}: {} (at: {})",
            "failed to parse compat profile file",
            rapidjson::GetParseError_En(result.Code()),
            result.Offset());
      }
    } else {
      logger::log<logger::LV_ERROR, false>(nullptr,
          "{}", "failed to open compat profile file, using default");
    }

    f.clear();
    f.close();
  }

  // validate profile

  const char* s_gtasa_name = "gtasa_name";
  const char* s_compat_scripts = "compat_scripts";
  const char* s_profile_name = "profile_name";
  const char* s_samp_name = "samp_name";
  const char* s_receiveignorerpc_pattern = "receiveignorerpc_pattern";
  const char* s_cnetgame_ctor_pattern = "cnetgame_ctor_pattern";
  const char* s_rakclientinterface_netgame_offset = "rakclientinterface_netgame_offset";
  const char* s_use_samp_touch_workaround = "use_samp_touch_workaround";
  const char* s_nveventinsertnewest_offset = "nveventinsertnewest_offset";

  if (!doc.IsObject() || !doc.HasMember(s_gtasa_name) || !doc[s_gtasa_name].IsString()
      || !doc.HasMember(s_compat_scripts) || !doc[s_compat_scripts].IsArray()
      || !doc.HasMember(s_profile_name) || !doc[s_profile_name].IsString()
      || !doc.HasMember(s_samp_name) || !doc[s_samp_name].IsString()
      || !doc.HasMember(s_receiveignorerpc_pattern) || !doc[s_receiveignorerpc_pattern].IsString()
      || !doc.HasMember(s_cnetgame_ctor_pattern) || !doc[s_cnetgame_ctor_pattern].IsString()
      || !doc.HasMember(s_rakclientinterface_netgame_offset) || !doc[s_rakclientinterface_netgame_offset].IsNumber()
      || !doc.HasMember(s_use_samp_touch_workaround) || !doc[s_use_samp_touch_workaround].IsBool()
      || !doc.HasMember(s_nveventinsertnewest_offset) || !doc[s_nveventinsertnewest_offset].IsNumber()) {
    logger::log<logger::LV_ERROR, false>(nullptr,
        "{}", "invalid compat profile, using default");
    set_default_profile(doc);
  }

  // parse profile

  gtasa_name = doc[s_gtasa_name].GetString();

  compat_scripts.clear();
  for (auto& i : doc[s_compat_scripts].GetArray()) {
    if (i.IsString()) {
      compat_scripts.push_back(i.GetString());
    }
  }

  profile_name = doc[s_profile_name].GetString();
  samp_name = doc[s_samp_name].GetString();
  receiveignorerpc_pattern = doc[s_receiveignorerpc_pattern].GetString();
  cnetgame_ctor_pattern = doc[s_cnetgame_ctor_pattern].GetString();
  rakclientinterface_netgame_offset = static_cast<std::uintptr_t>(doc[s_rakclientinterface_netgame_offset].GetDouble());
  use_samp_touch_workaround = doc[s_use_samp_touch_workaround].GetBool();
  nveventinsertnewest_offset = static_cast<std::uintptr_t>(doc[s_nveventinsertnewest_offset].GetDouble());

  logger::log<logger::LV_SYSTEM, false>(nullptr, "{}: {}",
      "loaded compat profile",
      profile_name);
}