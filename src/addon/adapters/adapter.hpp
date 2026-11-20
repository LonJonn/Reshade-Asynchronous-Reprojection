#pragma once
// Game adapters: what FrameWarp can be told about one engine, as evidence for the paths that find things by themselves.
// An adapter only classifies what the game does (which draws are the first-person ones, where its 2D UI starts, which
// targets hold its G-buffer); everything it says goes into the existing entry points and is checked there like any other
// source (a HUD by layer_trust, a camera by game_camera_check, a character mask by character_source). The producer does
// every copy. Without an adapter for the game nothing here is registered with ReShade (host.cpp).
//
// Plain values only (no ReShade types): events recorded in a game can be replayed into an adapter in the tests.
#include <cstdint>
#include <memory>
#include <string>

namespace fw::adapters {

// What an adapter can tell. Each one is used only while the adapter is healthy (host_logic.hpp) and its switch is on
// ([FrameWarp] Adapter<Name>=0 turns one off).
enum Capability : std::uint32_t {
    kCapHud = 1u << 0,          // where the game starts drawing its 2D UI
    kCapFirstPerson = 1u << 1,  // which draws are the first-person hands / weapon
    kCapSceneBehind = 1u << 2,  // ...and the scene's G-buffer just before them (the world behind the hands)
    kCapOverlays = 1u << 3,     // screen-fixed overlays (a gas mask's cracks, a scope's mask)
    kCapabilityCount = 4,
};
inline const char* capability_name(int bit) {
    static const char* names[kCapabilityCount] = {"Hud", "FirstPerson", "SceneBehind", "Overlays"};
    return bit >= 0 && bit < kCapabilityCount ? names[bit] : "?";
}

struct GameInfo {
    std::wstring exe_path;  // the game's executable, full path
};

// One render target or depth-stencil as an event saw it (0 resource: none bound).
struct TargetInfo {
    std::uint64_t resource = 0;
    std::uint32_t width = 0, height = 0, format = 0, samples = 0;
};

enum class EventKind : std::uint8_t {
    kBindTargets,      // render targets + depth-stencil bound (targets, target_count, depth)
    kViewport,         // viewport 0 set (min_depth, max_depth)
    kDraw,             // a draw (vertices: vertex or index count)
    kPresent,          // the game presented a frame: the adapter gives its verdict for the frame (GameAdapter::end_frame)
    kDestroyResource,  // a resource went away (resource): forget it, its handle may come back for another one
};

struct AdapterEvent {
    EventKind kind = EventKind::kDraw;
    std::uint64_t context = 0;  // the command list / device context (native handle)
    bool deferred = false;      // Direct3D 11: a deferred context (the host passes none on for now)
    std::uint32_t target_count = 0;
    TargetInfo targets[8];
    TargetInfo depth;
    float min_depth = 0, max_depth = 1;
    std::uint32_t vertices = 0;
    std::uint64_t resource = 0;
    std::uint64_t streamline_frame = 0;  // the Streamline frame token at the event (0: none)
};

// An adapter's verdict on one frame: whether what it claims was seen and held (confirmed), seen and broken
// (contradicted: e.g. its first-person draws no longer squashed into the near depth), or not seen at all - a menu,
// a loading screen or a holstered weapon is not a failure.
enum class Verdict : std::uint8_t { kNotSeen, kConfirmed, kContradicted };

class GameAdapter {
public:
    virtual ~GameAdapter() = default;
    virtual std::uint32_t capabilities() const = 0;
    virtual void on_event(const AdapterEvent& event) = 0;
    // At the game's present (after its kPresent event): this frame's verdict.
    virtual Verdict end_frame() = 0;
    // The device, its swap chain or the game's resolution changed: forget what was learned about the frame's layout.
    virtual void reset() {}
};

struct AdapterDescriptor {
    const char* id;    // the value of [FrameWarp] Adapter=<id>
    const char* name;  // for the panel and the log
    bool (*matches)(const GameInfo& game);  // a cheap static check (the exe's name, files next to it)
    std::unique_ptr<GameAdapter> (*create)();
};

}  // namespace fw::adapters
