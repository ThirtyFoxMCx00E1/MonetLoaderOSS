// 4.0.0-mp: PC MoonLoader compatibility layer - see pccompat.h
#include "pccompat.h"
#include "lib_manager.h"
#include "logger.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cxxabi.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <link.h>
#include <mutex>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lua::pccompat {
namespace {
const entry g_table[] = {
#include "../pc_addr_map.inc"
};
constexpr std::size_t g_count = sizeof(g_table) / sizeof(g_table[0]);
constexpr std::uintptr_t PC_BASE = 0x400000; // gta_sa.exe image base
constexpr std::uintptr_t PC_END = 0x1000000;

std::mutex g_lock;
bool g_resolved = false;
std::vector<std::uintptr_t> g_mobile; // parallel to g_table, 0 = unresolved
std::size_t g_resolved_count = 0;
std::unordered_set<std::uint32_t> g_logged;
std::string g_root;

struct sym_candidate {
  std::uintptr_t address;
  int nparams;
};

int count_params(std::string_view sig)
{
  auto open = sig.find('(');
  if (open == std::string_view::npos) {
    return -1;
  }
  int depth = 0, commas = 0;
  bool any = false;
  for (std::size_t i = open + 1; i < sig.size(); ++i) {
    char c = sig[i];
    if (c == '(' || c == '<') {
      ++depth;
    } else if (c == ')' || c == '>') {
      if (depth == 0) {
        break;
      }
      --depth;
    } else if (c == ',' && depth == 0) {
      ++commas;
    } else if (c != ' ') {
      any = true;
    }
  }
  if (!any || sig.substr(open + 1, 5) == "void)") {
    return 0;
  }
  return commas + 1;
}

std::string strip_params(std::string_view sig)
{
  // "CPed::SetModelIndex(unsigned int)" -> "CPed::SetModelIndex"; "operator()" kept intact
  std::size_t start = 0;
  auto op = sig.find("operator()");
  if (op != std::string_view::npos) {
    start = op + 10;
  }
  auto p = sig.find('(', start);
  return std::string(p == std::string_view::npos ? sig : sig.substr(0, p));
}

// Read libGTASA.so's .dynsym from the file and match every demangled symbol to the plugin-sdk names we need.
void resolve_locked()
{
  if (g_resolved) {
    return;
  }
  g_resolved = true;
  g_mobile.assign(g_count, 0);
  if (!lib_manager::gtasa_base || lib_manager::gtasa_path.empty()) {
    logger::log<logger::LV_WARN>(nullptr, "pccompat: libGTASA.so not located yet, PC addresses stay unresolved");
    g_resolved = false;
    return;
  }

  std::unordered_map<std::string, std::vector<std::size_t>> wanted;
  wanted.reserve(g_count);
  for (std::size_t i = 0; i < g_count; ++i) {
    wanted[g_table[i].name].push_back(i);
  }

  int fd = open(lib_manager::gtasa_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    logger::log<logger::LV_ERROR>(nullptr, "pccompat: can't open {}", lib_manager::gtasa_path);
    return;
  }
  auto rd = [fd](void* dst, std::size_t size, off_t off) { return pread(fd, dst, size, off) == static_cast<ssize_t>(size); };
  ElfW(Ehdr) eh {};
  std::vector<ElfW(Shdr)> sh;
  if (rd(&eh, sizeof(eh), 0) && std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 && eh.e_shentsize == sizeof(ElfW(Shdr))) {
    sh.resize(eh.e_shnum);
    if (!rd(sh.data(), sizeof(ElfW(Shdr)) * eh.e_shnum, static_cast<off_t>(eh.e_shoff))) {
      sh.clear();
    }
  }
  std::unordered_map<std::string, std::vector<sym_candidate>> found;
  for (auto& s : sh) {
    if (s.sh_type != SHT_DYNSYM || s.sh_link >= sh.size()) {
      continue;
    }
    auto& strs = sh[s.sh_link];
    std::vector<ElfW(Sym)> syms(s.sh_size / sizeof(ElfW(Sym)));
    std::vector<char> str(strs.sh_size + 1, 0);
    if (!rd(syms.data(), syms.size() * sizeof(ElfW(Sym)), static_cast<off_t>(s.sh_offset)) ||
        !rd(str.data(), strs.sh_size, static_cast<off_t>(strs.sh_offset))) {
      continue;
    }
    char* buf = nullptr;
    std::size_t buflen = 0;
    for (auto& sym : syms) {
      if (sym.st_shndx == SHN_UNDEF || sym.st_value == 0 || sym.st_name >= strs.sh_size) {
        continue;
      }
      const char* raw = str.data() + sym.st_name;
      std::string_view demangled = raw;
      if (raw[0] == '_' && raw[1] == 'Z') {
        int status = 0;
        char* out = abi::__cxa_demangle(raw, buf, &buflen, &status);
        if (status != 0 || !out) {
          continue;
        }
        buf = out;
        demangled = out;
      }
      std::string key = strip_params(demangled);
      if (wanted.find(key) == wanted.end()) {
        continue;
      }
      int type = ELF_ST_TYPE(sym.st_info);
      std::uintptr_t addr = lib_manager::gtasa_base + sym.st_value;
#if defined(__arm__)
      if (type == STT_FUNC) {
        addr |= (sym.st_value & 1); // keep thumb bit (already in st_value)
      }
#endif
      found[key].push_back({ addr, type == STT_FUNC ? count_params(demangled) : -1 });
    }
    std::free(buf);
  }
  close(fd);

  for (auto& [name, idxs] : wanted) {
    auto it = found.find(name);
    if (it == found.end()) {
      continue;
    }
    for (auto i : idxs) {
      const entry& e = g_table[i];
      auto& cands = it->second;
      std::uintptr_t pick = 0;
      if (e.type != 'F' || cands.size() == 1) {
        pick = cands.front().address;
      } else {
        int pc_n = static_cast<int>(std::strlen(e.params));
        for (auto& c : cands) { // member functions: PC list includes `this`
          if (e.params[0] != '?' && (c.nparams == pc_n || c.nparams + 1 == pc_n)) {
            pick = c.address;
            break;
          }
        }
      }
      if (pick) {
        g_mobile[i] = pick;
        ++g_resolved_count;
      }
    }
  }
  logger::log<logger::LV_SYSTEM>(nullptr, "pccompat: {} of {} PC GTA:SA addresses (plugin-sdk-sa) resolved to libGTASA.so symbols",
      g_resolved_count, g_count);
}

void ensure_resolved()
{
  std::lock_guard lk { g_lock };
  resolve_locked();
}

// ---- mapped memory check (only for the low PC range, mobile never maps code/data there on arm64) ----
struct range {
  std::uintptr_t lo, hi;
};
std::mutex g_maps_lock;
std::vector<range> g_maps;
std::chrono::steady_clock::time_point g_maps_time {};

void reload_maps()
{
  g_maps.clear();
  FILE* f = std::fopen("/proc/self/maps", "re");
  if (!f) {
    return;
  }
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    unsigned long long lo = 0, hi = 0;
    if (std::sscanf(line, "%llx-%llx", &lo, &hi) == 2 && lo < PC_END) {
      g_maps.push_back({ static_cast<std::uintptr_t>(lo), static_cast<std::uintptr_t>(hi) });
    }
  }
  std::fclose(f);
  g_maps_time = std::chrono::steady_clock::now();
}

