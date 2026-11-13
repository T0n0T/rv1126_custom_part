#include "flow_events.h"
#include "crossing_engine.hpp"
#include "botsort_tracker.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

struct flow_events {
    flow_demo::CrossingConfig config;
    std::unique_ptr<botsort::BoTSortTracker> tracker;
    std::unique_ptr<flow_demo::CrossingEngine> crossing;
    std::ofstream tracks_output;
    std::ofstream events_output;
    std::ofstream summary_output;
    std::map<std::string, int> event_counts;
    flow_overlay overlay{};
    size_t uncertain_written = 0;
    int64_t previous_frame = -1;
    double previous_pts = -1;
};

namespace {
void open_new(std::ofstream &output, const std::string &path) {
    FILE *reservation = std::fopen(path.c_str(), "wx");
    if (!reservation) throw std::runtime_error("cannot create new output: " + path);
    std::fclose(reservation);
    output.open(path);
    if (!output) throw std::runtime_error("cannot open output: " + path);
}

flow_demo::Polygon polygon(const json &value, flow_polygon &view) {
    if (!value.is_array() || value.size() < 3 || value.size() > FLOW_ZONE_VERTICES)
        throw std::runtime_error("zone needs 3..32 normalized vertices");
    flow_demo::Polygon result;
    for (const auto &point : value) {
        if (!point.is_array() || point.size() != 2)
            throw std::runtime_error("invalid zone vertex");
        double x = point.at(0).get<double>(), y = point.at(1).get<double>();
        view.xy[view.count][0] = x;
        view.xy[view.count++][1] = y;
        result.vertices.push_back({x, y});
    }
    return result;
}

json event_json(const flow_events &events, const flow_demo::CrossingEvent &event, int delta) {
    return {{"event_id", event.event_id}, {"session_id", event.session_id},
        {"direction", flow_demo::direction_name(event.direction)}, {"count", event.count},
        {"delta_count", delta}, {"op", "upsert"}, {"track_ids", event.track_ids},
        {"status", events.overlay.geometry_confirmed ? "confirmed" : "debug"},
        {"geometry_confirmed", bool(events.overlay.geometry_confirmed)},
        {"start_pts_ms", event.start_pts_ms}, {"end_pts_ms", event.end_pts_ms},
        {"start_frame", event.start_frame}, {"end_frame", event.end_frame},
        {"evidence", {{"source", events.config.video}, {"coordinate_space", "rotated-camera"}}}};
}

void write_uncertain(flow_events &events, const flow_demo::UncertainEvent &event) {
    events.events_output << json{{"event_id", event.event_id}, {"session_id", event.session_id},
        {"direction", "unknown"}, {"status", "uncertain"}, {"count", 0},
        {"geometry_confirmed", bool(events.overlay.geometry_confirmed)},
        {"track_ids", {event.track_id}}, {"reason", event.reason},
        {"start_pts_ms", event.start_pts_ms}, {"end_pts_ms", event.end_pts_ms},
        {"start_frame", event.start_frame}, {"end_frame", event.end_frame}}.dump() << '\n';
    ++events.uncertain_written;
    ++events.overlay.uncertain;
    std::snprintf(events.overlay.last_event, sizeof(events.overlay.last_event), "UNCERTAIN ID %d", event.track_id);
    std::snprintf(events.overlay.reason, sizeof(events.overlay.reason), "%s", event.reason.c_str());
}
}

