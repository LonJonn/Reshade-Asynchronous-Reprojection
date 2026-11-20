#include "addon/ui_probe.hpp"
#include <reshade.hpp>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "addon/diag.hpp"
#include "addon/streamline_hooks.hpp"

namespace fw::ui_probe {
namespace {
using namespace reshade::api;

enum class Kind : std::uint8_t { Target, Draw, Dispatch, Copy, Clear, Mark, BackBufferState, Viewport };
// The output state draws are made with: blending (render target 0), depth test and write, the pixel shader.
struct DrawState {
    std::uint8_t blend = 0;            // 0 overwrite, 1 alpha blend, 2 additive, 3 other
    bool depth_test = true, depth_write = true;
    std::uint32_t ps = 0;              // the pixel shader's bytecode hash (0: none, or its creation not seen)
    bool operator==(const DrawState& o) const {
        return blend == o.blend && depth_test == o.depth_test && depth_write == o.depth_write && ps == o.ps;
    }
};
// What a pipeline sets (Direct3D 11: one per state object or shader; Direct3D 12 and Vulkan: all in one).
struct PipelineState {
    bool has_blend = false, has_depth = false, has_ps = false;
    std::uint8_t blend = 0;
    bool depth_test = true, depth_write = true;
    std::uint32_t ps = 0;
};
struct Entry {
    Kind kind = Kind::Target;
    std::uint64_t res = 0, src = 0;   // the target (copies: the destination) and a copy's source
    std::uint32_t w = 0, h = 0, fmt = 0, count = 0, targets = 0;
    bool depth = false;
    bool cleared = false;              // (targets: a render pass that starts by clearing the first target)
    DrawState draw;                    // (draws)
    std::uint32_t vertices = 0;        // (draws: their vertices or indices, all together)
    float min_depth = 0, max_depth = 0;  // (viewports; w x h their size)
    std::uint64_t sl_frame = 0;        // (targets: the newest Streamline camera frame when bound)
    bool nested = false;               // (Direct3D 11: recorded on a deferred context, shown where the immediate one ran it)
    std::uint32_t state = 0;           // (back buffer states)
    int frame = 0;                     // 1 the first recorded frame, 2 the second
    float color[4] = {};               // (clears)
    std::string text;
};

// 0 idle, 1 armed (recording starts at the next present), 2 recording the first frame, 3 the second (the one written:
// a game that records ahead - DOOM Eternal - records the end of a frame before the present that starts it).
std::atomic<int> g_state{0};
std::mutex g_mutex;
std::unordered_map<std::uint64_t, std::vector<Entry>> g_lists;  // by native command list: its latest recording
std::vector<std::pair<std::uint64_t, int>> g_order;             // native command lists as submitted, and in which frame
std::vector<std::uint64_t> g_recorded;                          // ...as first recorded into
std::vector<std::uint64_t> g_back_buffers;
std::unordered_map<std::uint64_t, PipelineState> g_pipelines;  // pipeline -> what it sets
std::unordered_map<std::uint64_t, DrawState> g_bound;           // native command list -> the bound draw state
std::unordered_map<std::uint64_t, Entry> g_last_viewport;       // native command list -> its last viewport recorded
// Direct3D 11: the immediate context (from the present), and deferred contexts' recordings finished into a command list,
// by that command list, until the immediate context executes it.
std::atomic<std::uint64_t> g_immediate{0};
std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::vector<Entry>>> g_finished;
std::vector<std::uint64_t> g_immediates;                        // (the immediate contexts seen while recording)
std::size_t g_entries = 0;
constexpr std::size_t kMaxEntries = 400000;
bool g_key_down = false;

int recording_frame() {
    const int s = g_state.load(std::memory_order_relaxed);
    return s == 2 ? 1 : s == 3 ? 2 : 0;
}
std::uint64_t list_key(command_list* c) { return c ? c->get_native() : 0; }

std::vector<Entry>* list_of(std::uint64_t key) {  // (under the lock)
    if (!key || g_entries >= kMaxEntries) return nullptr;
    auto [it, fresh] = g_lists.try_emplace(key);
    if (fresh) g_recorded.push_back(key);
    return &it->second;
}
void add(command_list* cmd, Entry e) {
    std::lock_guard<std::mutex> lock(g_mutex);
    e.frame = recording_frame();
    if (auto* l = list_of(list_key(cmd))) { l->push_back(std::move(e)); ++g_entries; }
}
void count(command_list* cmd, Kind kind, std::uint32_t n, std::uint32_t vertices = 0) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* l = list_of(list_key(cmd));
    if (!l) return;
    DrawState state;
    if (kind == Kind::Draw) {
        const auto it = g_bound.find(list_key(cmd));
        if (it != g_bound.end()) state = it->second;
    }
    const int frame = recording_frame();
    if (!l->empty() && l->back().kind == kind && l->back().draw == state && l->back().frame == frame) {
        l->back().count += n;
        l->back().vertices += vertices;
        return;
    }
    Entry e; e.kind = kind; e.count = n; e.vertices = vertices; e.draw = state; e.frame = frame;
    l->push_back(e);
    ++g_entries;
}
void describe(device* dev, resource r, Entry& e) {
    e.res = r.handle;
    if (!dev || !r.handle) return;
    const resource_desc d = dev->get_resource_desc(r);
    e.w = d.texture.width; e.h = d.texture.height; e.fmt = static_cast<std::uint32_t>(d.texture.format);
}
void target(command_list* cmd, uint32_t n, resource_view first, bool depth, const float* clear) {
    if (!recording_frame() || !cmd) return;
    Entry e; e.kind = Kind::Target; e.targets = n; e.depth = depth;
    device* dev = cmd->get_device();
    if (n && first.handle && dev) describe(dev, dev->get_resource_from_view(first), e);
    if (clear) { e.cleared = true; for (int i = 0; i < 4; ++i) e.color[i] = clear[i]; }
    e.sl_frame = streamline_current_frame();
    add(cmd, std::move(e));
}
void on_bind(command_list* cmd, uint32_t n, const resource_view* rtvs, resource_view dsv) {
    target(cmd, n, n && rtvs ? rtvs[0] : resource_view{0}, dsv.handle != 0, nullptr);
}
bool on_begin_pass(command_list* cmd, uint32_t n, const render_pass_render_target_desc* rts, const render_pass_depth_stencil_desc* ds, render_pass_flags) {
    // (Vulkan: a render pass that starts by clearing its target - no separate clear command)
    const bool clears = n && rts && rts[0].load_op == render_pass_load_op::clear;
    target(cmd, n, n && rts ? rts[0].view : resource_view{0}, ds && ds->view.handle, clears ? rts[0].clear_color : nullptr);
    return false;
}
bool on_draw(command_list* cmd, uint32_t vertices, uint32_t, uint32_t, uint32_t) { if (recording_frame()) count(cmd, Kind::Draw, 1, vertices); return false; }
bool on_draw_indexed(command_list* cmd, uint32_t indices, uint32_t, uint32_t, int32_t, uint32_t) { if (recording_frame()) count(cmd, Kind::Draw, 1, indices); return false; }
// (the first viewport: its size and depth range - a game drawing its first-person models into a near slice of depth shows
// here; recorded only when it changes)
void on_viewports(command_list* cmd, uint32_t first, uint32_t n, const viewport* vps) {
    if (!recording_frame() || !cmd || first != 0 || !n || !vps) return;
    Entry e; e.kind = Kind::Viewport;
    e.w = static_cast<std::uint32_t>(vps[0].width); e.h = static_cast<std::uint32_t>(vps[0].height);
    e.min_depth = vps[0].min_depth; e.max_depth = vps[0].max_depth;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Entry& last = g_last_viewport[list_key(cmd)];
        if (last.kind == Kind::Viewport && last.w == e.w && last.h == e.h && last.min_depth == e.min_depth && last.max_depth == e.max_depth) return;
        last = e;
    }
    add(cmd, std::move(e));
}
bool on_dispatch(command_list* cmd, uint32_t, uint32_t, uint32_t) { if (recording_frame()) count(cmd, Kind::Dispatch, 1); return false; }
bool on_indirect(command_list* cmd, indirect_command type, resource, uint64_t, uint32_t n, uint32_t) {
    if (recording_frame()) count(cmd, type == indirect_command::dispatch ? Kind::Dispatch : Kind::Draw, n);
    return false;
}
void copy(command_list* cmd, resource src, resource dst) {
    if (!recording_frame() || !cmd) return;
    Entry e; e.kind = Kind::Copy; e.src = src.handle;
    describe(cmd->get_device(), dst, e);
    add(cmd, std::move(e));
}
bool on_copy_resource(command_list* cmd, resource src, resource dst) { copy(cmd, src, dst); return false; }
bool on_copy_region(command_list* cmd, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, const subresource_box*, filter_mode) {
    copy(cmd, src, dst);
    return false;
}
bool on_resolve(command_list* cmd, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, uint32_t, uint32_t, uint32_t, format) {
    copy(cmd, src, dst);
    return false;
}
bool on_clear(command_list* cmd, resource_view rtv, const float* color, uint32_t, const rect*) {
    if (!recording_frame() || !cmd) return false;
    Entry e; e.kind = Kind::Clear;
    if (color) for (int i = 0; i < 4; ++i) e.color[i] = color[i];
    if (device* dev = cmd->get_device(); dev && rtv.handle) describe(dev, dev->get_resource_from_view(rtv), e);
    add(cmd, std::move(e));
    return false;
}
// (the back buffers' state changes: a compute pass writing the final picture into one shows as a change to unordered
// access / storage)
void on_barrier(command_list* cmd, uint32_t n, const resource* resources, const resource_usage*, const resource_usage* new_states) {
    if (!recording_frame() || !cmd || !n) return;
    for (uint32_t i = 0; i < n; ++i) {
        bool back = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const std::uint64_t b : g_back_buffers) back = back || b == resources[i].handle;
        }
        if (!back) continue;
        Entry e; e.kind = Kind::BackBufferState; e.res = resources[i].handle; e.state = static_cast<std::uint32_t>(new_states[i]);
        add(cmd, std::move(e));
    }
}
void on_reset(command_list* cmd) {
    if (!recording_frame() || !cmd) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_lists.find(list_key(cmd));
    if (it != g_lists.end()) { g_entries -= it->second.size(); it->second.clear(); }  // (a new recording of it)
    g_last_viewport.erase(list_key(cmd));
}
// Direct3D 11's deferred contexts: FinishCommandList (on the deferred context) ends a recording into a command list, which
// the immediate context then executes - that is where it reaches the GPU, so its entries are shown there.
void on_secondary(command_list* cmd, command_list* secondary) {
    if (!recording_frame() || !cmd || !secondary) return;
    const std::uint64_t key = list_key(cmd), sec = list_key(secondary);
    const int frame = recording_frame();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (key != g_immediate.load()) {  // (finished on a deferred context)
        auto& slot = g_finished[sec];
        slot.first = key;
        slot.second.clear();
        const auto it = g_lists.find(key);
        if (it != g_lists.end()) slot.second.swap(it->second);
        g_last_viewport.erase(key);
        return;
    }
    auto* l = list_of(key);
    if (!l) return;
    auto mark_here = [&](const char* text) {
        Entry m; m.kind = Kind::Mark; m.text = text; m.frame = frame;
        l->push_back(std::move(m));
        ++g_entries;
    };
    char text[160];
    const auto it = g_finished.find(sec);
    if (it == g_finished.end()) {
        std::snprintf(text, sizeof(text), "executes command list %016llx (its recording not seen)", static_cast<unsigned long long>(sec));
        mark_here(text);
        return;
    }
    std::snprintf(text, sizeof(text), "executes deferred context %016llx's command list (%zu entries):",
                  static_cast<unsigned long long>(it->second.first), it->second.second.size());
    mark_here(text);
    for (Entry e : it->second.second) {  // (copied: a command list can be executed more than once)
        e.nested = true;
        e.frame = frame;
        l->push_back(std::move(e));
        ++g_entries;
    }
    mark_here("end of the deferred context's command list");
}
void on_execute(command_queue*, command_list* cmd) {
    if (!recording_frame() || !cmd) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_order.emplace_back(list_key(cmd), recording_frame());
}
// Blending of each pipeline (render target 0): how its draws combine with what is there.
std::uint8_t blend_kind(const blend_desc& b) {
    if (!b.blend_enable[0]) return 0;
    const blend_factor s = b.source_color_blend_factor[0], d = b.dest_color_blend_factor[0];
    if ((s == blend_factor::source_alpha || s == blend_factor::one) && d == blend_factor::one_minus_source_alpha) return 1;
    if (d == blend_factor::one) return 2;
    return 3;
}
// (a short fingerprint of a shader's bytecode, to tell the same shader apart across draws and runs: FNV-1a)
std::uint32_t shader_hash(const shader_desc& s) {
    if (!s.code || !s.code_size) return 0;
    std::uint32_t h = 2166136261u;
    const auto* b = static_cast<const std::uint8_t*>(s.code);
    for (size_t i = 0; i < s.code_size; ++i) h = (h ^ b[i]) * 16777619u;
    return h ? h : 1;
}
void on_init_pipeline(device*, pipeline_layout, uint32_t count_, const pipeline_subobject* subobjects, pipeline p) {
    if (!p.handle) return;
    PipelineState st;
    bool any = false;
    for (uint32_t i = 0; i < count_; ++i) {
        if (!subobjects[i].data) continue;
        switch (subobjects[i].type) {
            case pipeline_subobject_type::blend_state:
                st.has_blend = any = true;
                st.blend = blend_kind(*static_cast<const blend_desc*>(subobjects[i].data));
                break;
            case pipeline_subobject_type::depth_stencil_state: {
                const auto& d = *static_cast<const depth_stencil_desc*>(subobjects[i].data);
                st.has_depth = any = true;
                st.depth_test = d.depth_enable;
                st.depth_write = d.depth_enable && d.depth_write_mask;
                break;
            }
            case pipeline_subobject_type::pixel_shader:
                st.has_ps = any = true;
                st.ps = shader_hash(*static_cast<const shader_desc*>(subobjects[i].data));
                break;
            default: break;
        }
    }
    if (!any) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pipelines[p.handle] = st;
}
void on_destroy_pipeline(device*, pipeline p) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pipelines.erase(p.handle);
}
void on_bind_pipeline(command_list* cmd, pipeline_stage stages, pipeline p) {
    // (only while recording: binds are many every frame)
    if (!recording_frame() || !cmd) return;
    const auto has = [stages](pipeline_stage s) { return (static_cast<uint32_t>(stages) & static_cast<uint32_t>(s)) != 0; };
    std::lock_guard<std::mutex> lock(g_mutex);
    DrawState& bound = g_bound[list_key(cmd)];
    const auto it = g_pipelines.find(p.handle);
    const PipelineState* st = it != g_pipelines.end() ? &it->second : nullptr;
    // (what the pipeline sets for each stage bound; a null state object or shader, or one not seen made: the defaults - no
    // blending, depth test and write on, no pixel shader hash)
    if (has(pipeline_stage::output_merger) && (!st || st->has_blend)) bound.blend = st ? st->blend : 0;
    if (has(pipeline_stage::depth_stencil) && (!st || st->has_depth)) {
        bound.depth_test = st ? st->depth_test : true;
        bound.depth_write = st ? st->depth_write : true;
    }
    if (has(pipeline_stage::pixel_shader) && (!st || st->has_ps)) bound.ps = st ? st->ps : 0;
}