bool mapped_low(std::uintptr_t a)
{
  std::lock_guard lk { g_maps_lock };
  auto check = [a]() {
    for (auto& r : g_maps) {
      if (a >= r.lo && a < r.hi) {
        return true;
      }
    }
    return false;
  };
  if (g_maps_time.time_since_epoch().count() == 0) {
    reload_maps();
  }
  if (check()) {
    return true;
  }
  if (std::chrono::steady_clock::now() - g_maps_time > std::chrono::seconds(2)) {
    reload_maps();
    return check();
  }
  return false;
}

void log_once(std::uintptr_t a, const char* what)
{
  {
    std::lock_guard lk { g_lock };
    if (!g_logged.insert(static_cast<std::uint32_t>(a)).second || g_logged.size() > 4096) {
      return;
    }
  }
  const entry* e = find_entry(a);
  logger::log<logger::LV_WARN>(nullptr, "pccompat: PC address 0x{:X} ({}) has no mobile counterpart - {}", a,
      e ? e->name : "not in plugin-sdk-sa", what);
}

// ---- keys (PC virtual-key codes) ----
std::atomic<bool> g_down[256];
std::atomic<bool> g_press_pending[256];
std::atomic<bool> g_release_pending[256];
bool g_pressed[256];
bool g_released[256];

