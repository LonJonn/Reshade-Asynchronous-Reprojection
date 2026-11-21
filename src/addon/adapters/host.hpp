#pragma once
// The game adapters' host inside the game (adapter.hpp, host_logic.hpp): picks the game's adapter when the add-on loads
// and, only if there is one, registers the ReShade events it needs and passes them on as plain values.
#include "addon/adapters/adapter.hpp"
#include "addon/adapters/host_logic.hpp"
#include <functional>

namespace fw::adapters::host {

// Where kCapHud's outputs go: (picture, kind, context) as Producer::on_hud_before_d3d11 takes them (HudBefore's values).
// Called on the game's thread, outside the host's lock, only while the adapter is trusted and the capability on.
using HudSink = std::function<void(std::uint64_t picture, std::uint32_t kind, std::uint64_t context)>;

void init(HudSink hud_sink);
void shutdown();
// A capability to be used now (offered, switched on, the adapter trusted).
bool gives(Capability cap);

struct Status {
    const char* name = nullptr;  // nullptr: no adapter for this game
    Health::State health = Health::kProbation;
    std::uint32_t offered = 0, enabled = 0;
};
Status status();

}  // namespace fw::adapters::host
