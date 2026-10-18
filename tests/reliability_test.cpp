#include "dsp_engine.h"
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <sys/resource.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using json = nlohmann::json;
using namespace std::chrono_literals;

struct DspEngineTestAccess {
    static void block(DspEngine& engine) { engine.process_audio(); }
    static float sample(DspEngine& engine, NodeId node, size_t frame = 0) {
        return engine.get_buffer(node)[frame * engine.m_nodeWidths[node]];
    }
    static void hook(DspEngine& engine, std::function<void()> hook) {
        assert(!engine.is_running());
        engine.m_rebuildGraphTestHook = std::move(hook);
    }
};

namespace {
class AddOne : public AudioProcessor {
public:
    void process(float* buffer, size_t frames, unsigned channels) override {
        for (size_t i = 0; i < frames * channels; ++i) buffer[i] += 1.0f;
    }
};
json response(const httplib::Result& result, int expected = 200) {
    if (!result || result->status != expected) {
        std::cerr << "Expected " << expected << ", got " << (result ? std::to_string(result->status) + ": " + result->body : "no response") << '\n';
        std::abort();
    }
    return json::parse(result->body);
}
std::shared_ptr<WebClientSession> session(ClientManager& clients, uint32_t id, const std::string& key) {
    auto s = clients.create_session(id);
    s->input_channels.store(1);
    s->output_channels.store(1);
    assert(s->incoming_rb.init(ma_format_f32, 1, 48000));
    assert(s->outgoing_rb.init(ma_format_f32, 1, 48000));
    s->media_ready.store(true);
    assert(clients.claim_identity(s, key, key));
    return s;
}
struct Rig {
    DeviceManager devices{false};
    AudioMetrics metrics;
    AudioControls controls;
    ToneControls tone;
    ClientManager clients;
    std::unique_ptr<DspEngine> engine = std::make_unique<DspEngine>(devices, clients, metrics, tone);
    Rig() { clients.set_device_manager(&devices); clients.set_dsp_engine(engine.get()); }
    ~Rig() { engine->stop(); clients.stop_sessions(); clients.set_dsp_engine(nullptr); }
};
void ring_test() {
    PcmRingBuffer rb;
    assert(rb.init(ma_format_f32, 1, 5));
    float a[]{1,2,3,4}, out[8]{};
    assert(rb.write(a, sizeof(a)) == sizeof(a));
    assert(rb.read(out, 3 * sizeof(float)) == 3 * sizeof(float));
    float b[]{5,6,7,8};
    assert(rb.write(b, sizeof(b)) == sizeof(b));
    assert(rb.read(out, sizeof(out)) == 5 * sizeof(float));
    for (int i = 0; i < 5; ++i) assert(out[i] == 4 + i);
    assert(rb.init(ma_format_f32, 2, 8));
    assert(rb.read(out, sizeof(out)) == 0);
    assert(!rb.init(ma_format_f32, 0, 8));
    assert(rb.write(a, sizeof(a)) == 0);
}
void deterministic_graph_and_samples() {
    Rig rig;
    auto source = session(rig.clients, 1, "source");
    auto sinkSession = session(rig.clients, 2, "sink/capture/nested");
    auto other = session(rig.clients, 3, "sink");
    const auto src = rig.clients.get_node_id("client/source/capture");
    const auto sink = rig.clients.get_node_id("client/sink/capture/nested/playback");
    AudioRoute route; route.source_node = src; route.destination_node = sink;
    uint32_t id; std::string error;
    assert(rig.clients.add_route(route, id, error));
    auto block = [&](float expected) {
        float input[CM5_BUFFER_FRAMES], output[CM5_BUFFER_FRAMES];
        std::fill(std::begin(input), std::end(input), 1.0f);
        assert(source->incoming_rb.write(input, sizeof(input)) == sizeof(input));
        DspEngineTestAccess::block(*rig.engine);
        assert(sinkSession->outgoing_rb.read(output, sizeof(output)) == sizeof(output));
        for (float value : output) assert(std::abs(value - expected) < 0.00001f);
        assert(other->outgoing_rb.read(output, sizeof(output)) == sizeof(output));
        for (float value : output) assert(value == 0.0f);
    };
    block(1.0f);
    assert(rig.clients.update_route(id, 0.5f, true));
    // Publish B after rebuild captured A. A's completion must not erase B.
    bool changed = false;
    DspEngineTestAccess::hook(*rig.engine, [&] {
        if (!changed) { changed = true; assert(rig.clients.update_route(id, 0.25f, true)); }
    });
    block(0.5f);
    assert(rig.engine->adopted_revision() < rig.engine->requested_revision());
    block(0.25f);
    assert(rig.engine->adopted_revision() == rig.engine->requested_revision());
    DspEngineTestAccess::hook(*rig.engine, {});
    std::vector<std::unique_ptr<AudioProcessor>> chain;
    chain.push_back(std::make_unique<GainProcessor>(0.5f));
    rig.engine->update_route_dsp(id, std::move(chain), 0.5f, true);
    rig.engine->request_adoption();
    block(0.0625f);
    std::vector<std::unique_ptr<AudioProcessor>> ordered;
    ordered.push_back(std::make_unique<AddOne>());
    ordered.push_back(std::make_unique<GainProcessor>(0.5f));
    rig.engine->update_route_dsp(id, std::move(ordered), 1.0f, true);
    rig.engine->request_adoption();
    block(0.25f);
    ordered.push_back(std::make_unique<GainProcessor>(0.5f));
    ordered.push_back(std::make_unique<AddOne>());
    rig.engine->update_route_dsp(id, std::move(ordered), 1.0f, true);
    rig.engine->request_adoption();
    block(0.375f); // Reordering noncommuting processors changes actual samples.
    rig.engine->update_route_dsp(id, {}, 0.0f, false);
    rig.engine->request_adoption();
    block(0.25f); // Bypass keeps ordinary route gain, not DSP gain.
    assert(rig.clients.update_route(id, 0.25f, false));
    DspEngineTestAccess::block(*rig.engine);
    assert(DspEngineTestAccess::sample(*rig.engine, sink) == 0);
    assert(rig.clients.update_route(id, 0.25f, true));
    // A partial capture read must zero the rest, not repeat the preceding block.
    float one = 1, partial[CM5_BUFFER_FRAMES]{};
    source->incoming_rb.read(partial, sizeof(partial));
    source->incoming_rb.write(&one, sizeof(one));
    DspEngineTestAccess::block(*rig.engine);
    assert(DspEngineTestAccess::sample(*rig.engine, sink, 0) == 0.25f);
    assert(DspEngineTestAccess::sample(*rig.engine, sink, 1) == 0.0f);
    const auto original = sink;
    std::weak_ptr<WebClientSession> oldOwner = sinkSession;
    rig.clients.remove_session(2);
    rig.clients.collect_retired_sessions();
    sinkSession.reset();
    assert(!oldOwner.expired()); // Current graph still owns its raw handler.
    auto reconnected = session(rig.clients, 4, "sink/capture/nested");
    assert(rig.clients.get_node_id("client/sink/capture/nested/playback") == original);
    DspEngineTestAccess::block(*rig.engine);
    assert(!oldOwner.expired()); // Rebuild queued final reclamation, not audio destruction.
    rig.engine->collect_retired_endpoints();
    assert(oldOwner.expired());
    assert(rig.clients.remove_route(id));
    DspEngineTestAccess::block(*rig.engine);
}
void persistence_and_http(const std::filesystem::path& path) {
    setenv("CM5AUDIO_SETTINGS", path.c_str(), 1);
    json expected;
    {
        Rig rig;
        auto sink = session(rig.clients, 10, "offline/sink");
        rig.engine->start();
        WebServer server(rig.metrics, rig.controls, rig.tone, rig.clients, rig.devices, *rig.engine, 28184, 28185, true);
        assert(server.start());
        httplib::Client http("127.0.0.1", 28184); http.set_read_timeout(4);
        // Dynamic meter API widths and bounds: no eight-channel truncation and
        // no out-of-bounds indexing on a missing or too-short meter bank.
        {
            std::lock_guard<std::mutex> lock(rig.metrics.lock);
            rig.metrics.playback["synthetic/wide"] = std::vector<ChannelMeter>(32);
            rig.metrics.playback["synthetic/wide"][31].raw_peak.store(0.25f);
        }
        auto wideMeters = response(http.Get("/api/meters"));
        assert(wideMeters["playback"]["synthetic/wide"].size() == 32);
        assert(response(http.Get("/api/raw"))["playback"]["synthetic/wide"][31]["peak"] == 0.25f);
        response(http.Post("/api/reset_clips?endpoint=synthetic%2Fwide&type=playback&ch=31", "", "application/json"));
        response(http.Post("/api/reset_clips?endpoint=synthetic%2Fwide&type=playback&ch=32", "", "application/json"), 400);
        response(http.Post("/api/reset_clips?endpoint=missing&type=playback&ch=0", "", "application/json"), 400);
        response(http.Post("/api/devices/activate?id=missing&capture_channels=255", "", "application/json"), 400);
        response(http.Post("/api/devices/activate?id=missing&capture_channels=-1", "", "application/json"), 400);
        rig.clients.set_device_channels("offline-card", 12, 32);
        const auto created = response(http.Post("/api/routes?source_node=tone%2Fgenerator&destination_node=client%2Foffline%2Fsink%2Fplayback&source_channel=0&destination_channel=0", "", "application/json"));
        assert(created["adopted"] == true);
        const std::string id = created["id"];
        const auto url = "/api/routes/" + id;
        const auto draft = json{{"gain", 0.5}, {"enabled", true}, {"dsp", {{"active", true}, {"gain", 0.5}, {"chain", json::array({{{"type", "gain"}, {"params", {{"gain", 0.25}}}}})}}}};
        const auto applied = response(http.Post(url + "/apply", draft.dump(), "application/json"));
        assert(applied["adopted"] == true);
        auto invalid = draft; invalid["gain"] = 0.1; invalid["dsp"]["chain"][0]["params"]["gain"] = -1;
        response(http.Post(url + "/apply", invalid.dump(), "application/json"), 400);
        assert(rig.clients.get_routes()[0].gain == 0.5f);
        response(http.Post("/api/tone?enabled=1&frequency=1000&amplitude=0.1", "", "application/json"));
        response(http.Post("/api/dsp?gain=0.7&threshold=0.5&ratio=3", "", "application/json"));
        response(http.Post("/api/control?endpoint=x&type=playback&ch=0&gain=0", "", "application/json"), 501);
        // Going offline retains logical route identity and must save it unchanged.
        rig.clients.remove_session(10);
        std::vector<std::thread> writers;
        for (int t = 0; t < 6; ++t) writers.emplace_back([&] {
            httplib::Client concurrent("127.0.0.1", 28184); concurrent.set_read_timeout(5);
            for (int n = 0; n < 12; ++n) {
                const auto saved = response(concurrent.Post("/api/settings/save", "", "application/json"));
                assert(saved["adopted"] == true && saved["persisted"] == true);
                std::ifstream file(path); auto j = json::parse(file);
                assert(j["routes"][0]["destination"] == "client/offline/sink/playback");
            }
        });
        for (auto& writer : writers) writer.join();
        { std::ifstream file(path); file >> expected; }
        // Failure leaves prior destination intact; no false status:ok.
        const auto bad = path.parent_path() / "missing" / "settings.json";
        setenv("CM5AUDIO_SETTINGS", bad.c_str(), 1);
        const auto failed = response(http.Post("/api/settings/save", "", "application/json"), 500);
        assert(failed["adopted"] == true && failed["persisted"] == false);
        { std::ifstream file(path); assert(json::parse(file) == expected); }
        setenv("CM5AUDIO_SETTINGS", path.c_str(), 1);
        // Force a short filesystem write after a successful open. The previous
        // destination must survive and the API must not return status:ok.
        struct rlimit previous{}; assert(getrlimit(RLIMIT_FSIZE, &previous) == 0);
        auto ignored = std::signal(SIGXFSZ, SIG_IGN);
        struct rlimit limited = previous; limited.rlim_cur = 32;
        assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
        response(http.Post("/api/settings/save", "", "application/json"), 500);
        assert(setrlimit(RLIMIT_FSIZE, &previous) == 0);
        std::signal(SIGXFSZ, ignored);
        { std::ifstream file(path); assert(json::parse(file) == expected); }
        for (const auto& entry : std::filesystem::directory_iterator(path.parent_path()))
            assert(entry.path().filename().string().find(".tmp.") == std::string::npos);
        rig.engine->stop();
        const auto pending = response(http.Post("/api/settings/save", "", "application/json"), 503);
        assert(pending["adopted"] == false && pending["persisted"] == false);
        server.stop(); server.stop();
    }
    {
        Rig fresh;
        assert(fresh.clients.load_settings(fresh.controls, fresh.tone));
        assert(fresh.clients.get_routes().size() == 1);
        assert((fresh.clients.device_channels("offline-card") == std::pair<unsigned, unsigned>{12, 32}));
        const auto restored = fresh.clients.get_routes()[0];
        assert(fresh.clients.get_endpoint_from_id(restored.destination_node) == "client/offline/sink/playback");
        assert(fresh.engine->get_gain_processor()->get_gain() == 0.7f);
        auto sink = session(fresh.clients, 11, "offline/sink");
        DspEngineTestAccess::block(*fresh.engine);
        float output[CM5_BUFFER_FRAMES]{};
        assert(sink->outgoing_rb.read(output, sizeof(output)) == sizeof(output));
        bool heard = false;
        for (float value : output) if (std::abs(value) > 0.00001f) heard = true;
        assert(heard); // Restore reached the sample path, not merely JSON cache.
        assert(fresh.clients.save_settings(fresh.controls, fresh.tone));
    }
    unsetenv("CM5AUDIO_SETTINGS");
}
}
int main() {
    char pattern[] = "/tmp/cm5audio-reliability-XXXXXX";
    const auto directory = mkdtemp(pattern); assert(directory);
    setenv("CM5AUDIO_BIND_ADDRESS", "127.0.0.1", 1);
    ring_test();
    deterministic_graph_and_samples();
    persistence_and_http(std::filesystem::path(directory) / "settings.json");
    std::filesystem::remove_all(directory);
    std::cout << "PASS: ring wrap/RAII, deterministic rebuild race, sample gain/bypass/disable, slash identity, reconnect, HTTP apply/adoption, concurrent saves, save failure, fresh restore\n";
}