// ---- native call ----
struct call_arg {
  char kind;
  std::uint64_t bits;
  double real;
};

std::uintptr_t do_call(std::uintptr_t fn, const std::vector<call_arg>& args)
{
#if defined(__aarch64__)
  std::uint64_t x[8] {};
  double v[8] {};
  int nx = 0, nv = 0;
  for (auto& a : args) {
    if (a.kind == 'f' || a.kind == 'd') {
      if (nv >= 8) {
        throw std::runtime_error("callFunction: more than 8 float arguments");
      }
      std::uint64_t raw = 0;
      if (a.kind == 'f') {
        raw = a.bits & 0xFFFFFFFFu; // callee reads s<n> = low 32 bits of v<n>
      } else {
        std::memcpy(&raw, &a.real, sizeof(raw));
      }
      std::memcpy(&v[nv++], &raw, sizeof(raw));
    } else {
      if (nx >= 8) {
        throw std::runtime_error("callFunction: more than 8 integer arguments");
      }
      x[nx++] = a.bits;
    }
  }
  using fn_t = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
      std::uint64_t, std::uint64_t, std::uint64_t, double, double, double, double, double, double, double, double);
  return reinterpret_cast<fn_t>(fn)(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
#else
  // armeabi-v7a is softfp: floats travel in core registers / stack words exactly like ints
  std::uint32_t w[16] {};
  int n = 0;
  for (auto& a : args) {
    if (a.kind == 'd') {
      if (n & 1) {
        ++n;
      }
      std::uint64_t raw;
      std::memcpy(&raw, &a.real, sizeof(raw));
      if (n + 2 > 16) {
        throw std::runtime_error("callFunction: too many arguments");
      }
      w[n++] = static_cast<std::uint32_t>(raw);
      w[n++] = static_cast<std::uint32_t>(raw >> 32);
    } else {
      if (n >= 16) {
        throw std::runtime_error("callFunction: too many arguments");
      }
      w[n++] = static_cast<std::uint32_t>(a.bits);
    }
  }
  using fn_t = std::uint32_t (*)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
      std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
      std::uint32_t, std::uint32_t, std::uint32_t);
  return reinterpret_cast<fn_t>(fn)(w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
#endif
}

call_arg make_arg(const sol::object& o, char kind, std::vector<std::string>& keep)
{
  call_arg a { kind, 0, 0.0 };
  switch (o.get_type()) {
  case sol::type::number: {
    double d = o.as<double>();
    bool integral = std::floor(d) == d && std::fabs(d) < 18446744073709551616.0;
    if (kind == 'f') {
      if (integral) { // PC semantics: the dword is pushed as is (representFloatAsInt)
        a.bits = static_cast<std::uint32_t>(static_cast<std::int64_t>(d));
      } else {
        float f = static_cast<float>(d);
        std::uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        a.bits = u;
      }
    } else if (kind == 'd') {
      a.real = d;
    } else if (kind == '?' && !integral) {
      float f = static_cast<float>(d);
      std::uint32_t u;
      std::memcpy(&u, &f, sizeof(u));
      a.kind = 'f';
      a.bits = u;
    } else {
      a.kind = 'i';
      a.bits = d < 0 ? static_cast<std::uint64_t>(static_cast<std::int64_t>(d)) : static_cast<std::uint64_t>(d);
    }
    break;
  }
  case sol::type::boolean:
    a.kind = 'i';
    a.bits = o.as<bool>() ? 1 : 0;
    break;
  case sol::type::string:
    keep.push_back(o.as<std::string>());
    a.kind = 'i';
    a.bits = reinterpret_cast<std::uintptr_t>(keep.back().c_str());
    break;
  case sol::type::lightuserdata:
  case sol::type::userdata:
    a.kind = 'i';
    a.bits = reinterpret_cast<std::uintptr_t>(o.as<void*>());
    break;
  default:
    a.kind = 'i';
    a.bits = 0;
  }
  return a;
}

