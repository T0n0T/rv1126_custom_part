#include "crossing_engine.hpp"
#include "botsort_tracker.hpp"
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

namespace {

std::vector<flow_demo::Point> polygon(const json &value, const char *name) {
    if (!value.is_array() || value.size() < 3) throw std::runtime_error(std::string(name) + " must be a polygon array");
    std::vector<flow_demo::Point> result;
    for (const auto &point : value) {
        if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number())
            throw std::runtime_error(std::string(name) + " vertices must be [x,y] numeric pairs");
        result.push_back({point[0].get<double>(), point[1].get<double>()});
    }
    return result;
}

flow_demo::CrossingConfig load_config(const std::string &path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open config: " + path);
    const json value = json::parse(input);
    flow_demo::CrossingConfig result;
    result.session_id = value.at("session_id").get<std::string>();
    result.video = value.at("video").get<std::string>();
    const auto geometry = value.find("geometry_confirmed");
    if (geometry == value.end() || !geometry->is_boolean() || !geometry->get<bool>())
        throw std::runtime_error("real video processing requires geometry_confirmed=true after camera review");
    result.zones.inside.vertices = polygon(value.at("zones").at("inside"), "inside");
    result.zones.transition.vertices = polygon(value.at("zones").at("transition"), "transition");
    result.zones.outside.vertices = polygon(value.at("zones").at("outside"), "outside");
    auto option = value.find("max_observation_gap_ms");
    if (option != value.end()) result.max_observation_gap_ms = option->get<double>();
    option = value.find("merge_window_ms");
    if (option != value.end()) result.merge_window_ms = option->get<double>();
    option = value.find("min_track_score");
    if (option != value.end()) result.min_track_score = option->get<double>();
    return result;
}

std::string required_path(int argc, char **argv, const std::string &key) {
    for (int i = 1; i + 1 < argc; ++i) if (argv[i] == key) return argv[i + 1];
    throw std::runtime_error("missing " + key);
}

void write_json(const std::string &path, const json &value) {
    std::ofstream output(path);
    if (!output) throw std::runtime_error("cannot write: " + path);
    output << value.dump(2) << '\n';
}

json event_json(const flow_demo::CrossingEvent &event) {
    return {{"event_id", event.event_id}, {"session_id", event.session_id},
        {"direction", flow_demo::direction_name(event.direction)}, {"count", event.count},
        {"track_ids", event.track_ids}, {"status", event.status},
        {"start_pts_ms", event.start_pts_ms}, {"end_pts_ms", event.end_pts_ms},
        {"start_frame", event.start_frame}, {"end_frame", event.end_frame},
        {"reason", ""}, {"evidence", {{"video", event.video},
            {"start_pts_ms", event.start_pts_ms}, {"end_pts_ms", event.end_pts_ms}}}};
}

json uncertain_json(const flow_demo::UncertainEvent &event) {
    return {{"event_id", event.event_id}, {"session_id", event.session_id},
        {"track_id", event.track_id}, {"count", 0}, {"track_ids", {event.track_id}},
        {"direction", "unknown"}, {"status", "uncertain"}, {"reason", event.reason},
        {"start_pts_ms", event.start_pts_ms}, {"end_pts_ms", event.end_pts_ms},
        {"start_frame", event.start_frame}, {"end_frame", event.end_frame},
        {"evidence", {{"video", event.video}, {"start_pts_ms", event.start_pts_ms},
            {"end_pts_ms", event.end_pts_ms}}}};
}

json detection_json(const botsort::Detection &detection) {
    return {{"xyxy", {detection.x1, detection.y1, detection.x2, detection.y2}},
        {"score", detection.score}, {"class_id", detection.class_id},
        {"track_id", detection.track_id},
        {"track_state", detection.track_state == botsort::TrackState::kNew ? "new" : "matched"}};
}

