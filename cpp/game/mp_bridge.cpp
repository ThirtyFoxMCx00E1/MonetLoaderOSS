// MonetLoader for MP - the MonetLoader side of MP's Lua bridge (mp_lua_bridge.h).
// MP's network events become the SA-MP RPCs MoonLoader scripts know and run through rakhook's normal in-path
// (onReceiveRpc / samp.events + netinfo); everything scripts send (sampSendChat, sampSendDialogResponse, ...)
// comes back to MP. Scripts keep using the PC API: sampAddChatMessage, sampGetPlayerNickname, sampRegisterChatCommand ...
#include "mp_bridge.h"
#include "mp_lua_bridge.h"
#include "game/CPools.h"
#include "game/netinfo.h"
#include "game/rakhook.h"
#include "hook/monethook.h"
#include "raknet/BitStream.h"
#include "raknet/RakNetTypes.h"
#include "raknet/StringCompressor.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace {
MpLuaBridge* g_mp;
bool g_given_up;
int g_tries;
bool g_was_connected;
std::chrono::steady_clock::time_point g_last_sync;

#if defined(__aarch64__)
const char* MP_LIB = "libMP64.so";
#else
const char* MP_LIB = "libMP32.so";
#endif

std::string read_str8(RakNet::BitStream* bs)
{
  std::uint8_t len = 0;
  if (!bs->Read(len)) {
    return {};
  }
  std::string s(len, '\0');
  if (len) {
    bs->Read(s.data(), len);
  }
  return s;
}
std::string read_str32(RakNet::BitStream* bs, std::uint32_t max = 4095)
{
  std::uint32_t len = 0;
  if (!bs->Read(len)) {
    return {};
  }
  len = std::min(len, max);
  std::string s(len, '\0');
  if (len) {
    bs->Read(s.data(), static_cast<int>(len));
  }
  return s;
}
void write_str8(RakNet::BitStream& bs, const char* s)
{
  std::uint8_t len = static_cast<std::uint8_t>(std::min<std::size_t>(std::strlen(s), 255));
  bs.Write(len);
  bs.Write(s, len);
}
void write_str32(RakNet::BitStream& bs, const char* s)
{
  std::uint32_t len = static_cast<std::uint32_t>(std::strlen(s));
  bs.Write(len);
  bs.Write(s, static_cast<int>(len));
}
void copy(char* dst, const std::string& s, std::size_t n)
{
  std::size_t l = std::min(s.size(), n - 1);
  std::memcpy(dst, s.data(), l);
  dst[l] = 0;
}

// ---- MP -> scripts ----------------------------------------------------------------------------
void emit_connected(int id, const char* name, const char* server, const char* host, int port)
{
  game::netinfo::address = host ? host : "";
  game::netinfo::port = static_cast<std::uint16_t>(port);
  game::netinfo::local_player.name = name ? name : "";

  RakNet::BitStream pk; // ID_CONNECTION_REQUEST_ACCEPTED: ip, port, player index (netinfo reads it at 8+32+16)
  pk.Write<std::uint8_t>(RakNet::ID_CONNECTION_REQUEST_ACCEPTED);
  pk.Write<std::uint32_t>(0);
  pk.Write<std::uint16_t>(static_cast<std::uint16_t>(port));
  pk.Write<std::uint16_t>(static_cast<std::uint16_t>(id));
  pk.Write<std::uint32_t>(0);
  rakhook::mp::incoming_packet(&pk);
  game::netinfo::local_player_id = static_cast<std::uint16_t>(id);
  game::netinfo::max_id = static_cast<std::uint16_t>(id);

  RakNet::BitStream ig; // InitGame: everything zero up to the host name (bit 395, netinfo's offset)
  for (int i = 0; i < 395; ++i) {
    ig.Write0();
  }
  write_str8(ig, server ? server : "MP");
  rakhook::mp::incoming_rpc(RakNet::RPC_ScrInitGame, &ig, nullptr);
  game::netinfo::hostname = server ? server : "MP";
}
void emit_disconnected()
{
  RakNet::BitStream pk;
  pk.Write<std::uint8_t>(RakNet::ID_CONNECTION_LOST);
  rakhook::mp::incoming_packet(&pk);
  game::netinfo::mp_reset();
}
void emit_join(int id, const char* name, std::uint32_t color)
{
  RakNet::BitStream bs;
  bs.Write<std::uint16_t>(static_cast<std::uint16_t>(id));
  bs.Write<std::uint32_t>(color);
  bs.Write<std::uint8_t>(0);
  write_str8(bs, name);
  rakhook::mp::incoming_rpc(RakNet::RPC_ScrServerJoin, &bs, nullptr);
}
void emit_quit(int id, int reason)
{
  game::netinfo::mp_stream_out(static_cast<std::uint16_t>(id));
  RakNet::BitStream bs;
  bs.Write<std::uint16_t>(static_cast<std::uint16_t>(id));
  bs.Write<std::uint8_t>(static_cast<std::uint8_t>(reason));
  rakhook::mp::incoming_rpc(RakNet::RPC_ScrServerQuit, &bs, nullptr);
  game::netinfo::remote_players.erase(static_cast<std::uint16_t>(id));
}

