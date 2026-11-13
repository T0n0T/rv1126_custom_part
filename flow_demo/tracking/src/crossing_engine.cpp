#include "crossing_engine.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

namespace flow_demo {
namespace {
constexpr double kEpsilon = 1e-6;

bool valid_polygon(const Polygon &polygon) {
    if (polygon.vertices.size() < 3) return false;
    double twice_area = 0.0;
    for (std::size_t i = 0; i < polygon.vertices.size(); ++i) {
        const Point &a = polygon.vertices[i];
        const Point &b = polygon.vertices[(i + 1) % polygon.vertices.size()];
        if (!std::isfinite(a.x) || !std::isfinite(a.y) || a.x < 0 || a.x > 1 || a.y < 0 || a.y > 1)
            return false;
        twice_area += a.x * b.y - b.x * a.y;
    }
    return std::fabs(twice_area) > kEpsilon;
}

bool on_segment(Point p, Point a, Point b) {
    const double cross = (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
    if (std::fabs(cross) > kEpsilon) return false;
    return p.x >= std::min(a.x, b.x) - kEpsilon && p.x <= std::max(a.x, b.x) + kEpsilon &&
           p.y >= std::min(a.y, b.y) - kEpsilon && p.y <= std::max(a.y, b.y) + kEpsilon;
}

bool contains(const Polygon &polygon, Point point) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.vertices.size() - 1; i < polygon.vertices.size(); j = i++) {
        const Point &a = polygon.vertices[j];
        const Point &b = polygon.vertices[i];
        if (on_segment(point, a, b)) return false;
        const bool crosses = (a.y > point.y) != (b.y > point.y) &&
            point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x;
        if (crosses) inside = !inside;
    }
    return inside;
}

} // namespace

const char *direction_name(Direction direction) noexcept {
    return direction == Direction::kTakeOut ? "take_out" : "put_in";
}

std::optional<std::string> CrossingEngine::validate_config(const CrossingConfig &config) {
    if (config.session_id.empty()) return "session_id must not be empty";
    if (config.video.empty()) return "video must not be empty";
    if (!valid_polygon(config.zones.inside)) return "inside polygon must have 3+ normalized vertices and nonzero area";
    if (!valid_polygon(config.zones.transition)) return "transition polygon must have 3+ normalized vertices and nonzero area";
    if (!valid_polygon(config.zones.outside)) return "outside polygon must have 3+ normalized vertices and nonzero area";
    if (!std::isfinite(config.max_observation_gap_ms) || config.max_observation_gap_ms <= 0)
        return "max_observation_gap_ms must be finite and positive";
    if (!std::isfinite(config.merge_window_ms) || config.merge_window_ms < 0)
        return "merge_window_ms must be finite and nonnegative";
    if (!std::isfinite(config.min_track_score) || config.min_track_score < 0 || config.min_track_score > 1)
        return "min_track_score must be in [0,1]";
    return std::nullopt;
}

CrossingEngine::CrossingEngine(CrossingConfig config) : config_(std::move(config)) {
    if (const auto error = validate_config(config_)) throw std::invalid_argument(*error);
}

std::optional<CrossingEngine::Zone> CrossingEngine::classify(const botsort::Detection &detection) const {
    const Point center{(detection.x1 + detection.x2) * 0.5, (detection.y1 + detection.y2) * 0.5};
    std::optional<Zone> found;
    for (const auto &[zone, polygon] : std::vector<std::pair<Zone, const Polygon *>>{
             {Zone::kInside, &config_.zones.inside}, {Zone::kTransition, &config_.zones.transition},
             {Zone::kOutside, &config_.zones.outside}}) {
        if (!contains(*polygon, center)) continue;
        if (found) return std::nullopt;
        found = zone;
    }
    return found;
}

std::string CrossingEngine::zone_name(const botsort::Detection &detection) const {
    const auto zone = classify(detection);
    if (!zone) return "unknown";
    return *zone == Zone::kInside ? "inside" : *zone == Zone::kTransition ? "transition" : "outside";
}

std::vector<TrackZoneDebug> CrossingEngine::debug_tracks() const {
    std::vector<TrackZoneDebug> result;
    for (const auto &[id, state] : tracks_) {
        const char *name = !state.last_zone ? "unknown" : *state.last_zone == Zone::kInside ? "inside" :
            *state.last_zone == Zone::kTransition ? "transition" : "outside";
        result.push_back({id, state.pending, name});
    }
    return result;
}

void CrossingEngine::mark_uncertain(int track_id, const TrackState &state, double pts_ms,
                                    std::int64_t frame_index, const std::string &reason,
                                    CrossingUpdate *update) {
    std::ostringstream id;
    id << config_.session_id << "-uncertain-" << (uncertain_.size() + 1);
    UncertainEvent event{id.str(), config_.session_id, track_id,
        state.pending ? state.crossing_start_pts_ms : state.last_seen_pts_ms, pts_ms,
        state.pending ? state.crossing_start_frame : state.last_seen_frame, frame_index,
        reason, config_.video};
    uncertain_.push_back(event);
    update->new_uncertain.push_back(event);
}