std::uintptr_t call_native(std::uintptr_t address, std::optional<std::uintptr_t> self, int nparams, sol::variadic_args va)
{
  auto r = translate(address, 0);
  if (!r.ok) {
    const entry* e = find_entry(address);
    throw std::runtime_error(fmt::format("callFunction: PC function 0x{:X} ({}) has no counterpart in libGTASA.so", address,
        e ? e->name : "not in plugin-sdk-sa"));
  }
  const entry* e = r.e;
  std::string kinds = (e && e->type == 'F') ? e->params : "";
  if (self && !kinds.empty()) {
    kinds.erase(0, 1); // `this`
  }
  if (kinds.find('s') != std::string::npos) {
    throw std::runtime_error(fmt::format("callFunction: {} takes a struct by value, call it through ffi instead", e->name));
  }
  std::vector<std::string> keep;
  std::vector<call_arg> args;
  if (self) {
    args.push_back({ 'i', *self, 0.0 });
  }
  int i = 0;
  for (auto v : va) {
    if (nparams >= 0 && i >= nparams) {
      break;
    }
    char k = (i < static_cast<int>(kinds.size()) && kinds[0] != '?') ? kinds[i] : '?';
    args.push_back(make_arg(sol::object(v), k, keep));
    ++i;
  }
  for (; i < nparams; ++i) {
    args.push_back({ 'i', 0, 0.0 });
  }
  return do_call(r.address, args);
}

std::string to_lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Map a Windows module name to the Android library that provides the same API, if any.
std::vector<std::string> so_candidates(const std::string& name)
{
  namespace fs = std::filesystem;
  std::string base = fs::path(name).filename().string();
  std::string stem = fs::path(base).stem().string();
  std::string lower = to_lower(base);
  std::vector<std::string> out;
  bool win = lower.ends_with(".dll") || lower.ends_with(".asi") || lower.ends_with(".exe");
  std::vector<std::string> names;
  if (win) {
    names = { "lib" + stem + ".so", stem + ".so" };
  } else {
    names = { base };
    if (!lower.ends_with(".so")) {
      names.push_back("lib" + base + ".so");
      names.push_back(base + ".so");
    }
  }
  std::vector<fs::path> dirs;
  if (fs::path(name).has_parent_path()) {
    dirs.push_back(fs::path(name).parent_path());
  }
  if (!g_root.empty()) {
    dirs.push_back(fs::path(g_root) / "lib");
    dirs.push_back(fs::path(g_root) / "libs");
    dirs.push_back(fs::path(g_root));
  }
  for (auto& d : dirs) {
    for (auto& n : names) {
      std::error_code ec;
      if (fs::exists(d / n, ec)) {
        out.push_back((d / n).string());
      }
    }
  }
  for (auto& n : names) {
    out.push_back(n); // system / already loaded (libGTASA.so, libsamp.so, libMP.so ...)
  }
  return out;
}

bool is_windows_pe(const std::string& path)
{
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return false;
  }
  char mz[2] {};
  bool pe = std::fread(mz, 1, 2, f) == 2 && mz[0] == 'M' && mz[1] == 'Z';
  std::fclose(f);
  return pe;
}

std::unordered_set<void*> g_libs;
}

const entry* find_entry(std::uintptr_t a)
{
  if (a < PC_BASE || a >= PC_END) {
    return nullptr;
  }
  auto it = std::upper_bound(g_table, g_table + g_count, a, [](std::uintptr_t v, const entry& e) { return v < e.pc; });
  if (it == g_table) {
    return nullptr;
  }
  --it;
  return it->pc == a || it->type != 'F' ? &*it : nullptr;
}

