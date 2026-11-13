#include "flow_events.h"
#include <nlohmann/json.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

json load(const fs::path &path) { std::ifstream input(path); return json::parse(input); }

flow_detection detection(double x, double y) {
    return {{x - 0.1, y - 0.08, x + 0.1, y + 0.08}, 0.95F, 0};
}

struct Session {
    fs::path directory;
    flow_events *events;
    flow_overlay overlay{};
    frame_metadata metadata{};
    Session(const fs::path &root, const char *name, bool confirmed, bool debug = true) {
        directory = root / name;
        fs::create_directory(directory);
        json value = {
            {"camera_id", "test"}, {"geometry_confirmed", confirmed},
            {"coordinate_space", "rotated-camera"}, {"camera_rotation", 90},
            {"source_width", 360}, {"source_height", 640},
            {"zones", {{"inside", {{0,0}, {0.3,0}, {0.3,1}, {0,1}}},
                       {"transition", {{0.3,0}, {0.6,0}, {0.6,1}, {0.3,1}}},
                       {"outside", {{0.6,0}, {1,0}, {1,1}, {0.6,1}}}}},
            {"max_observation_gap_ms", 400}};
        std::ofstream(directory / "zones.json") << value;
        events = flow_events_open((directory / "zones.json").c_str(), name, directory.c_str(), 90, 360, 640, debug);
    }
    void step(const std::vector<flow_detection> &objects, double increment = 100) {
        ++metadata.frame_index; metadata.pts_ms += increment;
        assert(flow_events_update(events, &metadata, objects.data(), objects.size(), &overlay) == 0);
    }
    void close(bool run_ok = true) { assert(flow_events_close(events, run_ok) == 0); events = nullptr; }
};

void round_trip(const fs::path &root, bool confirmed) {
    Session session(root, confirmed ? "calibrated" : "debug", confirmed);
    assert(session.events);
    for (int i = 0; i < 25; ++i) session.step({detection(0.12 + i * 0.03, 0.5)});
    assert(session.overlay.take_out == 1 && session.overlay.put_in == 0);
    assert(session.overlay.tracks[0].track_id >= 0 && session.overlay.tracks[0].zone == 3);
    for (int i = 24; i >= 0; --i) session.step({detection(0.12 + i * 0.03, 0.5)});
    assert(session.overlay.put_in == 1 && session.overlay.uncertain == 0);
    session.close();
    auto summary = load(session.directory / "summary.json");
    assert(summary.at(confirmed ? "confirmed_take_out" : "debug_take_out") == 1);
    assert(summary.at(confirmed ? "confirmed_put_in" : "debug_put_in") == 1);
    assert(summary.at("confirmed_take_out") == (confirmed ? 1 : 0));
    std::ifstream input(session.directory / "events.jsonl");
    std::string row;
    int count = 0;
    while (std::getline(input, row)) {
        auto event = json::parse(row);
        assert(event.at("delta_count") == 1);
        assert(event.at("status") == (confirmed ? "confirmed" : "debug"));
        ++count;
    }
    assert(count == 2);
}

void merged_events(const fs::path &root) {
    Session session(root, "merged", true);
    for (int i = 0; i < 25; ++i)
        session.step({detection(0.12 + i * 0.03, 0.3), detection(0.1 + i * 0.03, 0.7)});
    assert(session.overlay.take_out == 2 && session.overlay.put_in == 0);
    session.close();
    std::ifstream input(session.directory / "events.jsonl");
    std::string row, id;
    int total = 0, final_count = 0;
    while (std::getline(input, row)) {
        auto event = json::parse(row);
        if (id.empty()) id = event.at("event_id");
        assert(id == event.at("event_id").get<std::string>());
        total += event.at("delta_count").get<int>();
        final_count = event.at("count");
    }
    assert(total == 2 && final_count == 2);
}

int main() {
    char pattern[] = "/tmp/flow-live-events-XXXXXX";
    const char *temporary = mkdtemp(pattern);
    assert(temporary);
    fs::path root(temporary);
    round_trip(root, true);
    round_trip(root, false);
    merged_events(root);
    Session refused(root, "refused", false, false);
    assert(!refused.events);
    auto profile = refused.directory / "zones.json";
    assert(!flow_events_open(profile.c_str(), "wrong-rotation", refused.directory.c_str(), 0, 360, 640, true));
    Session gap(root, "gap", true);
    for (int i = 0; i < 10; ++i) gap.step({detection(0.12 + i * 0.03, 0.5)});
    assert(gap.overlay.pending == 1);
    gap.step({}, 501);
    assert(gap.overlay.take_out == 0 && gap.overlay.uncertain > 0);
    assert(std::string(gap.overlay.reason) == "observation_gap_exceeded");
    gap.close();
    Session failed(root, "failed-run", true);
    for (int i = 0; i < 25; ++i) failed.step({detection(0.12 + i * 0.03, 0.5)});
    failed.close(false);
    auto failed_summary = load(failed.directory / "summary.json");
    assert(failed_summary.at("confirmed_take_out") == 1);
    assert(!failed_summary.at("run_ok").get<bool>() && !failed_summary.at("result_complete").get<bool>());
    Session empty(root, "empty-run", true);
    empty.close();
    assert(!load(empty.directory / "summary.json").at("result_complete").get<bool>());
    std::cout << "FLOW_LIVE_EVENTS_TEST_OK " << root << '\n';
}