void CrossingEngine::confirm_crossing(int track_id, Zone from, Zone to, double pts_ms,
                                      std::int64_t frame_index, const TrackState &state,
                                      CrossingUpdate *update) {
    const Direction direction = from == Zone::kInside && to == Zone::kOutside
        ? Direction::kTakeOut : Direction::kPutIn;
    if (!events_.empty() && events_.back().direction == direction &&
        pts_ms - events_.back().end_pts_ms <= config_.merge_window_ms) {
        auto &event = events_.back();
        ++event.count;
        if (std::find(event.track_ids.begin(), event.track_ids.end(), track_id) == event.track_ids.end())
            event.track_ids.push_back(track_id);
        event.end_pts_ms = pts_ms;
        event.end_frame = frame_index;
        update->new_events.push_back(event);
        return;
    }
    std::ostringstream id;
    id << config_.session_id << "-event-" << (events_.size() + 1);
    CrossingEvent event{id.str(), config_.session_id, direction, 1, {track_id}, "confirmed",
        state.crossing_start_pts_ms, pts_ms, state.crossing_start_frame, frame_index, config_.video};
    events_.push_back(event);
    update->new_events.push_back(event);
}

CrossingUpdate CrossingEngine::update(std::int64_t frame_index, double pts_ms,
                                      const botsort::FrameDetections &detections) {
    if (closed_) throw std::logic_error("cannot update a closed session");
    if (frame_index < 0 || frame_index <= last_frame_index_) throw std::invalid_argument("frame_index must increase");
    if (!std::isfinite(pts_ms) || (last_frame_index_ >= 0 && pts_ms < last_pts_ms_))
        throw std::invalid_argument("pts_ms must be finite and monotonic");
    last_frame_index_ = frame_index;
    last_pts_ms_ = pts_ms;
    CrossingUpdate update;

    for (auto &[track_id, state] : tracks_) {
        if (pts_ms - state.last_seen_pts_ms > config_.max_observation_gap_ms) {
            if (state.last_zone) mark_uncertain(track_id, state, pts_ms, frame_index, "observation_gap_exceeded", &update);
            state.pending = false;
            state.last_side.reset();
            state.last_zone.reset();
        }
    }

    std::set<int> seen;
    for (const auto &detection : detections) {
        if (detection.track_id < 0 || detection.class_id != 0 || detection.score < config_.min_track_score) continue;
        if (!std::isfinite(detection.x1) || !std::isfinite(detection.x2) ||
            !std::isfinite(detection.y1) || !std::isfinite(detection.y2) ||
            !std::isfinite(detection.score) || detection.x2 <= detection.x1 || detection.y2 <= detection.y1)
            continue;
        if (!seen.insert(detection.track_id).second) throw std::invalid_argument("duplicate track_id in frame");
        auto &state = tracks_[detection.track_id];
        const auto zone = classify(detection);
        state.last_seen_pts_ms = pts_ms;
        state.last_seen_frame = frame_index;
        if (!zone) continue;
        if (!state.last_zone) {
            if (*zone == Zone::kTransition) {
                mark_uncertain(detection.track_id, state, pts_ms, frame_index, "track_started_in_transition", &update);
            } else {
                state.last_side = *zone;
            }
            state.last_zone = *zone;
            continue;
        }
        if (*zone == Zone::kTransition) {
            if (state.last_side && !state.pending) {
                state.pending = true;
                state.crossing_start_pts_ms = state.last_seen_pts_ms;
                state.crossing_start_frame = state.last_seen_frame;
            }
            state.last_zone = *zone;
            continue;
        }
        if (state.last_side && *zone != *state.last_side) {
            if (state.pending && state.last_zone == Zone::kTransition) {
                confirm_crossing(detection.track_id, *state.last_side, *zone, pts_ms, frame_index, state, &update);
            } else {
                mark_uncertain(detection.track_id, state, pts_ms, frame_index, "transition_not_observed", &update);
            }
        } else if (state.pending && state.last_side && *zone == *state.last_side) {
            mark_uncertain(detection.track_id, state, pts_ms, frame_index, "reversed_before_crossing", &update);
        }
        state.pending = false;
        state.last_side = *zone;
        state.last_zone = *zone;
    }
    return update;
}

SessionSummary CrossingEngine::close() {
    if (!closed_) {
        closed_ = true;
        CrossingUpdate ignored;
        for (const auto &[track_id, state] : tracks_) {
            if (state.pending) mark_uncertain(track_id, state, last_pts_ms_, last_frame_index_,
                                              "session_closed_mid_crossing", &ignored);
        }
    }
    SessionSummary summary;
    summary.confirmed_events = events_;
    summary.uncertain_events = uncertain_;
    for (const auto &event : events_) {
        if (event.direction == Direction::kTakeOut) summary.take_out_count += event.count;
        else summary.put_in_count += event.count;
    }
    summary.uncertain_count = static_cast<int>(uncertain_.size());
    summary.result_complete = uncertain_.empty();
    return summary;
}


} // namespace flow_demo