bool is_pc_address(std::uintptr_t a)
{
  return a < PC_END && !mapped_low(a);
}

result translate(std::uintptr_t a, std::size_t access_size)
{
  result r;
  r.address = a;
  if (!is_pc_address(a)) {
    r.ok = true;
    return r;
  }
  r.pc = true;
  ensure_resolved();
  auto it = std::upper_bound(g_table, g_table + g_count, a, [](std::uintptr_t v, const entry& e) { return v < e.pc; });
  if (it == g_table) {
    return r;
  }
  --it;
  std::size_t idx = static_cast<std::size_t>(it - g_table);
  const entry& e = *it;
  std::uintptr_t mob = idx < g_mobile.size() ? g_mobile[idx] : 0;
  if (!mob) {
    return r;
  }
  std::uintptr_t off = a - e.pc;
  std::uintptr_t limit = (it + 1 < g_table + g_count) ? (it + 1)->pc - e.pc : 0x10000;
  if (e.type == 'V' && e.kind != 2 && e.size) {
    limit = std::min<std::uintptr_t>(limit, e.size);
  }
  if (off >= limit || (e.type == 'F' && off != 0)) {
    return r;
  }
  r.e = &e;
  if (off == 0 && e.type == 'F') {
    r.address = mob;
    r.ok = true;
    return r;
  }
  if (e.kind == 0) { // primitive scalars / arrays: identical layout on both ABIs
    r.address = mob + off;
    r.ok = true;
  } else if (e.kind == 1) { // pointers: 4 bytes on PC, sizeof(void*) here
    if (off % 4 == 0) {
      r.address = mob + (off / 4) * sizeof(void*);
      r.widen = sizeof(void*) == 8 && access_size == 4;
      r.ok = true;
    }
  } else { // struct objects: the object itself; members only where the 32-bit layout matches the PC one
    if (off == 0 || sizeof(void*) == 4) {
      r.address = mob + off;
      r.ok = true;
    }
  }
  return r;
}

bool fix(std::uintptr_t& address, std::size_t size, bool* widen)
{
  auto r = translate(address, size);
  if (!r.ok) {
    log_once(address, "read returns 0, write skipped");
    return false;
  }
  address = r.address;
  if (widen) {
    *widen = r.widen;
  }
  return true;
}

std::uintptr_t resolve_name(const char* name)
{
  ensure_resolved();
  for (std::size_t i = 0; i < g_count; ++i) {
    if (std::strcmp(g_table[i].name, name) == 0 && i < g_mobile.size() && g_mobile[i]) {
      return g_mobile[i];
    }
  }
  // not in plugin-sdk: try the mobile symbol table directly (mangled or C names)
  void* h = dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD);
  void* p = h ? dlsym(h, name) : nullptr;
  if (h) {
    dlclose(h);
  }
  return reinterpret_cast<std::uintptr_t>(p);
}

void set_virtual_key(int vk, bool down)
{
  if (vk <= 0 || vk > 255) {
    return;
  }
  bool was = g_down[vk].exchange(down);
  if (down && !was) {
    g_press_pending[vk] = true;
  } else if (!down && was) {
    g_release_pending[vk] = true;
  }
}

void frame_tick()
{
  for (int i = 0; i < 256; ++i) {
    g_pressed[i] = g_press_pending[i].exchange(false);
    g_released[i] = g_release_pending[i].exchange(false);
  }
}

void set_root(const std::string& root)
{
  g_root = root;
}

