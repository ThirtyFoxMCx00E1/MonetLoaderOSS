#pragma once
#include "raknet/BitStream.h"
#include "raknet/RakNetTypes.h"
#include "utils/callback.h"
#include <cstdint>
class RakClientInterface;

namespace rakhook {
void init(RakClientInterface* rci, std::uintptr_t recv_ignore_rpc);
bool available();
void process();

// MP Multiplayer (TYFMC MP): no SA-MP client, MP feeds SA-MP style RPCs through the same Lua path
namespace mp {
  using out_rpc_fn = void (*)(int id, RakNet::BitStream* bs);
  using out_packet_fn = void (*)(RakNet::BitStream* bs);
  using apply_rpc_fn = void (*)(std::uint8_t id, RakNet::BitStream* bs);
  void init(out_rpc_fn out_rpc, out_packet_fn out_packet, apply_rpc_fn apply_rpc);
  bool active();
  // Lua onReceiveRpc/onReceivePacket + netinfo. false = a script blocked it; *out gets the (maybe edited) RPC body
  bool incoming_rpc(std::uint8_t id, RakNet::BitStream* bs, RakNet::BitStream* out);
  bool incoming_packet(RakNet::BitStream* bs);
  void pump(); // emulated RPCs (sampAddChatMessage, sampShowDialog, ...) -> MP
}

namespace emu {
  void clear();
  void only(bool toggle);
  void packet(RakNet::BitStream* bs);
  void rpc(std::uint8_t id, RakNet::BitStream* bs = nullptr);
}

namespace send {
  void packet(RakNet::BitStream* bs, RakNet::PacketPriority priority = RakNet::HIGH_PRIORITY,
      RakNet::PacketReliability reliability = RakNet::UNRELIABLE_SEQUENCED, char ordering_channel = 0);
  void packet_invoke_handlers(RakNet::BitStream* bs, RakNet::PacketPriority priority = RakNet::HIGH_PRIORITY,
      RakNet::PacketReliability reliability = RakNet::UNRELIABLE_SEQUENCED, char ordering_channel = 0);
  void rpc(int id, RakNet::BitStream* bs = nullptr, RakNet::PacketPriority priority = RakNet::HIGH_PRIORITY,
      RakNet::PacketReliability reliability = RakNet::RELIABLE_ORDERED, char ordering_channel = 0);
  void rpc_invoke_handlers(int id, RakNet::BitStream* bs = nullptr, RakNet::PacketPriority priority = RakNet::HIGH_PRIORITY,
      RakNet::PacketReliability reliability = RakNet::RELIABLE_ORDERED, char ordering_channel = 0);
}
};