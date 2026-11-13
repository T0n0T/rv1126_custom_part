#pragma once

#include "botsort_types.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace flow_demo {

struct Point { double x = 0.0; double y = 0.0; };
struct Polygon { std::vector<Point> vertices; };
struct ZoneConfig { Polygon inside; Polygon transition; Polygon outside; };

struct CrossingConfig {
    std::string session_id;
    std::string video;
    ZoneConfig zones;
    double max_observation_gap_ms = 500.0;
    double merge_window_ms = 1000.0;
    double min_track_score = 0.1;
};

enum class Direction { kTakeOut, kPutIn };

struct CrossingEvent {
    std::string event_id;
    std::string session_id;
    Direction direction = Direction::kTakeOut;
    int count = 0;
    std::vector<int> track_ids;
    std::string status = "confirmed";
    double start_pts_ms = 0.0;
    double end_pts_ms = 0.0;
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0;
    std::string video;
};

struct UncertainEvent {
    std::string event_id;
    std::string session_id;
    int track_id = -1;
    double start_pts_ms = 0.0;
    double end_pts_ms = 0.0;
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0;
    std::string reason;
    std::string video;
};

struct SessionSummary {
    int take_out_count = 0;
    int put_in_count = 0;
    int uncertain_count = 0;
    bool result_complete = true;
    std::vector<CrossingEvent> confirmed_events;
    std::vector<UncertainEvent> uncertain_events;
};

struct CrossingUpdate {
    std::vector<CrossingEvent> new_events;
    std::vector<UncertainEvent> new_uncertain;
};

struct TrackZoneDebug {
    int track_id = -1;
    bool pending = false;
    std::string last_zone;
};

class CrossingEngine {
public:
    explicit CrossingEngine(CrossingConfig config);
    CrossingUpdate update(std::int64_t frame_index, double pts_ms,
                          const botsort::FrameDetections &detections);
    SessionSummary close();
    std::string zone_name(const botsort::Detection &detection) const;
    std::vector<TrackZoneDebug> debug_tracks() const;

    static std::optional<std::string> validate_config(const CrossingConfig &config);

private:
    enum class Zone { kInside, kTransition, kOutside };
    struct TrackState {
        std::optional<Zone> last_zone;
        std::optional<Zone> last_side;
        bool pending = false;
        double crossing_start_pts_ms = 0.0;
        std::int64_t crossing_start_frame = 0;
        double last_seen_pts_ms = 0.0;
        std::int64_t last_seen_frame = 0;
    };

    std::optional<Zone> classify(const botsort::Detection &detection) const;
    void mark_uncertain(int track_id, const TrackState &state, double pts_ms,
                        std::int64_t frame_index, const std::string &reason,
                        CrossingUpdate *update);
    void confirm_crossing(int track_id, Zone from, Zone to, double pts_ms,
                          std::int64_t frame_index, const TrackState &state,
                          CrossingUpdate *update);

    CrossingConfig config_;
    std::map<int, TrackState> tracks_;
    std::vector<CrossingEvent> events_;
    std::vector<UncertainEvent> uncertain_;
    std::int64_t last_frame_index_ = -1;
    double last_pts_ms_ = -1.0;
    bool closed_ = false;
};

const char *direction_name(Direction direction) noexcept;

} // namespace flow_demo
