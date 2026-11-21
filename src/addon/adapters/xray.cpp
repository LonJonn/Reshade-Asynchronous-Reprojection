#include "addon/adapters/xray.hpp"
#include "addon/adapters/host_logic.hpp"

namespace fw::adapters {

void XRayAdapter::on_event(const AdapterEvent& e, Output& out) {
    switch (e.kind) {
    case EventKind::kBindTargets: {
        Bound& b = bound_[e.context];
        b.target = e.target_count ? e.targets[0].resource : 0;
        b.back_buffer = e.target_count && e.targets[0].back_buffer;
        break;
    }
    case EventKind::kDraw: drawn(e, out); break;
    case EventKind::kPresent: presented(out); break;
    case EventKind::kDestroyResource: steps_.erase(e.resource); break;
    case EventKind::kViewport: break;
    }
}

void XRayAdapter::drawn(const AdapterEvent& e, Output& out) {
    if (!e.writes_colour) return;  // (an occlusion query: not the picture, not the HUD)
    const auto b = bound_.find(e.context);
    if (b == bound_.end() || !b->second.back_buffer || !b->second.target) return;
    const std::uint64_t bb = b->second.target;
    Steps& s = steps_[bb];
    if (e.blend == Blend::kNone) {
        if (s.step == 2) {
            // (after the copy, an overwriting draw by the picture's shader: the picture drawn again - the copy given up; by
            // another shader: an opaque part of the HUD)
            if (e.pixel_shader == s.picture_shader) {
                s.step = 3;
                gave_up_ = true;
                out.hud_before_d3d11(e.context, bb, HudBefore::kGivenUp);
            }
        } else if (s.step != 3 && e.vertices <= kMaxPictureVertices) {
            s.step = 1;
            s.picture_shader = e.pixel_shader;
        }
        return;
    }
    if (e.blend != Blend::kAlpha || s.step != 1) return;
    s.step = 2;
    seen_ = true;
    hud_context_ = e.context;
    took_ = true;
    out.hud_before_d3d11(e.context, bb, HudBefore::kCopyNow);
}

void XRayAdapter::presented(Output& out) {
    // (a back buffer the picture was drawn onto with no HUD after it: no HUD this frame - once the HUD has been seen drawn
    // this way at all, so a game that never draws one like this is never told "no HUD")
    std::uint64_t no_hud = 0;
    for (const auto& [bb, s] : steps_)
        if (s.step == 1 && seen_ && hud_context_) no_hud = bb;
    steps_.clear();
    if (no_hud) {
        none_ = true;
        out.hud_before_d3d11(hud_context_, no_hud, HudBefore::kNoHudThisFrame);
    }
}

Verdict XRayAdapter::end_frame() {
    const Verdict v = gave_up_ ? Verdict::kContradicted : (took_ || none_) ? Verdict::kConfirmed : Verdict::kNotSeen;
    took_ = gave_up_ = none_ = false;
    return v;
}

void XRayAdapter::reset() {
    bound_.clear();
    steps_.clear();
    seen_ = false;
    hud_context_ = 0;
    took_ = gave_up_ = none_ = false;
}

// S.T.A.L.K.E.R. Anomaly's Direct3D 11 executables (AnomalyDX11.exe, AnomalyDX11AVX.exe; the modded ones keep the names).
bool xray_matches(const GameInfo& game) { return exe_name(game.exe_path).rfind(L"anomalydx11", 0) == 0; }
std::unique_ptr<GameAdapter> xray_create() { return std::make_unique<XRayAdapter>(); }

}  // namespace fw::adapters