void usage(const char *program) {
    std::cerr << "Usage: " << program << " --config zones.json --detections detections.jsonl"
              << " --tracks tracks.jsonl --events events.jsonl --summary summary.json\n";
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 11) { usage(argv[0]); return 2; }
        const auto config_path = required_path(argc, argv, "--config");
        const auto detections_path = required_path(argc, argv, "--detections");
        const auto tracks_path = required_path(argc, argv, "--tracks");
        const auto events_path = required_path(argc, argv, "--events");
        const auto summary_path = required_path(argc, argv, "--summary");
        auto config = load_config(config_path);
        botsort::TrackerConfig tracker_config;
        tracker_config.gmc_method = botsort::GmcMethod::kNone;
        tracker_config.enable_class_constraint = true;
        if (!config.session_id.empty()) {
            std::ifstream config_input(config_path);
            const auto value = json::parse(config_input);
            const auto tracker_option = value.find("tracker");
            if (tracker_option != value.end()) {
                const auto &tracker = *tracker_option;
                auto option = tracker.find("track_low_thresh");
                if (option != tracker.end()) tracker_config.track_low_thresh = option->get<float>();
                option = tracker.find("track_high_thresh");
                if (option != tracker.end()) tracker_config.track_high_thresh = option->get<float>();
                option = tracker.find("new_track_thresh");
                if (option != tracker.end()) tracker_config.new_track_thresh = option->get<float>();
                option = tracker.find("match_thresh");
                if (option != tracker.end()) tracker_config.match_thresh = option->get<float>();
                option = tracker.find("track_buffer");
                if (option != tracker.end()) tracker_config.track_buffer = option->get<int>();
            }
        }
        botsort::BoTSortTracker tracker(tracker_config);
        flow_demo::CrossingEngine crossing(config);
        std::ifstream input(detections_path);
        std::ofstream tracks_output(tracks_path);
        if (!input || !tracks_output) throw std::runtime_error("cannot open detection input or tracks output");
        std::string line;
        std::int64_t previous_frame = -1;
        double previous_pts = -1;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            const json frame = json::parse(line);
            const auto index = frame.at("frame_index").get<std::int64_t>();
            const auto pts_ms = frame.at("pts_ms").get<double>();
            if (index <= previous_frame || !std::isfinite(pts_ms) || pts_ms < previous_pts)
                throw std::runtime_error("input frame indices and source PTS must be monotonic");
            previous_frame = index;
            previous_pts = pts_ms;
            botsort::FrameDetections detections;
            for (const auto &value : frame.at("detections")) {
                botsort::Detection detection;
                detection.x1 = value.at("xyxy").at(0).get<float>();
                detection.y1 = value.at("xyxy").at(1).get<float>();
                detection.x2 = value.at("xyxy").at(2).get<float>();
                detection.y2 = value.at("xyxy").at(3).get<float>();
                detection.score = value.at("score").get<float>();
                detection.class_id = value.at("class_id").get<int>();
                detection.class_name = detection.class_id == 0 ? "helmet" : "head";
                detections.push_back(detection);
            }
            const auto tracked = tracker.update(detections);
            crossing.update(index, pts_ms, tracked.detections);
            json objects = json::array();
            for (const auto &detection : tracked.detections) objects.push_back(detection_json(detection));
            tracks_output << json{{"frame_index", index}, {"pts_ms", pts_ms}, {"detections", objects}}.dump() << '\n';
            if (!tracks_output) throw std::runtime_error("failed writing tracked frame");
        }
        const auto result = crossing.close();
        std::ofstream events_output(events_path);
        if (!events_output) throw std::runtime_error("cannot write events output");
        for (const auto &event : result.confirmed_events) events_output << event_json(event).dump() << '\n';
        for (const auto &event : result.uncertain_events) events_output << uncertain_json(event).dump() << '\n';
        write_json(summary_path, {{"session_id", config.session_id},
            {"confirmed_take_out", result.take_out_count}, {"confirmed_put_in", result.put_in_count},
            {"uncertain_count", result.uncertain_count}, {"result_complete", result.result_complete},
            {"source_video", config.video}, {"processed_through_frame", previous_frame},
            {"processed_through_pts_ms", previous_pts}});
        std::cout << "FLOW_DEMO_OK frames_through=" << previous_frame
                  << " take_out=" << result.take_out_count << " put_in=" << result.put_in_count
                  << " uncertain=" << result.uncertain_count
                  << " result_complete=" << (result.result_complete ? "true" : "false") << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "flow_demo_tracker: " << error.what() << '\n';
        return 1;
    }
}