int sink(MpLuaEvent* ev, void*)
{
  switch (ev->kind) {
  case MPL_CONNECTED:
    emit_connected(ev->id, ev->name, ev->text, ev->caption, ev->a);
    g_was_connected = true;
    return 1;
  case MPL_DISCONNECTED:
    if (g_was_connected) {
      emit_disconnected();
    }
    g_was_connected = false;
    return 1;
  case MPL_PLAYER_JOIN:
    emit_join(ev->id, ev->name, ev->color);
    return 1;
  case MPL_PLAYER_QUIT:
    emit_quit(ev->id, ev->a);
    return 1;
  case MPL_CLIENT_MESSAGE: {
    RakNet::BitStream bs, out;
    bs.Write<std::uint32_t>(ev->color);
    write_str32(bs, ev->text);
    if (!rakhook::mp::incoming_rpc(RakNet::RPC_ScrClientMessage, &bs, &out)) {
      return 0;
    }
    std::uint32_t color = ev->color;
    out.Read(color);
    ev->color = color;
    copy(ev->text, read_str32(&out), sizeof(ev->text));
    return 1;
  }
  case MPL_PLAYER_CHAT: {
    RakNet::BitStream bs, out;
    bs.Write<std::uint16_t>(static_cast<std::uint16_t>(ev->id));
    write_str8(bs, ev->text);
    if (!rakhook::mp::incoming_rpc(RakNet::RPC_Chat, &bs, &out)) {
      return 0;
    }
    out.IgnoreBits(16);
    copy(ev->text, read_str8(&out), sizeof(ev->text));
    return 1;
  }
  case MPL_DIALOG: {
    RakNet::BitStream bs, out;
    bs.Write<std::int16_t>(static_cast<std::int16_t>(ev->a));
    bs.Write<std::uint8_t>(static_cast<std::uint8_t>(ev->b));
    write_str8(bs, ev->caption);
    write_str8(bs, ev->button1);
    write_str8(bs, ev->button2);
    RakNet::StringCompressor::Instance()->EncodeString(ev->text, static_cast<int>(std::strlen(ev->text) + 1), &bs);
    if (!rakhook::mp::incoming_rpc(RakNet::RPC_ScrShowDialog, &bs, &out)) {
      return 0;
    }
    std::int16_t id = 0;
    std::uint8_t style = 0;
    out.Read(id);
    out.Read(style);
    ev->a = id;
    ev->b = style;
    copy(ev->caption, read_str8(&out), sizeof(ev->caption));
    copy(ev->button1, read_str8(&out), sizeof(ev->button1));
    copy(ev->button2, read_str8(&out), sizeof(ev->button2));
    RakNet::StringCompressor::Instance()->DecodeString(ev->text, sizeof(ev->text), &out);
    return 1;
  }
  case MPL_GAMETEXT: {
    RakNet::BitStream bs, out;
    bs.Write<std::int32_t>(ev->a);
    bs.Write<std::int32_t>(ev->b);
    write_str32(bs, ev->text);
    if (!rakhook::mp::incoming_rpc(RakNet::RPC_ScrDisplayGameText, &bs, &out)) {
      return 0;
    }
    std::int32_t style = ev->a, time = ev->b;
    out.Read(style);
    out.Read(time);
    ev->a = style;
    ev->b = time;
    copy(ev->text, read_str32(&out), sizeof(ev->text));
    return 1;
  }
  }
  return 1;
}