extern "C" struct flow_events *flow_events_open(const char *path, const char *session_id,
    const char *directory, int rotation, int width, int height, int allow_debug) {
    try {
        auto result = std::make_unique<flow_events>();
        std::ifstream input(path);
        if (!input) throw std::runtime_error("cannot open camera zones profile");
        auto value = json::parse(input);
        auto &overlay = result->overlay;
        overlay.enabled = 1;
        overlay.geometry_confirmed = value.at("geometry_confirmed").get<bool>();
        if (!overlay.geometry_confirmed && !allow_debug)
            throw std::runtime_error("unconfirmed camera geometry requires explicit --debug-zones");
        if (value.at("coordinate_space") != "rotated-camera" || value.at("camera_rotation") != rotation ||
            value.at("source_width") != width || value.at("source_height") != height)
            throw std::runtime_error("zones coordinate space, camera rotation or dimensions do not match this camera");
        auto &config = result->config;
        config.session_id = session_id;
        config.video = "camera://" + value.value("camera_id", std::string("board-0"));
        config.zones.inside = polygon(value.at("zones").at("inside"), overlay.zones[0]);
        config.zones.transition = polygon(value.at("zones").at("transition"), overlay.zones[1]);
        config.zones.outside = polygon(value.at("zones").at("outside"), overlay.zones[2]);
        config.max_observation_gap_ms = value.value("max_observation_gap_ms", 400.0);
        config.merge_window_ms = value.value("merge_window_ms", 1000.0);
        config.min_track_score = value.value("min_track_score", 0.1);
        botsort::TrackerConfig tracker_config;
        tracker_config.gmc_method = botsort::GmcMethod::kNone;
        tracker_config.enable_class_constraint = true;
        if (value.contains("tracker")) {
            const auto &tracker = value.at("tracker");
            tracker_config.track_low_thresh = tracker.value("track_low_thresh", 0.1F);
            tracker_config.track_high_thresh = tracker.value("track_high_thresh", 0.6F);
            tracker_config.new_track_thresh = tracker.value("new_track_thresh", 0.7F);
            tracker_config.match_thresh = tracker.value("match_thresh", 0.8F);
            tracker_config.track_buffer = tracker.value("track_buffer", 30);
        }
        result->tracker = std::make_unique<botsort::BoTSortTracker>(tracker_config);
        result->crossing = std::make_unique<flow_demo::CrossingEngine>(config);
        open_new(result->tracks_output, std::string(directory) + "/tracks.jsonl");
        open_new(result->events_output, std::string(directory) + "/events.jsonl");
        open_new(result->summary_output, std::string(directory) + "/summary.json");
        std::snprintf(overlay.last_event, sizeof(overlay.last_event), "WAITING FOR CROSSING");
        std::fprintf(stderr, "FLOW_EVENTS_READY mode=%s profile=%s gap_ms=%.1f\n",
            overlay.geometry_confirmed ? "CALIBRATED" : "DEBUG", path, config.max_observation_gap_ms);
        return result.release();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "flow_events: %s\n", error.what());
        return nullptr;
    }
}