void register_functions(sol::state& state)
{
  using sol::variadic_args;
  // --- memory reads/writes through PC addresses (C++ side: readMemory & co. are wrapped in script_manager) ---
  state["callFunction"] = [](std::uintptr_t address, int nparams, int /*pop*/, variadic_args va) {
    return static_cast<double>(call_native(address, std::nullopt, nparams, va));
  };
  state["callMethod"] = [](std::uintptr_t address, std::uintptr_t self, int nparams, int /*pop*/, variadic_args va) {
    return static_cast<double>(call_native(address, self, nparams, va));
  };

  state["getModuleHandle"] = [](std::optional<std::string> name) -> double {
    std::string n = to_lower(name.value_or("gta_sa.exe"));
    if (n == "gta_sa.exe" || n == "gta_sa" || n == "gta-sa.exe") {
      return static_cast<double>(PC_BASE); // PC image base: base + PC offset is translated like any PC address
    }
    if (n == "libgtasa.so" || n == "libgtasa") {
      return static_cast<double>(lib_manager::gtasa_base);
    }
    if (n == "samp.dll" || n == "libsamp.so") {
      return static_cast<double>(lib_manager::samp_base); // SA-MP for Android when present (MP itself: 0)
    }
    for (auto& c : so_candidates(n)) {
      void* h = dlopen(c.c_str(), RTLD_NOW | RTLD_NOLOAD);
      if (h) {
        Dl_info info {};
        void* any = dlsym(h, "JNI_OnLoad");
        double base = 0;
        if (any && dladdr(any, &info)) {
          base = static_cast<double>(reinterpret_cast<std::uintptr_t>(info.dli_fbase));
        }
        dlclose(h);
        return base ? base : static_cast<double>(reinterpret_cast<std::uintptr_t>(h));
      }
    }
    return 0;
  };
  state["getModuleProcAddress"] = [](std::string module, std::string proc) -> double {
    for (auto& c : so_candidates(module)) {
      void* h = dlopen(c.c_str(), RTLD_NOW | RTLD_NOLOAD);
      if (h) {
        void* p = dlsym(h, proc.c_str());
        dlclose(h);
        if (p) {
          return static_cast<double>(reinterpret_cast<std::uintptr_t>(p));
        }
      }
    }
    if (auto a = resolve_name(proc.c_str())) {
      return static_cast<double>(a);
    }
    return 0;
  };
  state["loadDynamicLibrary"] = [](std::string path) -> std::tuple<bool, double> {
    for (auto& c : so_candidates(path)) {
      if (c.find('/') != std::string::npos && is_windows_pe(c)) {
        continue;
      }
      void* h = dlopen(c.c_str(), RTLD_NOW);
      if (h) {
        g_libs.insert(h);
        return { true, static_cast<double>(reinterpret_cast<std::uintptr_t>(h)) };
      }
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && is_windows_pe(path)) {
      logger::log<logger::LV_WARN>(nullptr,
          "loadDynamicLibrary: {} is a Windows x86 library; Android ARM can't execute it. Put an Android build next to it as lib{}.so",
          path, std::filesystem::path(path).stem().string());
    } else {
      logger::log<logger::LV_WARN>(nullptr, "loadDynamicLibrary: {} not found ({})", path, dlerror() ? dlerror() : "no .so candidate");
    }
    return { false, 0.0 };
  };
  state["freeDynamicLibrary"] = [](std::uintptr_t handle) {
    void* h = reinterpret_cast<void*>(handle);
    if (g_libs.erase(h)) {
      dlclose(h);
    }
  };
  state["getDynamicLibraryProcedure"] = [](std::string proc, std::uintptr_t handle) -> std::tuple<bool, double> {
    void* p = dlsym(reinterpret_cast<void*>(handle), proc.c_str());
    return { p != nullptr, static_cast<double>(reinterpret_cast<std::uintptr_t>(p)) };
  };

  // --- script globals ($var in main.scm): CTheScripts::ScriptSpace ---
  state["getGameGlobalPtr"] = [](int var) -> double {
    auto base = resolve_name("CTheScripts::ScriptSpace");
    return base ? static_cast<double>(base + static_cast<std::uintptr_t>(var) * 4) : 0.0;
  };
  state["getGameGlobal"] = [](int var) -> std::int32_t {
    auto base = resolve_name("CTheScripts::ScriptSpace");
    std::int32_t v = 0;
    if (base) {
      std::memcpy(&v, reinterpret_cast<void*>(base + static_cast<std::uintptr_t>(var) * 4), sizeof(v));
    }
    return v;
  };
  state["setGameGlobal"] = [](int var, std::int32_t value) {
    auto base = resolve_name("CTheScripts::ScriptSpace");
    if (base) {
      std::memcpy(reinterpret_cast<void*>(base + static_cast<std::uintptr_t>(var) * 4), &value, sizeof(value));
    }
  };

  // --- keys: virtual-key codes fed by setVirtualKeyDown, MP's PC key pad and hardware keyboards ---
  state["isKeyDown"] = [](int vk) { return vk > 0 && vk < 256 && g_down[vk].load(); };
  state["wasKeyPressed"] = [](int vk) { return vk > 0 && vk < 256 && g_pressed[vk]; };
  state["wasKeyReleased"] = [](int vk) { return vk > 0 && vk < 256 && g_released[vk]; };
  state["setVirtualKeyDown"] = [](int vk, bool down) { set_virtual_key(vk, down); };
  state["isGameWindowForeground"] = []() { return true; };
  state["getD3DDevicePtr"] = []() { return 0; }; // no Direct3D on Android (OpenGL ES); d3d9 scripts must use mimgui / render*

  // --- ffi.load("foo.dll" / "C:\\...\\foo.dll") -> Android build lib<foo>.so / <foo>.so next to the script or in lib/ ---
  state["__pccompat_so_candidates"] = [](std::string name, sol::this_state ts) {
    sol::state_view lua(ts);
    sol::table t = lua.create_table();
    int i = 1;
    for (auto& c : so_candidates(name)) t[i++] = c;
    return t;
  };
  state.safe_script(R"LUA(
local ok, ffi = pcall(require, 'ffi')
if ok and ffi and not ffi.__pccompat then
  local raw_load = ffi.load
  ffi.__pccompat = true
  local winsys = { kernel32 = 'c', user32 = 'c', msvcrt = 'c', ucrtbase = 'c', ws2_32 = 'c', winmm = 'c' }
  ffi.load = function(name, global)
    local ok1, lib = pcall(raw_load, name, global)
    if ok1 then return lib end
    local stem = tostring(name):gsub('\\', '/'):match('([^/]+)$') or tostring(name)
    local base = stem:lower():gsub('%.dll$', ''):gsub('%.asi$', '')
    if winsys[base] then return ffi.C end
    for _, c in ipairs(__pccompat_so_candidates(tostring(name))) do
      local ok2, l2 = pcall(raw_load, c, global)
      if ok2 then return l2 end
    end
    error(lib, 2)
  end
end
)LUA", sol::script_pass_on_error);
}