// ---- the player's own chat / dialog answers from MP's UI ------------------------------------
int out_chat(const char* text, void*)
{
  std::size_t n = std::strlen(text);
  RakNet::BitStream bs;
  if (text[0] == '/') {
    bs.Write<std::uint32_t>(static_cast<std::uint32_t>(n));
    bs.Write(text, static_cast<int>(n));
    rakhook::send::rpc_invoke_handlers(RakNet::RPC_ServerCommand, &bs);
  } else {
    n = std::min<std::size_t>(n, 255);
    bs.Write<std::uint8_t>(static_cast<std::uint8_t>(n));
    bs.Write(text, static_cast<int>(n));
    rakhook::send::rpc_invoke_handlers(RakNet::RPC_Chat, &bs);
  }
  return 1;
}
int out_dialog(int id, int button, int item, const char* input, void*)
{
  RakNet::BitStream bs;
  bs.Write<std::int16_t>(static_cast<std::int16_t>(id));
  bs.Write<std::uint8_t>(static_cast<std::uint8_t>(button));
  bs.Write<std::int16_t>(static_cast<std::int16_t>(item));
  write_str8(bs, input);
  rakhook::send::rpc_invoke_handlers(RakNet::RPC_DialogResponse, &bs);
  return 1;
}

// ---- scripts -> MP ----------------------------------------------------------------------------
void mp_out_rpc(int id, RakNet::BitStream* bs)
{
  if (!g_mp) {
    return;
  }
  bs->ResetReadPointer();
  if (id == RakNet::RPC_Chat) {
    std::string t = read_str8(bs);
    g_mp->SendChatRaw(t.c_str());
  } else if (id == RakNet::RPC_ServerCommand) {
    std::string t = read_str32(bs, 255);
    g_mp->SendChatRaw(t.c_str());
  } else if (id == RakNet::RPC_DialogResponse) {
    std::int16_t did = 0, item = -1;
    std::uint8_t button = 0;
    bs->Read(did);
    bs->Read(button);
    bs->Read(item);
    std::string t = read_str8(bs);
    g_mp->DialogResponseRaw(did, button, item, t.c_str());
  } else if (id == RakNet::RPC_UpdateScoresPingsIPs) {
    // the scoreboard answer, straight from MP
    RakNet::BitStream reply;
    const int maxp = g_mp->MaxPlayers();
    for (int i = 0; i < maxp; ++i) {
      int score = 0, ping = 0;
      if (g_mp->PlayerInfo(i, nullptr, 0, nullptr, &score, &ping, nullptr)) {
        reply.Write<std::uint16_t>(static_cast<std::uint16_t>(i));
        reply.Write<std::int32_t>(score);
        reply.Write<std::int32_t>(ping);
      }
    }
    rakhook::emu::rpc(RakNet::RPC_UpdateScoresPingsIPs, &reply);
  }
  // spawn / class / sync style RPCs have no MP equivalent: MP's own sync handles them
}
void mp_out_packet(RakNet::BitStream*)
{
  // sync packets (sampSendOnfootData ...) are not used on MP servers: MP syncs players itself
}
// emulated RPCs (sampAddChatMessage, sampShowDialog, raknetEmulRpcReceiveBitStream ...) shown by MP
void mp_apply_rpc(std::uint8_t id, RakNet::BitStream* bs)
{
  if (!g_mp) {
    return;
  }
  if (id == RakNet::RPC_ScrClientMessage) {
    std::uint32_t color = 0xFFFFFFFF;
    bs->Read(color);
    std::string t = read_str32(bs);
    g_mp->AddChatMessage(t.c_str(), color);
  } else if (id == RakNet::RPC_ScrShowDialog) {
    std::int16_t did = 0;
    std::uint8_t style = 0;
    bs->Read(did);
    bs->Read(style);
    std::string cap = read_str8(bs), b1 = read_str8(bs), b2 = read_str8(bs);
    static char info[4096];
    info[0] = 0;
    RakNet::StringCompressor::Instance()->DecodeString(info, sizeof(info), bs);
    g_mp->ShowDialog(did, style, cap.c_str(), info, b1.c_str(), b2.c_str());
  } else if (id == RakNet::RPC_ScrDisplayGameText) {
    std::int32_t style = 1, time = 3000;
    bs->Read(style);
    bs->Read(time);
    std::string t = read_str32(bs);
    g_mp->GameText(t.c_str(), time, style);
  } else if (id == RakNet::RPC_Chat) {
    std::uint16_t pid = 0;
    bs->Read(pid);
    std::string t = read_str8(bs);
    char name[32] = "";
    std::uint32_t color = 0xFFFFFFFF;
    g_mp->PlayerInfo(pid, name, sizeof(name), &color, nullptr, nullptr, nullptr);
    std::string line = std::string(name) + ": " + t;
    g_mp->AddChatMessage(line.c_str(), 0xFFFFFFFF);
  }
}