extern "C" int flow_events_update(flow_events *events, const frame_metadata *metadata,
    const flow_detection *detections, size_t count, flow_overlay *output) {
    try {
        if (metadata->frame_index <= events->previous_frame || !std::isfinite(metadata->pts_ms) ||
            metadata->pts_ms < events->previous_pts || count > FLOW_MAX_DETECTIONS)
            throw std::runtime_error("nonmonotonic live frame or invalid detection count");
        events->previous_frame = metadata->frame_index;
        events->previous_pts = metadata->pts_ms;
        botsort::FrameDetections input;
        for (size_t i = 0; i < count; ++i) {
            const auto &d = detections[i];
            botsort::Detection object;
            object.x1 = float(d.xyxy[0]); object.y1 = float(d.xyxy[1]);
            object.x2 = float(d.xyxy[2]); object.y2 = float(d.xyxy[3]);
            object.score = d.score; object.class_id = int(d.class_id);
            object.class_name = d.class_id == 0 ? "helmet" : "head";
            object.input_index = int(i);
            input.push_back(object);
        }
        const auto tracked = events->tracker->update(input);
        const auto changes = events->crossing->update(metadata->frame_index, metadata->pts_ms, tracked.detections);
        auto &overlay = events->overlay;
        overlay.pts_ms = metadata->pts_ms;
        overlay.count = count;
        overlay.pending = 0;
        std::map<int, bool> pending;
        for (const auto &state : events->crossing->debug_tracks()) {
            pending[state.track_id] = state.pending;
            if (state.pending) ++overlay.pending;
        }
        for (size_t i = 0; i < count; ++i) {
            overlay.tracks[i] = {};
            overlay.tracks[i].detection = detections[i];
            overlay.tracks[i].track_id = -1;
        }
        json rows = json::array();
        for (const auto &d : tracked.detections) {
            const auto zone = events->crossing->zone_name(d);
            const int code = zone == "inside" ? 1 : zone == "transition" ? 2 : zone == "outside" ? 3 : 0;
            if (d.input_index >= 0 && size_t(d.input_index) < count) {
                auto &view = overlay.tracks[size_t(d.input_index)];
                view.detection = {{d.x1, d.y1, d.x2, d.y2}, d.score, uint32_t(d.class_id)};
                view.track_id = d.track_id;
                view.zone = code;
                view.pending = pending[d.track_id];
            }
            rows.push_back({{"xyxy", {d.x1, d.y1, d.x2, d.y2}}, {"score", d.score},
                {"class_id", d.class_id}, {"track_id", d.track_id}, {"zone", zone},
                {"pending", pending[d.track_id]}});
        }
        events->tracks_output << json{{"frame_index", metadata->frame_index}, {"pts_ms", metadata->pts_ms},
            {"geometry_confirmed", bool(overlay.geometry_confirmed)}, {"detections", rows}}.dump() << '\n';
        for (const auto &event : changes.new_events) {
            int &previous = events->event_counts[event.event_id];
            const int delta = event.count - previous;
            previous = event.count;
            if (event.direction == flow_demo::Direction::kTakeOut) overlay.take_out += delta;
            else overlay.put_in += delta;
            events->events_output << event_json(*events, event, delta).dump() << '\n';
            std::snprintf(overlay.last_event, sizeof(overlay.last_event), "%s +%d ID %d",
                event.direction == flow_demo::Direction::kTakeOut ? "TAKE OUT" : "PUT IN", delta,
                event.track_ids.empty() ? -1 : event.track_ids.back());
            overlay.reason[0] = '\0';
        }
        for (const auto &uncertain : changes.new_uncertain) write_uncertain(*events, uncertain);
        events->tracks_output.flush();
        events->events_output.flush();
        if (!events->tracks_output || !events->events_output) throw std::runtime_error("cannot write live event output");
        *output = overlay;
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "flow_events_update: %s\n", error.what());
        return -1;
    }
}

extern "C" int flow_events_close(flow_events *raw, int run_ok) {
    std::unique_ptr<flow_events> events(raw);
    if (!events) return 0;
    try {
        const auto result = events->crossing->close();
        for (size_t i = events->uncertain_written; i < result.uncertain_events.size(); ++i)
            write_uncertain(*events, result.uncertain_events[i]);
        const bool confirmed = bool(events->overlay.geometry_confirmed);
        events->summary_output << json{{"session_id", events->config.session_id},
            {"geometry_confirmed", confirmed}, {"mode", confirmed ? "calibrated" : "debug"},
            {"confirmed_take_out", confirmed ? result.take_out_count : 0},
            {"confirmed_put_in", confirmed ? result.put_in_count : 0},
            {"debug_take_out", confirmed ? 0 : result.take_out_count},
            {"debug_put_in", confirmed ? 0 : result.put_in_count}, {"uncertain_count", result.uncertain_count},
            {"run_ok", bool(run_ok)},
            {"result_complete", confirmed && run_ok && events->previous_frame >= 0 && result.result_complete},
            {"processed_through_frame", events->previous_frame}, {"processed_through_pts_ms", events->previous_pts}}.dump(2) << '\n';
        events->events_output.flush(); events->summary_output.flush();
        if (!events->events_output || !events->summary_output) throw std::runtime_error("failed writing event summary");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "flow_events_close: %s\n", error.what());
        return -1;
    }
}
