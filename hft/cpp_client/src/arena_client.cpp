// ─────────────────────────────────────────────────────────────────────────────
// arena_client.cpp — implementation of the AlgoArena HFT transport layer.
//
// Every outbound message is a JSON object whose shape matches a Pydantic model
// in shared/messages.py exactly (the exchange calls parse_message() on it).
// Every inbound frame is dispatched by its "type" discriminator to a virtual
// handler. See arena_client.hpp for the class contract.
// ─────────────────────────────────────────────────────────────────────────────

#include "arena_client.hpp"

#include <cstdlib>
#include <iostream>
#include <thread>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace arena {

namespace {

// Read an environment variable, falling back to `def` when unset/empty.
std::string env_or(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') return def;
    return std::string(v);

}


// Lets us hand std::visit several lambdas at once — one per message type.
// The compiler picks the lambda whose parameter type matches what's in the box.
template <class... Fs> struct overloaded : Fs... { using Fs::operator()...; };
template <class... Fs> overloaded(Fs...) -> overloaded<Fs...>;

}  // namespace

// ── ClientConfig ─────────────────────────────────────────────────────────────

ClientConfig ClientConfig::from_env() {
    ClientConfig c;
    c.team_id = env_or("TEAM_ID", "hft_alpha");
    c.token   = env_or("ARENA_TOKEN", "");

    // EXCHANGE_URL wins (hosted TLS play, e.g. wss://feed.arena.example.edu),
    // otherwise assemble ws://HOST:PORT — the same precedence as trader/config.py.
    const std::string url = env_or("EXCHANGE_URL", "");
    if (!url.empty()) {
        c.url = url;
    } else {
        const std::string host = env_or("EXCHANGE_HOST", "localhost");
        const std::string port = env_or("EXCHANGE_PORT", "8765");
        c.url = "ws://" + host + ":" + port;
    }

    const std::string lvl = env_or("ARENA_LEVEL", "1");
    try { c.level = std::stoi(lvl); } catch (...) { c.level = 1; }
    return c;
}
}

