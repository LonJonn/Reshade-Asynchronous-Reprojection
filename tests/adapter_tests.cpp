// The game adapters' host without a game (addon/adapters/host_logic.hpp): which adapter a game gets, when it is
// trusted and when let go, its switches, and that nothing is registered for a game without one.
#include "addon/adapters/host_logic.hpp"
#include "addon/adapters/xray.hpp"
#include <cstdio>
#include <map>

static int failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace fw::adapters;

namespace {

// An adapter whose verdict for each frame is set by the test.
struct FakeAdapter final : GameAdapter {
    static inline Verdict next = Verdict::kNotSeen;
    static inline int events = 0, resets = 0;
    std::uint32_t capabilities() const override { return kCapHud | kCapFirstPerson; }
    void on_event(const AdapterEvent& e, Output& out) override {
        ++events;
        if (e.kind == EventKind::kDraw) out.hud_before_d3d11(e.context, 0xBB, HudBefore::kCopyNow);  // (each draw says something)
    }
    Verdict end_frame() override { return next; }
    void reset() override { ++resets; }
};

bool matches_fake(const GameInfo& game) { return exe_name(game.exe_path) == L"fakegame.exe"; }
bool matches_other(const GameInfo& game) { return exe_name(game.exe_path) == L"other.exe"; }
std::unique_ptr<GameAdapter> create_fake() { return std::make_unique<FakeAdapter>(); }

const std::vector<AdapterDescriptor> kTable = {
    {"fake", "Fake engine", matches_fake, create_fake},
    {"other", "Other engine", matches_other, create_fake},
};

struct CountingRegistrar final : Registrar {
    int registered = 0, unregistered = 0;
    void register_events() override { ++registered; }
    void unregister_events() override { ++unregistered; }
};

std::function<std::string(const std::string&)> settings(std::map<std::string, std::string> values) {
    return [values](const std::string& key) {
        const auto it = values.find(key);
        return it == values.end() ? std::string() : it->second;
    };
}

void present(HostCore& host, Verdict v) {
    FakeAdapter::next = v;
    AdapterEvent e;
    e.kind = EventKind::kPresent;
    host.on_event(e);
}

void test_selection() {
    EXPECT(exe_name(L"C:\\Games\\Bin\\FakeGame.EXE") == L"fakegame.exe", "the file name, lower case");
    const GameInfo fake{L"C:\\Games\\FakeGame.exe"}, unknown{L"C:\\Games\\Unknown.exe"};
    EXPECT(select_adapter(kTable, fake, "") == 0, "auto (empty) picks the matching adapter");
    EXPECT(select_adapter(kTable, fake, "auto") == 0, "auto picks the matching adapter");
    EXPECT(select_adapter(kTable, GameInfo{L"D:\\other.exe"}, "auto") == 1, "auto picks the second when it matches");
    EXPECT(select_adapter(kTable, unknown, "auto") == -1, "no adapter for an unknown game");
    EXPECT(select_adapter(kTable, fake, "off") == -1, "off: none, even for a matching game");
    EXPECT(select_adapter(kTable, unknown, "other") == 1, "an id forces that adapter on any game");
    EXPECT(select_adapter(kTable, fake, "nonsense") == -1, "an unknown id: none");
    EXPECT(select_adapter({}, fake, "auto") == -1, "an empty table: none");
}

void test_registration() {
    CountingRegistrar reg;
    HostCore host;
    host.init(kTable, GameInfo{L"C:\\Games\\Unknown.exe"}, settings({}), reg);
    EXPECT(!host.active() && reg.registered == 0, "no adapter: no events registered");
    host.shutdown();
    EXPECT(reg.unregistered == 0, "nothing to unregister");

    host.init({}, GameInfo{L"C:\\Games\\FakeGame.exe"}, settings({}), reg);
    EXPECT(!host.active() && reg.registered == 0, "empty table: no events registered");

    host.init(kTable, GameInfo{L"C:\\Games\\FakeGame.exe"}, settings({{"Adapter", "off"}}), reg);
    EXPECT(!host.active() && reg.registered == 0, "Adapter=off: no events registered");

    host.init(kTable, GameInfo{L"C:\\Games\\FakeGame.exe"}, settings({}), reg);
    EXPECT(host.active() && reg.registered == 1, "a matching adapter registers once");
    EXPECT(std::string(host.name()) == "Fake engine", "its name");
    host.shutdown();
    EXPECT(reg.unregistered == 1 && !host.active(), "shutdown unregisters");
}

void test_health() {
    CountingRegistrar reg;
    HostCore host;
    host.init(kTable, GameInfo{L"FakeGame.exe"}, settings({}), reg);
    EXPECT(host.health() == Health::kProbation && !host.gives(kCapHud), "starts on probation, nothing used");

    for (int i = 0; i < 500; ++i) present(host, Verdict::kNotSeen);
    EXPECT(host.health() == Health::kProbation, "frames without a verdict (menus, loading) change nothing");

    for (int i = 0; i < Health::kConfirmFrames - 1; ++i) present(host, Verdict::kConfirmed);
    EXPECT(host.health() == Health::kProbation, "not trusted before %d confirmed frames", Health::kConfirmFrames);
    present(host, Verdict::kConfirmed);
    EXPECT(host.health() == Health::kHealthy && host.gives(kCapHud) && host.gives(kCapFirstPerson), "trusted after them");
    EXPECT(!host.gives(kCapSceneBehind), "a capability it does not offer is never given");

    // Occasional contradictions (a glitch) do not let it go.
    for (int i = 0; i < 200; ++i) present(host, i % 10 == 0 ? Verdict::kContradicted : Verdict::kConfirmed);
    EXPECT(host.health() == Health::kHealthy, "one in ten contradicted: still trusted");

    // A reset (resolution change) puts it back on probation.
    const int resets = FakeAdapter::resets;
    host.reset();
    EXPECT(host.health() == Health::kProbation && FakeAdapter::resets == resets + 1, "reset: probation, the adapter told");

    // Contradicted on many recent frames: let go for good.
    for (int i = 0; i < Health::kContradictLimit; ++i) present(host, Verdict::kContradicted);
    EXPECT(host.health() == Health::kDetached && !host.gives(kCapHud), "let go after %d contradictions", Health::kContradictLimit);
    const int events = FakeAdapter::events;
    present(host, Verdict::kConfirmed);
    EXPECT(FakeAdapter::events == events, "a let-go adapter gets no more events");
    host.reset();
    EXPECT(host.health() == Health::kDetached, "a reset does not bring it back");
    host.shutdown();

    // A fault inside the adapter (the host's guard calls detach).
    host.init(kTable, GameInfo{L"FakeGame.exe"}, settings({}), reg);
    host.detach();
    EXPECT(host.health() == Health::kDetached, "detach lets it go");
    host.shutdown();
}

void test_switches() {
    EXPECT(enabled_capabilities(kCapHud | kCapFirstPerson, nullptr) == (kCapHud | kCapFirstPerson), "all on by default");
    EXPECT(enabled_capabilities(kCapHud | kCapFirstPerson, [](const std::string& k) { return k == "AdapterHud" ? '0' : '\0'; }) == kCapFirstPerson,
           "AdapterHud=0 turns the HUD off");
    EXPECT(enabled_capabilities(kCapHud, [](const std::string&) { return '1'; }) == kCapHud, "=1 keeps it on");
    EXPECT(enabled_capabilities(0, nullptr) == 0, "nothing offered, nothing on");

    CountingRegistrar reg;
    HostCore host;
    host.init(kTable, GameInfo{L"FakeGame.exe"}, settings({{"AdapterFirstPerson", "0"}}), reg);
    for (int i = 0; i < Health::kConfirmFrames; ++i) present(host, Verdict::kConfirmed);
    EXPECT(host.gives(kCapHud) && !host.gives(kCapFirstPerson), "a switched-off capability is not given while trusted");
    host.shutdown();
}

// What an adapter says reaches the producer only while it is trusted and the capability is on.
void test_output_gate() {
    CountingRegistrar reg;
    HostCore host;
    host.init(kTable, GameInfo{L"FakeGame.exe"}, settings({}), reg);
    AdapterEvent draw;
    draw.kind = EventKind::kDraw;
    draw.context = 7;
    host.on_event(draw);
    EXPECT(host.take_outputs().empty(), "nothing handed on while on probation");
    for (int i = 0; i < Health::kConfirmFrames; ++i) present(host, Verdict::kConfirmed);
    host.on_event(draw);
    const auto out = host.take_outputs();
    EXPECT(out.size() == 1 && out[0].context == 7 && out[0].picture == 0xBB && out[0].kind == HudBefore::kCopyNow,
           "handed on once trusted (got %zu)", out.size());
    EXPECT(host.take_outputs().empty(), "taken once");
    host.shutdown();

    HostCore off;
    off.init(kTable, GameInfo{L"FakeGame.exe"}, settings({{"AdapterHud", "0"}}), reg);
    for (int i = 0; i < Health::kConfirmFrames; ++i) present(off, Verdict::kConfirmed);
    off.on_event(draw);
    EXPECT(off.take_outputs().empty(), "nothing handed on with the capability switched off");
    off.shutdown();
}

// The X-Ray adapter, fed a frame the way the host tells it (plain events).
struct Recorder final : Output {
    struct Said { std::uint64_t context, picture; HudBefore kind; };
    std::vector<Said> said;
    void hud_before_d3d11(std::uint64_t context, std::uint64_t picture, HudBefore kind) override { said.push_back({context, picture, kind}); }
};
constexpr std::uint64_t kCtx = 0x10, kBackBuffer = 0xB0, kSceneTarget = 0x50, kPictureShader = 0x900, kHudShader = 0x901;
void bind(XRayAdapter& a, Recorder& r, std::uint64_t target, bool back_buffer, bool depth = true) {
    AdapterEvent e;
    e.kind = EventKind::kBindTargets;
    e.context = kCtx;
    e.target_count = 1;
    e.targets[0].resource = target;
    e.targets[0].back_buffer = back_buffer;
    if (depth) e.depth.resource = 0xD0;
    a.on_event(e, r);
}
void draw(XRayAdapter& a, Recorder& r, std::uint32_t vertices, Blend blend, std::uint64_t shader, bool writes_colour = true) {
    AdapterEvent e;
    e.kind = EventKind::kDraw;
    e.context = kCtx;
    e.vertices = vertices;
    e.blend = blend;
    e.pixel_shader = shader;
    e.writes_colour = writes_colour;
    a.on_event(e, r);
}
Verdict end(XRayAdapter& a, Recorder& r) {
    AdapterEvent e;
    e.kind = EventKind::kPresent;
    a.on_event(e, r);
    return a.end_frame();
}

void test_xray() {
    EXPECT(xray_matches(GameInfo{L"D:\\Anomaly\\bin\\AnomalyDX11AVX.exe"}) && xray_matches(GameInfo{L"AnomalyDX11.exe"}),
           "matches Anomaly's Direct3D 11 executables");
    EXPECT(!xray_matches(GameInfo{L"AnomalyDX9.exe"}) && !xray_matches(GameInfo{L"game.exe"}), "nothing else");

    {  // a frame with a HUD: the copy at the first alpha-blended draw after the picture draw
        XRayAdapter a;
        Recorder r;
        bind(a, r, kSceneTarget, false);
        draw(a, r, 30000, Blend::kNone, 0x1);   // (the 3D scene, not on the back buffer)
        draw(a, r, 3, Blend::kAlpha, 0x2);      // (a blended draw into another target: nothing)
        bind(a, r, kBackBuffer, true);
        draw(a, r, 36, Blend::kNone, 0x3, false);  // (an occlusion query on the back buffer: writes no colour)
        draw(a, r, 6, Blend::kNone, kPictureShader);
        EXPECT(r.said.empty(), "nothing said before the HUD starts");
        draw(a, r, 120, Blend::kAlpha, kHudShader);
        draw(a, r, 60, Blend::kAlpha, kHudShader);
        draw(a, r, 6, Blend::kNone, kHudShader);  // (an opaque part of the HUD: another shader - kept)
        EXPECT(r.said.size() == 1 && r.said[0].kind == HudBefore::kCopyNow && r.said[0].picture == kBackBuffer && r.said[0].context == kCtx,
               "one copy of the back buffer, at the HUD's first draw (said %zu)", r.said.size());
        EXPECT(end(a, r) == Verdict::kConfirmed && r.said.size() == 1, "the frame confirms it; nothing more at the present");

        r.said.clear();  // the next frame, no HUD: the back buffer itself
        bind(a, r, kBackBuffer, true);
        draw(a, r, 6, Blend::kNone, kPictureShader);
        EXPECT(end(a, r) == Verdict::kConfirmed && r.said.size() == 1 && r.said[0].kind == HudBefore::kNoHudThisFrame &&
                   r.said[0].picture == kBackBuffer,
               "no HUD: said at the present");

        r.said.clear();  // the picture drawn again after the copy: given up
        bind(a, r, kBackBuffer, true);
        draw(a, r, 6, Blend::kNone, kPictureShader);
        draw(a, r, 120, Blend::kAlpha, kHudShader);
        draw(a, r, 6, Blend::kNone, kPictureShader);
        EXPECT(r.said.size() == 2 && r.said[1].kind == HudBefore::kGivenUp, "the copy given up");
        EXPECT(end(a, r) == Verdict::kContradicted, "a given-up copy contradicts the adapter");

        r.said.clear();  // a menu: nothing drawn onto the back buffer the expected way
        bind(a, r, kBackBuffer, true);
        draw(a, r, 400, Blend::kNone, 0x7);  // (more than a full-screen draw: not the picture)
        draw(a, r, 60, Blend::kAlpha, kHudShader);
        EXPECT(end(a, r) == Verdict::kNotSeen && r.said.empty(), "a frame without the pattern is not a verdict");
    }
    {  // before the HUD was ever seen drawn this way, a picture draw alone says nothing
        XRayAdapter a;
        Recorder r;
        bind(a, r, kBackBuffer, true);
        draw(a, r, 6, Blend::kNone, kPictureShader);
        EXPECT(end(a, r) == Verdict::kNotSeen && r.said.empty(), "no \"no HUD\" before the pattern was seen");
    }
}

}  // namespace

int main() {
    test_selection();
    test_registration();
    test_health();
    test_switches();
    test_output_gate();
    test_xray();
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("adapter host: all passed\n");
    return failures ? 1 : 0;
}