void write_file() {
    std::unordered_map<std::uint64_t, std::vector<Entry>> lists;
    std::vector<std::pair<std::uint64_t, int>> order;
    std::vector<std::uint64_t> recorded, back_buffers, immediates;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        lists.swap(g_lists); order.swap(g_order); recorded.swap(g_recorded); back_buffers = g_back_buffers; immediates = g_immediates;
        g_finished.clear(); g_last_viewport.clear();
        g_entries = 0;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\FrameWarp\\logs\\";
    std::wstring path;
    for (int i = 1; i < 1000; ++i) {
        path = dir + L"ui_probe_" + std::to_wstring(i) + L".txt";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") || !f) return;
    std::unordered_map<std::uint64_t, int> names;
    auto name = [&](std::uint64_t r) {
        if (!r) return std::string("-");
        for (std::size_t i = 0; i < back_buffers.size(); ++i) if (back_buffers[i] == r) return "BACKBUFFER" + std::to_string(i);
        auto [it, fresh] = names.try_emplace(r, int(names.size()) + 1);
        return "T" + std::to_string(it->second);
    };
    std::size_t submitted = 0;
    for (const auto& kv : order) submitted += kv.second == 2 ? 1 : 0;
    std::fprintf(f, "UI probe: one game frame (the second of two recorded) - %zu submissions in it\n", submitted);
    std::fprintf(f, "(per command list as submitted in that frame, with what was recorded into it - possibly during the frame before:\n"
                    " render targets bound [first target WxH format, +n more, depth or not, cleared by the render pass, the newest\n"
                    " Streamline camera frame then], viewports [size, depth range] when they change, then what was drawn into them:\n"
                    " draws by blending - overwrite / alpha blend / additive / other -, depth test and write, the pixel shader's\n"
                    " bytecode hash and the vertices or indices; back buffer state changes. Direct3D 11: a deferred context's\n"
                    " recording is shown indented, where the immediate context executed it)\n\n");
    static const char* const kBlend[4] = {"overwrite", "alpha blend", "additive", "other blend"};
    auto dump = [&](std::uint64_t key, const char* how, std::size_t index, int only_frame) {
        const auto it = lists.find(key);
        if (it == lists.end() || it->second.empty()) return;
        std::size_t shown = 0;
        for (const Entry& e : it->second) shown += !only_frame || e.frame == only_frame ? 1 : 0;
        if (!shown) return;
        bool immediate = false;
        for (const std::uint64_t i : immediates) immediate = immediate || i == key;
        std::fprintf(f, "list %zu (%s%s, %016llx): %zu entries\n", index, how, immediate ? ", Direct3D 11 immediate context" : "",
                     static_cast<unsigned long long>(key), shown);
        for (const Entry& e : it->second) {
            if (only_frame && e.frame != only_frame) continue;
            if (e.nested) std::fputs("    ", f);
            switch (e.kind) {
                case Kind::Target: {
                    if (!e.targets) { std::fprintf(f, "  targets: none%s\n", e.depth ? " + depth" : ""); break; }
                    char cleared[96] = "";
                    if (e.cleared) std::snprintf(cleared, sizeof(cleared), " (cleared to (%g, %g, %g, %g))", e.color[0], e.color[1], e.color[2], e.color[3]);
                    std::fprintf(f, "  targets: %s %ux%u fmt %u%s%s%s [sl %llu]\n", name(e.res).c_str(), e.w, e.h, e.fmt,
                                 e.targets > 1 ? (" +" + std::to_string(e.targets - 1)).c_str() : "", e.depth ? " + depth" : "", cleared,
                                 static_cast<unsigned long long>(e.sl_frame));
                    break;
                }
                case Kind::Draw:
                    std::fprintf(f, "    draws %u (%s, depth %s, ps %08x, %u vertices)\n", e.count, kBlend[e.draw.blend & 3],
                                 !e.draw.depth_test ? "off" : e.draw.depth_write ? "test+write" : "test", e.draw.ps, e.vertices);
                    break;
                case Kind::Viewport: std::fprintf(f, "  viewport %ux%u depth %g..%g\n", e.w, e.h, e.min_depth, e.max_depth); break;
                case Kind::Dispatch: std::fprintf(f, "    dispatches %u\n", e.count); break;
                case Kind::Copy: std::fprintf(f, "  copy %s -> %s %ux%u fmt %u\n", name(e.src).c_str(), name(e.res).c_str(), e.w, e.h, e.fmt); break;
                case Kind::Clear:
                    std::fprintf(f, "  clear %s %ux%u fmt %u to (%g, %g, %g, %g)\n", name(e.res).c_str(), e.w, e.h, e.fmt, e.color[0], e.color[1],
                                 e.color[2], e.color[3]);
                    break;
                case Kind::Mark: std::fprintf(f, "  ** %s\n", e.text.c_str()); break;
                case Kind::BackBufferState: std::fprintf(f, "  %s -> state 0x%x\n", name(e.res).c_str(), e.state); break;
            }
        }
    };
    std::unordered_map<std::uint64_t, bool> done;
    std::size_t index = 0;
    for (const auto& kv : order) {  // (as submitted in the second frame; a list submitted twice is shown once)
        if (kv.second != 2 || done[kv.first]) continue;
        done[kv.first] = true;
        dump(kv.first, "submitted", ++index, 0);
    }
    // (recorded but not submitted in that frame: the next frame's, recorded ahead - or Direct3D 11's immediate context,
    // which is never submitted: only what it recorded in that frame)
    for (const std::uint64_t key : recorded)
        if (!done[key]) dump(key, "recorded, not submitted in the frame", ++index, 2);
    std::fclose(f);
    fw::diag_log(reshade::log::level::info, "XPAR: UI probe written (FrameWarp\\logs\\ui_probe_N.txt)");
}
}  // namespace

