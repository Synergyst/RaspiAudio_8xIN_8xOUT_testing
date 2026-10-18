#include "dsp_engine.h"
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

using json = nlohmann::json;

namespace {
std::atomic<bool> entered{false}, release_reader{false}, destroyed{false};
std::atomic<unsigned> processed{0};

class BlockingProcessor : public AudioProcessor {
public:
    ~BlockingProcessor() override { destroyed.store(true); }
    void process(float*, size_t, unsigned) override {
        entered.store(true);
        while (!release_reader.load()) std::this_thread::yield();
    }
};
class CountingProcessor : public AudioProcessor {
public:
    void process(float*, size_t, unsigned) override { processed.fetch_add(1); }
};

template<class F> void expect_invalid(F action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}
template<class F> void wait_for(F predicate) {
    for (unsigned i = 0; i < 5000; ++i) {
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "Timed out waiting for test condition");
}
json response_json(const httplib::Result& response, int status) {
    if (!response || response->status != status) {
        std::cerr << "Expected HTTP " << status << ", got "
                  << (response ? std::to_string(response->status) + ": " + response->body : "no response") << std::endl;
        assert(false);
    }
    return json::parse(response->body);
}
}

int main() {
    auto& registry = ProcessorRegistry::instance();
    const auto discovery = registry.discovery();
    assert(discovery["schema_version"] == 1);
    assert(discovery["max_chain_length"] == CM5_MAX_PROCESSORS_PER_ROUTE);
    assert(discovery["processors"].size() == 3);
    for (const auto& d : discovery["processors"]) {
        json normalized;
        auto p = registry.create(d["type"], json::object(), normalized);
        assert(normalized == d["defaults"]);
        float samples[4] = {0.01f, -0.01f, 0.0f, 0.02f};
        p->process(samples, 4, 1);
        for (const auto sample : samples) assert(std::isfinite(sample));
    }
    json normalized;
    auto gain = registry.create("gain", {{"gain", 0.0}}, normalized);
    float samples[] = {1.0f, -1.0f};
    gain->process(samples, 2, 1);
    assert(samples[0] == 0 && samples[1] == 0);
    auto compressor = registry.create("compressor", {{"threshold", 0.5}, {"attack", 0.0001}}, normalized);
    float signal = 1.0f;
    for (unsigned i = 0; i < 10000; ++i) { signal = 1.0f; compressor->process(&signal, 1, 1); }
    assert(std::abs(signal - 0.625f) < 0.001f);
    expect_invalid([&] { registry.create("eq", json::object(), normalized); });
    expect_invalid([&] { registry.create("gain", {{"gain", -1}}, normalized); });
    expect_invalid([&] { registry.create("gain", {{"gain", "1"}}, normalized); });
    expect_invalid([&] { registry.create("gain", {{"gain", std::numeric_limits<double>::infinity()}}, normalized); });
    expect_invalid([&] { registry.create("gain", {{"surprise", 1}}, normalized); });
    expect_invalid([&] { registry.create("compressor", {{"threshold", -20}}, normalized); });
    expect_invalid([&] { registry.create("compressor", {{"attack", 0}}, normalized); });

    registry.register_processor({"test_types", "Test typed parameters", "Test-only registration", {
        {"count", "Count", "integer", 2, 0, 10, 1, "", {}},
        {"enabled", "Enabled", "boolean", false, {}, {}, {}, "", {}},
        {"label", "Label", "string", "hello", {}, {}, {}, "", {}},
        {"mode", "Mode", "enum", "a", {}, {}, {}, "", {{"A", "a"}, {"B", "b"}}}},
        [](const json&) { return std::make_unique<PassthroughProcessor>(); }});
    registry.create("test_types", {{"count", 0}, {"enabled", false}, {"mode", "b"}}, normalized);
    assert(normalized["count"] == 0 && normalized["enabled"] == false && normalized["label"] == "hello");
    expect_invalid([&] { registry.create("test_types", {{"count", 1.5}}, normalized); });
    expect_invalid([&] { registry.create("test_types", {{"mode", "c"}}, normalized); });
    expect_invalid([&] { registry.create("test_types", {{"enabled", 0}}, normalized); });

    DeviceManager devices(false); // No backend initialization or physical devices.
    ClientManager clients;
    AudioMetrics metrics;
    AudioControls controls;
    ToneControls tone;
    auto engine = std::make_unique<DspEngine>(devices, clients, metrics, tone);
    clients.set_dsp_engine(engine.get());
    const auto sink = clients.register_node("test/sink");
    AudioRoute route;
    route.source_node = NODE_TONE_GENERATOR;
    route.destination_node = sink;
    uint32_t id = 0;
    std::string error;
    assert(clients.add_route(route, id, error));
    assert(engine->supports_route_dsp(id));
    assert(!engine->supports_route_dsp(1024));

    // Hold a reader across repeated publications. Replacing a chain must never
    // destroy a processor currently executing on the DSP thread.
    std::vector<std::unique_ptr<AudioProcessor>> blocking;
    blocking.push_back(std::make_unique<BlockingProcessor>());
    engine->update_route_dsp(id, std::move(blocking), 1.0f, true);
    engine->start();
    wait_for([] { return entered.load(); });
    for (unsigned i = 0; i < 1000; ++i) {
        std::vector<std::unique_ptr<AudioProcessor>> chain;
        chain.push_back(std::make_unique<CountingProcessor>());
        engine->update_route_dsp(id, std::move(chain), 1.0f, true);
        assert(!destroyed.load());
    }
    release_reader.store(true);
    wait_for([] { return processed.load() > 5; });
    engine->stop();
    // Control-side reclamation is lazy: cycle through the spare slots after the
    // reader unpins, rather than requiring destruction at the first new update.
    for (unsigned i = 0; i < 3; ++i) engine->update_route_dsp(id, {}, 1.0f, true);
    assert(destroyed.load());

    char directoryTemplate[] = "/tmp/cm5audio-plugins-test-XXXXXX";
    char* directory = mkdtemp(directoryTemplate);
    assert(directory);
    const auto settingsPath = std::filesystem::path(directory) / "settings.json";
    setenv("CM5AUDIO_SETTINGS", settingsPath.c_str(), 1);
    setenv("CM5AUDIO_BIND_ADDRESS", "127.0.0.1", 1);
    engine->start();
    WebServer server(metrics, controls, tone, clients, devices, *engine, 28182, 28183, true);
    assert(server.start());
    httplib::Client http("127.0.0.1", 28182);
    http.set_connection_timeout(1);
    http.set_read_timeout(5);
    wait_for([&] { const auto r = http.Get("/api/dsp/available"); return r && r->status == 200; });
    const auto available = response_json(http.Get("/api/dsp/available"), 200);
    assert(available["processors"].size() == 4); // Registered C++ plugin appears automatically.
    const std::string url = "/api/routes/" + std::to_string(id) + "/dsp";
    json body = {{"active", true}, {"gain", 0.0}, {"chain", json::array({{{"type", "gain"}, {"params", {{"gain", 0.25}}}}, {{"type", "compressor"}, {"params", json::object()}}})}};
    const auto accepted = response_json(http.Post(url, body.dump(), "application/json"), 200);
    assert(accepted["gain"] == 0.0);
    assert(accepted["chain"][1]["params"]["threshold"] == 0.5);
    assert(accepted["chain"][1]["params"]["attack"] == 0.01);
    const auto before = response_json(http.Get(url), 200);
    assert(before["chain"] == accepted["chain"]);
    assert(response_json(http.Get("/api/graph"), 200)["routes"][0]["dsp_active"] == true);

    for (const auto& invalid : std::vector<json>{
        {{"chain", json::array({{{"type", "eq"}, {"params", json::object()}}})}},
        {{"chain", json::array({{{"type", "compressor"}, {"params", {{"threshold", -20}}}}})}},
        {{"chain", json::object()}}, {{"active", "yes"}}, {{"gain", -1}},
        {{"chain", json::array({{{"type", "gain"}, {"params", {{"bad", 1}}}}})}}
    }) {
        response_json(http.Post(url, invalid.dump(), "application/json"), 400);
        assert(response_json(http.Get(url), 200) == before);
    }
    json tooLong = json::array();
    for (unsigned i = 0; i <= CM5_MAX_PROCESSORS_PER_ROUTE; ++i)
        tooLong.push_back({{"type", "passthrough"}, {"params", json::object()}});
    response_json(http.Post(url, json({{"chain", tooLong}}).dump(), "application/json"), 400);
    response_json(http.Post("/api/routes/999/dsp", "{}", "application/json"), 404);
    response_json(http.Get("/api/routes/999/dsp"), 404);
    response_json(http.Get("/api/routes/4294967297/dsp"), 400);
    response_json(http.Post("/api/settings/save", "", "application/json"), 200);
    server.stop();

    assert(clients.load_settings(controls, tone));
    json saved;
    { std::ifstream file(settingsPath); file >> saved; }
    assert(saved["routes"][0]["dsp"]["chain"] == accepted["chain"]);
    // A removed/unimplemented saved processor must be preserved, bypassed and
    // reported, not executed as an undisclosed passthrough or abort startup.
    saved["routes"][0]["dsp"]["chain"] = json::array({{{"type", "missing_plugin"}, {"params", json::object()}}});
    { std::ofstream file(settingsPath); file << saved; }
    assert(clients.load_settings(controls, tone));
    WebServer restoredServer(metrics, controls, tone, clients, devices, *engine, 28182, 28183, true);
    engine->start();
    assert(restoredServer.start());
    wait_for([&] { const auto r = http.Get(url); return r && r->status == 200; });
    const auto restored = response_json(http.Get(url), 200);
    assert(restored["active"] == false && restored.contains("warning"));
    assert(restored["chain"][0]["type"] == "missing_plugin");
    restoredServer.stop();
    std::filesystem::remove_all(directory);
    clients.set_dsp_engine(nullptr);
    unsetenv("CM5AUDIO_SETTINGS");
    unsetenv("CM5AUDIO_BIND_ADDRESS");
    std::cout << "PASS: registry, typed validation, DSP processing, reader lifetime, discovery, API rejection, persistence and unavailable-plugin recovery\n";
}
