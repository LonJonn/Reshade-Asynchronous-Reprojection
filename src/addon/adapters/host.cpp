#include "addon/adapters/host.hpp"
#include "addon/adapters/xray.hpp"
#include "addon/diag.hpp"
#include "addon/streamline_hooks.hpp"
#include <reshade.hpp>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fw::adapters::host {
namespace {

using namespace reshade::api;

// The adapters there are. An entry: {id, name, matches, create}.
const std::vector<AdapterDescriptor>& table() {
    static const std::vector<AdapterDescriptor> adapters = {
        {"xray", "X-Ray (S.T.A.L.K.E.R. Anomaly)", xray_matches, xray_create},
    };
    return adapters;
}

HostCore g_core;
std::mutex g_mutex;  // (events come from the game's threads; only ever taken when the game has an adapter)
std::atomic<std::uint64_t> g_immediate{0};  // Direct3D 11: the immediate context's command list (from the present)
bool g_registered = false;
HudSink g_hud_sink;  // (the producer's entry point for kCapHud - addon.cpp)
Health::State g_logged_health = Health::kProbation;
// The state the events are told with (under g_mutex): the swap chain's back buffers, each blend state's kind and whether it
// writes colour, and per context the blend state and pixel shader bound.
std::unordered_set<std::uint64_t> g_back_buffers;
struct BlendInfo {
    Blend blend = Blend::kNone;
    bool writes_colour = true;
};
std::unordered_map<std::uint64_t, BlendInfo> g_blend_states;
struct ContextState {
    BlendInfo blend;
    std::uint64_t pixel_shader = 0;
};
std::unordered_map<std::uint64_t, ContextState> g_contexts;

std::string config(const std::string& key) {
    char value[64]{};
    size_t size = sizeof(value);
    if (!reshade::get_config_value(nullptr, "FrameWarp", key.c_str(), value, &size)) return {};
    return value;
}

// An adapter's code runs under a structured exception guard: a fault inside one lets that adapter go for the run, the
// game carries on (fault_log's handler still notes the first-chance fault). No C++ objects in here (C2712).
struct Call { const AdapterEvent* event; };
void call_core(void* arg) { g_core.on_event(*static_cast<Call*>(arg)->event); }
bool guarded(void (*fn)(void*), void* arg) {
    __try {
        fn(arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// (under g_mutex) The adapter's trust changed: said in the log once per change.
void log_health() {
    const Health::State now = g_core.health();
    if (now == g_logged_health) return;
    g_logged_health = now;
    const char* what = now == Health::kHealthy ? "confirmed by the game - in use"
                     : now == Health::kDetached ? "contradicted by the game - let go for this run"
                                                : "being checked again before use";
    char text[200];
    std::snprintf(text, sizeof(text), "FrameWarp: game adapter %s %s", g_core.name() ? g_core.name() : "?", what);
    diag_log(now == Health::kDetached ? reshade::log::level::warning : reshade::log::level::info, text);
}

void dispatch(const AdapterEvent& event) {
    std::vector<HostCore::HudOut> hud;
    {
        std::lock_guard lock(g_mutex);
        if (!g_core.active() || g_core.health() == Health::kDetached) return;
        Call call{&event};
        if (!guarded(&call_core, &call)) {
            g_core.detach();
            char text[160];
            std::snprintf(text, sizeof(text), "FrameWarp: the game adapter %s faulted - let go for this run, FrameWarp carries on without it",
                          g_core.name() ? g_core.name() : "?");
            diag_log(reshade::log::level::warning, text);
            g_logged_health = Health::kDetached;
        }
        if (event.kind == EventKind::kPresent) log_health();
        hud = g_core.take_outputs();
    }
    // (outside the lock: the producer takes its own)
    if (g_hud_sink)
        for (const auto& h : hud) g_hud_sink(h.picture, static_cast<std::uint32_t>(h.kind), h.context);
}

AdapterEvent event_for(command_list* cmd, EventKind kind) {
    AdapterEvent e;
    e.kind = kind;
    e.context = cmd ? cmd->get_native() : 0;
    if (cmd && cmd->get_device()->get_api() == device_api::d3d11)
        e.deferred = g_immediate.load() != 0 && reinterpret_cast<std::uint64_t>(cmd) != g_immediate.load();
    e.streamline_frame = streamline_current_frame();
    return e;
}

TargetInfo target_of(device* dev, resource_view view) {
    TargetInfo t;
    if (!view.handle) return t;
    const resource res = dev->get_resource_from_view(view);
    if (!res.handle) return t;
    const resource_desc desc = dev->get_resource_desc(res);
    t.resource = res.handle;
    t.width = desc.texture.width;
    t.height = desc.texture.height;
    t.format = static_cast<std::uint32_t>(desc.texture.format);
    t.samples = desc.texture.samples;
    std::lock_guard lock(g_mutex);
    t.back_buffer = g_back_buffers.count(res.handle) != 0;
    return t;
}

void note_back_buffers(swapchain* chain) {
    if (!chain) return;
    std::lock_guard lock(g_mutex);
    g_back_buffers.clear();
    for (uint32_t i = 0; i < chain->get_back_buffer_count(); ++i) g_back_buffers.insert(chain->get_back_buffer(i).handle);
}

Blend blend_of(const blend_desc& b) {
    if (!b.blend_enable[0]) return Blend::kNone;
    const blend_factor s = b.source_color_blend_factor[0], d = b.dest_color_blend_factor[0];
    return (s == blend_factor::source_alpha || s == blend_factor::one) && d == blend_factor::one_minus_source_alpha ? Blend::kAlpha
                                                                                                                    : Blend::kOther;
}
void on_init_pipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline p) {
    if (!p.handle) return;
    for (uint32_t i = 0; i < count; ++i)
        if (subobjects[i].type == pipeline_subobject_type::blend_state && subobjects[i].data) {
            const auto& b = *static_cast<const blend_desc*>(subobjects[i].data);
            std::lock_guard lock(g_mutex);
            g_blend_states[p.handle] = {blend_of(b), b.render_target_write_mask[0] != 0};
        }
}
void on_destroy_pipeline(device*, pipeline p) {
    std::lock_guard lock(g_mutex);
    g_blend_states.erase(p.handle);
}
void on_bind_pipeline(command_list* cmd, pipeline_stage stages, pipeline p) {
    if (!cmd) return;
    const auto has = [stages](pipeline_stage s) { return (static_cast<uint32_t>(stages) & static_cast<uint32_t>(s)) != 0; };
    const bool om = has(pipeline_stage::output_merger), ps = has(pipeline_stage::pixel_shader);
    if (!om && !ps) return;
    std::lock_guard lock(g_mutex);
    ContextState& c = g_contexts[cmd->get_native()];
    if (om) {
        const auto it = g_blend_states.find(p.handle);
        c.blend = it != g_blend_states.end() ? it->second : BlendInfo{};
    }
    if (ps) c.pixel_shader = p.handle;
}

// Deferred contexts are passed on with e.deferred set: what they record reaches the GPU when the immediate context
// executes it, in an order the events do not show - each adapter decides whether that matters to what it looks for.
void on_bind_targets(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    AdapterEvent e = event_for(cmd, EventKind::kBindTargets);
    device* dev = cmd->get_device();
    e.target_count = count < 8 ? count : 8;
    for (uint32_t i = 0; i < e.target_count; ++i) e.targets[i] = target_of(dev, rtvs[i]);
    e.depth = target_of(dev, dsv);
    dispatch(e);
}
void on_viewports(command_list* cmd, uint32_t first, uint32_t count, const viewport* viewports) {
    if (first != 0 || count == 0) return;
    AdapterEvent e = event_for(cmd, EventKind::kViewport);
    e.min_depth = viewports[0].min_depth;
    e.max_depth = viewports[0].max_depth;
    dispatch(e);
}
void draw_event(command_list* cmd, uint32_t vertices) {
    AdapterEvent e = event_for(cmd, EventKind::kDraw);
    e.vertices = vertices;
    {
        std::lock_guard lock(g_mutex);
        const auto c = g_contexts.find(e.context);
        if (c != g_contexts.end()) {
            e.blend = c->second.blend.blend;
            e.writes_colour = c->second.blend.writes_colour;
            e.pixel_shader = c->second.pixel_shader;
        }
    }
    dispatch(e);
}
bool on_draw(command_list* cmd, uint32_t vertices, uint32_t, uint32_t, uint32_t) {
    draw_event(cmd, vertices);
    return false;  // (never skips the game's draw)
}
bool on_draw_indexed(command_list* cmd, uint32_t indices, uint32_t, uint32_t, int32_t, uint32_t) {
    draw_event(cmd, indices);
    return false;
}
// (registered after the add-on's own present event, so it runs before the add-on's ReShade present: what the adapter
// says at a present reaches the producer before it hands that frame over - Producer::begin_present_d3d11)
void on_present(command_queue* queue, swapchain* chain, const rect*, const rect*, uint32_t, const rect*) {
    if (queue && queue->get_device()->get_api() == device_api::d3d11)
        g_immediate = reinterpret_cast<std::uint64_t>(queue->get_immediate_command_list());
    if (chain && chain->get_back_buffer_count()) {  // (looked at again only when the first back buffer is a new one)
        bool known;
        {
            std::lock_guard lock(g_mutex);
            known = g_back_buffers.count(chain->get_back_buffer(0).handle) != 0;
        }
        if (!known) note_back_buffers(chain);
    }
    AdapterEvent e;
    e.kind = EventKind::kPresent;
    e.streamline_frame = streamline_current_frame();
    dispatch(e);
}
void on_destroy_resource(device*, resource res) {
    {
        std::lock_guard lock(g_mutex);
        g_back_buffers.erase(res.handle);
    }
    AdapterEvent e;
    e.kind = EventKind::kDestroyResource;
    e.resource = res.handle;
    dispatch(e);
}
// A new swap chain or a resize (a resolution or upscaler change): what the adapter learned about the frame starts over.
void on_init_swapchain(swapchain* chain, bool) {
    note_back_buffers(chain);
    std::lock_guard lock(g_mutex);
    g_core.reset();
}

class ReShadeRegistrar final : public Registrar {
public:
    void register_events() override {
        reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
        reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_targets);
        reshade::register_event<reshade::addon_event::bind_viewports>(on_viewports);
        reshade::register_event<reshade::addon_event::draw>(on_draw);
        reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
        reshade::register_event<reshade::addon_event::present>(on_present);
        reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        g_registered = true;
    }
    void unregister_events() override {
        if (!g_registered) return;
        reshade::unregister_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
        reshade::unregister_event<reshade::addon_event::present>(on_present);
        reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
        reshade::unregister_event<reshade::addon_event::draw>(on_draw);
        reshade::unregister_event<reshade::addon_event::bind_viewports>(on_viewports);
        reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_targets);
        reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
        reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
        reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
        g_registered = false;
    }
};
ReShadeRegistrar g_registrar;

}  // namespace

void init(HudSink hud_sink) {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::lock_guard lock(g_mutex);
    g_hud_sink = std::move(hud_sink);
    g_logged_health = Health::kProbation;
    g_core.init(table(), GameInfo{exe}, config, g_registrar);
    char text[200];
    if (g_core.active())
        std::snprintf(text, sizeof(text), "FrameWarp: game adapter %s (capabilities 0x%x, switched on 0x%x) - checking it before use",
                      g_core.name(), g_core.offered(), g_core.enabled());
    else
        std::snprintf(text, sizeof(text), "FrameWarp: no game adapter for this game - nothing registered for one");
    diag_log(reshade::log::level::info, text);
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    g_core.shutdown();
    g_hud_sink = nullptr;
    g_back_buffers.clear();
    g_blend_states.clear();
    g_contexts.clear();
}

bool gives(Capability cap) {
    std::lock_guard lock(g_mutex);
    return g_core.gives(cap);
}

Status status() {
    std::lock_guard lock(g_mutex);
    Status s;
    s.name = g_core.name();
    s.health = g_core.health();
    s.offered = g_core.offered();
    s.enabled = g_core.enabled();
    return s;
}

}  // namespace fw::adapters::host
