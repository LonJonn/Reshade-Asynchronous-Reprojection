#pragma once
// The game adapters' host, without ReShade (host.cpp wires it to ReShade's events): which adapter the game gets, whether
// it is trusted, and which of its capabilities are switched on. Header-only so the tests can run it without a game.
#include "addon/adapters/adapter.hpp"
#include <cwctype>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace fw::adapters {

// The adapter for the game: -1 none. setting is [FrameWarp] Adapter: empty or "auto" - the first whose matches() says
// yes; "off" - none; an id - that one, matching or not (to try an adapter on a game it does not know).
inline int select_adapter(const std::vector<AdapterDescriptor>& table, const GameInfo& game, const std::string& setting) {
    if (setting == "off") return -1;
    if (setting.empty() || setting == "auto") {
        for (std::size_t i = 0; i < table.size(); ++i)
            if (table[i].matches && table[i].matches(game)) return static_cast<int>(i);
        return -1;
    }
    for (std::size_t i = 0; i < table.size(); ++i)
        if (setting == table[i].id) return static_cast<int>(i);
    return -1;
}

// The file name of a path, lower case (for matches()).
inline std::wstring exe_name(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
    return name;
}

// The capabilities switched on: each one unless [FrameWarp] Adapter<Name> is "0". read(key) gives the setting's first
// character (0: not set).
inline std::uint32_t enabled_capabilities(std::uint32_t offered, const std::function<char(const std::string& key)>& read) {
    std::uint32_t on = 0;
    for (int bit = 0; bit < kCapabilityCount; ++bit) {
        const std::uint32_t cap = 1u << bit;
        if (!(offered & cap)) continue;
        if (read && read(std::string("Adapter") + capability_name(bit)) == '0') continue;
        on |= cap;
    }
    return on;
}

// Trust in the adapter, from its verdicts frame by frame. It starts on probation (nothing it says is used), is trusted
// once it was confirmed on kConfirmFrames frames, and is let go for the run when it was contradicted on kContradictLimit
// of its last kWindow frames with a verdict. Frames without a verdict (kNotSeen) count for nothing either way. reset()
// (a new device or resolution) puts it on probation again - unless it was let go.
class Health {
public:
    enum State : std::uint8_t { kProbation, kHealthy, kDetached };
    static constexpr int kConfirmFrames = 30;
    static constexpr int kWindow = 60;
    static constexpr int kContradictLimit = 10;

    State state() const { return state_; }
    void add(Verdict v) {
        if (state_ == kDetached || v == Verdict::kNotSeen) return;
        recent_.push_back(v == Verdict::kContradicted);
        if (recent_.size() > static_cast<std::size_t>(kWindow)) recent_.pop_front();
        int contradicted = 0;
        for (const bool c : recent_) contradicted += c ? 1 : 0;
        if (contradicted >= kContradictLimit) { state_ = kDetached; return; }
        if (v == Verdict::kConfirmed && state_ == kProbation && ++confirmed_ >= kConfirmFrames) state_ = kHealthy;
    }
    void reset() {
        if (state_ == kDetached) return;
        state_ = kProbation;
        confirmed_ = 0;
        recent_.clear();
    }
    void detach() { state_ = kDetached; }

private:
    State state_ = kProbation;
    int confirmed_ = 0;
    std::deque<bool> recent_;
};
inline const char* health_name(Health::State s) {
    return s == Health::kHealthy ? "trusted" : s == Health::kDetached ? "let go" : "checking";
}

// The ReShade events, registered only when the game has an adapter (host.cpp; the tests count the calls).
class Registrar {
public:
    virtual ~Registrar() = default;
    virtual void register_events() = 0;
    virtual void unregister_events() = 0;
};

class HostCore {
public:
    // Picks the game's adapter and, when there is one, registers the events. read: [FrameWarp] values by key (the
    // whole string; empty when not set).
    void init(const std::vector<AdapterDescriptor>& table, const GameInfo& game, const std::function<std::string(const std::string&)>& read,
              Registrar& registrar) {
        shutdown();
        const int chosen = select_adapter(table, game, read ? read("Adapter") : std::string());
        if (chosen < 0) return;
        adapter_ = table[chosen].create ? table[chosen].create() : nullptr;
        if (!adapter_) return;
        descriptor_ = &table[chosen];
        enabled_ = enabled_capabilities(adapter_->capabilities(), [&](const std::string& key) {
            const std::string v = read ? read(key) : std::string();
            return v.empty() ? '\0' : v[0];
        });
        health_ = Health();
        registrar_ = &registrar;
        registrar_->register_events();
    }
    void shutdown() {
        if (registrar_) registrar_->unregister_events();
        registrar_ = nullptr;
        adapter_.reset();
        descriptor_ = nullptr;
        enabled_ = 0;
    }

    void on_event(const AdapterEvent& event) {
        if (!adapter_ || health_.state() == Health::kDetached) return;
        adapter_->on_event(event);
        if (event.kind == EventKind::kPresent) health_.add(adapter_->end_frame());
    }
    void reset() {
        if (adapter_) adapter_->reset();
        health_.reset();
    }
    // The adapter failed (a fault inside it): let go for the run.
    void detach() { health_.detach(); }

    bool active() const { return adapter_ != nullptr; }
    GameAdapter* adapter() const { return adapter_.get(); }
    // A capability to be used now: offered, switched on, and the adapter trusted.
    bool gives(Capability cap) const { return adapter_ && health_.state() == Health::kHealthy && (enabled_ & cap); }
    const char* name() const { return descriptor_ ? descriptor_->name : nullptr; }
    std::uint32_t offered() const { return adapter_ ? adapter_->capabilities() : 0; }
    std::uint32_t enabled() const { return enabled_; }
    Health::State health() const { return health_.state(); }

private:
    std::unique_ptr<GameAdapter> adapter_;
    const AdapterDescriptor* descriptor_ = nullptr;
    std::uint32_t enabled_ = 0;
    Health health_;
    Registrar* registrar_ = nullptr;
};

}  // namespace fw::adapters
