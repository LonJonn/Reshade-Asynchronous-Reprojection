#pragma once
// The game adapters' host inside the game (adapter.hpp, host_logic.hpp): picks the game's adapter when the add-on loads
// and, only if there is one, registers the ReShade events it needs and passes them on as plain values.
#include "addon/adapters/adapter.hpp"
#include "addon/adapters/host_logic.hpp"

namespace fw::adapters::host {

void init();
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
