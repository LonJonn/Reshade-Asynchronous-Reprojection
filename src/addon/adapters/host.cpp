#include "addon/adapters/host.hpp"
#include "addon/diag.hpp"
#include "addon/streamline_hooks.hpp"
#include <reshade.hpp>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace fw::adapters::host {
namespace {

using namespace reshade::api;

// The adapters there are (none yet). An entry: {id, name, matches, create}.
const std::vector<AdapterDescriptor>& table() {
    static const std::vector<AdapterDescriptor> adapters;
    return adapters;
}

HostCore g_core;
std::mutex g_mutex;  // (events come from the game's threads; only ever taken when the game has an adapter)
std::atomic<std::uint64_t> g_immediate{0};  // Direct3D 11: the immediate context's command list (from the present)
bool g_registered = false;

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

void dispatch(const AdapterEvent& event) {
    std::lock_guard lock(g_mutex);
    if (!g_core.active() || g_core.health() == Health::kDetached) return;
    Call call{&event};
    if (!guarded(&call_core, &call)) {
        g_core.detach();
        char text[160];
        std::snprintf(text, sizeof(text), "FrameWarp: the game adapter %s faulted - let go for this run, FrameWarp carries on without it",
                      g_core.name() ? g_core.name() : "?");
        diag_log(reshade::log::level::warning, text);
    }
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
    return t;
}

// Deferred contexts are not passed on for now (as Streamline tags on them are not taken): what they record reaches the
// GPU later, in an order the events do not show.
void on_bind_targets(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    AdapterEvent e = event_for(cmd, EventKind::kBindTargets);
    if (e.deferred) return;
    device* dev = cmd->get_device();
    e.target_count = count < 8 ? count : 8;
    for (uint32_t i = 0; i < e.target_count; ++i) e.targets[i] = target_of(dev, rtvs[i]);
    e.depth = target_of(dev, dsv);
    dispatch(e);
}
void on_viewports(command_list* cmd, uint32_t first, uint32_t count, const viewport* viewports) {
    if (first != 0 || count == 0) return;
    AdapterEvent e = event_for(cmd, EventKind::kViewport);
    if (e.deferred) return;
    e.min_depth = viewports[0].min_depth;
    e.max_depth = viewports[0].max_depth;
    dispatch(e);
}
bool on_draw(command_list* cmd, uint32_t vertices, uint32_t, uint32_t, uint32_t) {
    AdapterEvent e = event_for(cmd, EventKind::kDraw);
    if (!e.deferred) { e.vertices = vertices; dispatch(e); }
    return false;  // (never skips the game's draw)
}
bool on_draw_indexed(command_list* cmd, uint32_t indices, uint32_t, uint32_t, int32_t, uint32_t) {
    AdapterEvent e = event_for(cmd, EventKind::kDraw);
    if (!e.deferred) { e.vertices = indices; dispatch(e); }
    return false;
}
void on_present(command_queue* queue, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    if (queue && queue->get_device()->get_api() == device_api::d3d11)
        g_immediate = reinterpret_cast<std::uint64_t>(queue->get_immediate_command_list());
    AdapterEvent e;
    e.kind = EventKind::kPresent;
    e.streamline_frame = streamline_current_frame();
    dispatch(e);
}
void on_destroy_resource(device*, resource res) {
    AdapterEvent e;
    e.kind = EventKind::kDestroyResource;
    e.resource = res.handle;
    dispatch(e);
}
// A new swap chain or a resize (a resolution or upscaler change): what the adapter learned about the frame starts over.
void on_init_swapchain(swapchain*, bool) {
    std::lock_guard lock(g_mutex);
    g_core.reset();
}

class ReShadeRegistrar final : public Registrar {
public:
    void register_events() override {
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
        g_registered = false;
    }
};
ReShadeRegistrar g_registrar;

}  // namespace

void init() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::lock_guard lock(g_mutex);
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
