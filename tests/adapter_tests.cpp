// The game adapters' host without a game (addon/adapters/host_logic.hpp): which adapter a game gets, when it is
// trusted and when let go, its switches, and that nothing is registered for a game without one.
#include "addon/adapters/host_logic.hpp"
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
    void on_event(const AdapterEvent&) override { ++events; }
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

}  // namespace

int main() {
    test_selection();
    test_registration();
    test_health();
    test_switches();
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("adapter host: all passed\n");
    return failures ? 1 : 0;
}