// ---- finding MP -------------------------------------------------------------------------------
MpLuaBridge* find_mp()
{
  void* h = dlopen(MP_LIB, RTLD_NOW | RTLD_NOLOAD);
  if (!h) {
    monethook::library_map::snapshot();
    for (auto& i : monethook::library_map::get()) {
      if (i.first.find(std::string("/") + MP_LIB) != std::string::npos) {
        h = dlopen(i.first.c_str(), RTLD_NOW | RTLD_NOLOAD);
        if (h) {
          break;
        }
      }
    }
  }
  if (!h) {
    return nullptr;
  }
  auto get = reinterpret_cast<void* (*)()>(dlsym(h, "MP_GetLuaBridge"));
  dlclose(h); // NOLOAD handle: MP stays loaded
  if (!get) {
    return nullptr;
  }
  auto* b = static_cast<MpLuaBridge*>(get());
  if (!b || b->version != MP_LUA_BRIDGE_VERSION) {
    spdlog::warn("MP found but the Lua bridge version differs ({} vs {}), update MP and MonetLoader for MP together", b ? b->version : -1, MP_LUA_BRIDGE_VERSION);
    return nullptr;
  }
  return b;
}

// players MP has that the scripts were not told about (scripts loaded mid-game, missed events) and ped streaming
void sync_players()
{
  const bool conn = g_mp->IsConnected() != 0;
  if (conn != g_was_connected) {
    if (conn) {
      char name[32] = "", server[64] = "", host[128] = "";
      int port = 0;
      g_mp->LocalName(name, sizeof(name));
      g_mp->ServerName(server, sizeof(server));
      g_mp->ServerAddress(host, sizeof(host), &port);
      emit_connected(g_mp->LocalId(), name, server, host, port);
    } else {
      emit_disconnected();
    }
    g_was_connected = conn;
  }
  if (!conn) {
    return;
  }

  const int me = g_mp->LocalId();
  const int maxp = g_mp->MaxPlayers();
  for (int i = 0; i < maxp; ++i) {
    char name[32] = "";
    std::uint32_t color = 0;
    int score = 0, ping = 0;
    void* ped = nullptr;
    const bool on = g_mp->PlayerInfo(i, name, sizeof(name), &color, &score, &ping, &ped) != 0;
    if (i == me) {
      if (on) {
        game::netinfo::local_player.name = name;
        game::netinfo::local_player.color = color;
        game::netinfo::local_player.score = score;
        game::netinfo::local_player.ping = ping;
        game::netinfo::local_player.streamed_in = ped != nullptr;
      }
      continue;
    }
    auto it = game::netinfo::remote_players.find(static_cast<std::uint16_t>(i));
    if (on && it == game::netinfo::remote_players.end()) {
      emit_join(i, name, color);
      it = game::netinfo::remote_players.find(static_cast<std::uint16_t>(i));
    } else if (!on && it != game::netinfo::remote_players.end()) {
      emit_quit(i, 1);
      continue;
    }
    if (!on || it == game::netinfo::remote_players.end()) {
      continue;
    }
    it->second.name = name;
    it->second.color = color;
    it->second.score = score;
    it->second.ping = ping;
    CPed* p = static_cast<CPed*>(ped);
    if (p && game::netinfo::find_ped(static_cast<std::uint16_t>(i)) != p) {
      if (game::netinfo::mp_is_streamed(static_cast<std::uint16_t>(i))) {
        game::netinfo::mp_stream_out(static_cast<std::uint16_t>(i));
      }
      game::netinfo::mp_stream_in(static_cast<std::uint16_t>(i), p);
    } else if (!p && game::netinfo::mp_is_streamed(static_cast<std::uint16_t>(i))) {
      game::netinfo::mp_stream_out(static_cast<std::uint16_t>(i));
    }
  }
}
}

bool mp_bridge::active()
{
  return g_mp != nullptr;
}

void mp_bridge::process()
{
  if (!g_mp) {
    if (g_given_up || rakhook::available()) { // real SA-MP: MonetLoader works as usual
      return;
    }
    if ((++g_tries % 30) != 1) {
      return;
    }
    g_mp = find_mp();
    if (!g_mp) {
      if (g_tries > 30 * 120) {
        g_given_up = true; // ~2 min of frames without MP: singleplayer MonetLoader
      }
      return;
    }
    rakhook::mp::init(mp_out_rpc, mp_out_packet, mp_apply_rpc);
    g_mp->SetHandlers(sink, out_chat, out_dialog, nullptr);
    spdlog::info("MonetLoader for MP: attached to MP {} - PC MoonLoader scripts run on MP servers", g_mp->Version());
  }

  rakhook::mp::pump();
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_sync >= std::chrono::milliseconds(250)) {
    g_last_sync = now;
    sync_players();
  }
}
