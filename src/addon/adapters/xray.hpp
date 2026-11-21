#pragma once
// The X-Ray engine adapter (S.T.A.L.K.E.R. Anomaly, Direct3D 11 renderer). Plain values only (adapter.hpp): the tests
// replay event sequences into it.
//
// HUD (kCapHud): X-Ray draws the finished picture onto the back buffer with one full-screen draw (6 vertices or fewer, no
// blending), then alpha-blends its HUD straight onto it, a depth buffer bound all along. FrameWarp's generic detectors
// do not cover that on Direct3D 11 (upstream's fourth pattern is Direct3D 12 only and, to tell a HUD from transparent 3D
// drawn into the back buffer, wants no depth buffer bound - X-Ray binds one), so this adapter tells the producer:
// - the first alpha-blended draw after the picture draw: the HUD's turn - copy the back buffer now (HudBefore::kCopyNow);
// - the picture's pixel shader drawing again after that copy: the copy is not HUD-less after all (kGivenUp);
// - a picture draw and no HUD before the present: the back buffer itself is HUD-less (kNoHudThisFrame).
// Draws writing no colour (X-Ray's occlusion queries draw onto the back buffer that way) are left out.
// Verdict per frame: confirmed when the sequence held (a copy kept, or no HUD), contradicted when a copy had to be given
// up, not seen otherwise (menus, loading screens).
#include "addon/adapters/adapter.hpp"
#include <unordered_map>

namespace fw::adapters {

class XRayAdapter final : public GameAdapter {
public:
    static constexpr std::uint32_t kMaxPictureVertices = 6;  // (a full-screen quad as two triangles, or fewer)

    std::uint32_t capabilities() const override { return kCapHud; }
    void on_event(const AdapterEvent& event, Output& out) override;
    Verdict end_frame() override;
    void reset() override;

private:
    struct Bound {
        std::uint64_t target = 0;  // render target 0 (0: none)
        bool back_buffer = false;
    };
    // Per back buffer, this frame: 0 nothing yet, 1 the picture drawn, 2 the HUD started (copy taken), 3 given up.
    struct Steps {
        std::uint8_t step = 0;
        std::uint64_t picture_shader = 0;
    };
    void drawn(const AdapterEvent& e, Output& out);
    void presented(Output& out);

    std::unordered_map<std::uint64_t, Bound> bound_;  // per context
    std::unordered_map<std::uint64_t, Steps> steps_;  // per back buffer
    bool seen_ = false;               // the HUD has been seen drawn this way (before that, "no HUD" is never said)
    std::uint64_t hud_context_ = 0;   // the context the HUD was last drawn on
    bool took_ = false, gave_up_ = false, none_ = false;  // this frame
};

bool xray_matches(const GameInfo& game);
std::unique_ptr<GameAdapter> xray_create();

}  // namespace fw::adapters