void init() {
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind);
    reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_pass);
    reshade::register_event<reshade::addon_event::draw>(on_draw);
    reshade::register_event<reshade::addon_event::bind_viewports>(on_viewports);
    reshade::register_event<reshade::addon_event::execute_secondary_command_list>(on_secondary);
    reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
    reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
    reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
    reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_region);
    reshade::register_event<reshade::addon_event::resolve_texture_region>(on_resolve);
    reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear);
    reshade::register_event<reshade::addon_event::barrier>(on_barrier);
    reshade::register_event<reshade::addon_event::reset_command_list>(on_reset);
    reshade::register_event<reshade::addon_event::execute_command_list>(on_execute);
    reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
}
void shutdown() {
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
    reshade::unregister_event<reshade::addon_event::execute_command_list>(on_execute);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(on_reset);
    reshade::unregister_event<reshade::addon_event::barrier>(on_barrier);
    reshade::unregister_event<reshade::addon_event::clear_render_target_view>(on_clear);
    reshade::unregister_event<reshade::addon_event::resolve_texture_region>(on_resolve);
    reshade::unregister_event<reshade::addon_event::copy_texture_region>(on_copy_region);
    reshade::unregister_event<reshade::addon_event::copy_resource>(on_copy_resource);
    reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
    reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
    reshade::unregister_event<reshade::addon_event::execute_secondary_command_list>(on_secondary);
    reshade::unregister_event<reshade::addon_event::bind_viewports>(on_viewports);
    reshade::unregister_event<reshade::addon_event::draw>(on_draw);
    reshade::unregister_event<reshade::addon_event::begin_render_pass>(on_begin_pass);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind);
}