int luaopen_pccompat(lua_State* L)
{
  sol::state_view s { L };
  sol::table t = s.create_table();
  t["pcBase"] = static_cast<double>(PC_BASE);
  t["isPcAddress"] = [](std::uintptr_t a) { return is_pc_address(a); };
  // translate(address[, size]) -> mobile address (number) and widen flag, or nil when there is no counterpart.
  // Addresses outside the PC range come back unchanged. size 0 = function (call).
  t["translate"] = [](sol::this_state ts, std::uintptr_t a, std::optional<std::size_t> size) -> std::tuple<sol::object, bool> {
    auto r = translate(a, size.value_or(4));
    if (!r.ok) {
      log_once(a, size.value_or(4) ? "read returns 0, write skipped" : "call refused");
      return { sol::make_object(ts, sol::lua_nil), false };
    }
    return { sol::make_object(ts, static_cast<double>(r.address)), r.widen };
  };
  t["name"] = [](std::uintptr_t a) -> sol::optional<std::string> {
    const entry* e = find_entry(a);
    if (!e) {
      return sol::nullopt;
    }
    return std::string(e->name);
  };
  t["resolve"] = [](std::string name) { return static_cast<double>(resolve_name(name.c_str())); };
  t["stats"] = [](sol::this_state ts) {
    ensure_resolved();
    sol::state_view sv { ts };
    sol::table r = sv.create_table();
    r["entries"] = g_count;
    r["resolved"] = g_resolved_count;
    return r;
  };
  sol::stack::push(L, t);
  return 1;
}
}

void monetloader_set_vk(int vk, int down)
{
  lua::pccompat::set_virtual_key(vk, down != 0);
}