void on_present(swapchain* chain, bool keys_on, std::uint64_t immediate_native) {
    if (immediate_native) g_immediate = immediate_native;
    const bool down = keys_on && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('D') & 0x8000);
    if (down && !g_key_down && g_state.load() == 0) g_state = 1;
    g_key_down = down;
    const int state = g_state.load();
    if (state == 3) {
        g_state = 0;
        write_file();
    } else if (state == 2) {
        g_state = 3;  // (the second frame starts)
    } else if (state == 1 && chain) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_back_buffers.clear();
            for (uint32_t i = 0; i < chain->get_back_buffer_count(); ++i) g_back_buffers.push_back(chain->get_back_buffer(i).handle);
            g_lists.clear(); g_order.clear(); g_recorded.clear(); g_entries = 0;
            g_bound.clear(); g_finished.clear(); g_last_viewport.clear();
            g_immediates.clear();
            if (immediate_native) g_immediates.push_back(immediate_native);
        }
        g_state = 2;
    }
}

void mark(std::uint64_t native_cmd_list, const char* what) {
    if (!recording_frame() || !native_cmd_list) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (auto* l = list_of(native_cmd_list)) {
        Entry e; e.kind = Kind::Mark; e.text = what; e.frame = recording_frame();
        l->push_back(std::move(e));
        ++g_entries;
    }
}
}  // namespace fw::ui_probe
