#include "crossing_engine.hpp"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

flow_demo::CrossingConfig config() {
    flow_demo::CrossingConfig value;
    value.session_id = "session-1";
    value.video = "cabinet.mp4";
    value.max_observation_gap_ms = 400.0;
    value.merge_window_ms = 1000.0;
    value.min_track_score = 0.1;
    value.zones.inside.vertices = {{0.0, 0.0}, {0.3, 0.0}, {0.3, 1.0}, {0.0, 1.0}};
    value.zones.transition.vertices = {{0.3, 0.0}, {0.6, 0.0}, {0.6, 1.0}, {0.3, 1.0}};
    value.zones.outside.vertices = {{0.6, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.6, 1.0}};
    return value;
}

botsort::Detection detection(int track_id, float center_x, float score = 0.9F, int class_id = 0) {
    botsort::Detection result;
    result.x1 = center_x - 0.04F;
    result.x2 = center_x + 0.04F;
    result.y1 = 0.4F;
    result.y2 = 0.6F;
    result.score = score;
    result.class_id = class_id;
    result.class_name = class_id == 0 ? "helmet" : "head";
    result.track_id = track_id;
    return result;
}

flow_demo::CrossingUpdate step(flow_demo::CrossingEngine &engine, std::int64_t frame,
                                double pts, int track_id, float x) {
    return engine.update(frame, pts, {detection(track_id, x)});
}

void test_inside_transition_outside_confirms_take_out() {
    flow_demo::CrossingEngine engine(config());
    step(engine, 1, 0, 7, 0.15F);
    step(engine, 2, 100, 7, 0.45F);
    const auto update = step(engine, 3, 200, 7, 0.8F);
    assert(update.new_events.size() == 1);
    assert(update.new_events[0].direction == flow_demo::Direction::kTakeOut);
    assert(update.new_events[0].count == 1);
    assert(update.new_events[0].track_ids == std::vector<int>{7});
    assert(update.new_events[0].start_pts_ms == 100);
    assert(update.new_events[0].end_pts_ms == 200);
    const auto summary = engine.close();
    assert(summary.result_complete);
    assert(summary.take_out_count == 1);
    assert(summary.put_in_count == 0);
    assert(summary.uncertain_count == 0);
}

void test_reverse_crossing_and_round_trip_are_counted_separately() {
    flow_demo::CrossingEngine engine(config());
    step(engine, 1, 0, 1, 0.15F);
    step(engine, 2, 100, 1, 0.45F);
    auto a = step(engine, 3, 200, 1, 0.8F);
    auto b = step(engine, 4, 300, 1, 0.45F);
    auto c = step(engine, 5, 400, 1, 0.15F);
    assert(a.new_events.size() == 1);
    assert(b.new_events.empty());
    assert(c.new_events.size() == 1);
    assert(c.new_events[0].direction == flow_demo::Direction::kPutIn);
    assert(engine.close().take_out_count == 1);
    assert(engine.close().put_in_count == 1);
}

void test_unobserved_transition_is_uncertain_never_confirmed() {
    flow_demo::CrossingEngine engine(config());
    step(engine, 1, 0, 1, 0.15F);
    const auto update = step(engine, 2, 100, 1, 0.8F);
    assert(update.new_events.empty());
    assert(update.new_uncertain.size() == 1);
    assert(update.new_uncertain[0].reason == "transition_not_observed");
    const auto summary = engine.close();
    assert(!summary.result_complete);
    assert(summary.take_out_count == 0);
    assert(summary.uncertain_count == 1);
}

void test_turn_back_in_transition_is_uncertain() {
    flow_demo::CrossingEngine engine(config());
    step(engine, 1, 0, 1, 0.15F);
    step(engine, 2, 100, 1, 0.45F);
    const auto update = step(engine, 3, 200, 1, 0.15F);
    assert(update.new_events.empty());
    assert(update.new_uncertain.size() == 1);
    assert(update.new_uncertain[0].reason == "reversed_before_crossing");
}

void test_large_observation_gap_and_session_end_do_not_invent_crossings() {
    flow_demo::CrossingEngine engine(config());
    step(engine, 1, 0, 1, 0.15F);
    step(engine, 2, 100, 1, 0.45F);
    const auto gap = engine.update(3, 501, {});
    assert(gap.new_events.empty());
    assert(gap.new_uncertain.size() == 1);
    assert(gap.new_uncertain[0].reason == "observation_gap_exceeded");

    flow_demo::CrossingEngine closing_engine(config());
    step(closing_engine, 1, 0, 2, 0.15F);
    step(closing_engine, 2, 100, 2, 0.45F);
    const auto summary = closing_engine.close();
    assert(!summary.result_complete);
    assert(summary.uncertain_count == 1);
    assert(summary.uncertain_events[0].reason == "session_closed_mid_crossing");
}

void test_disappearing_track_outside_transition_invalidates_session() {
    flow_demo::CrossingEngine engine(config());
    step(engine, 1, 0, 1, 0.15F);
    const auto gap = engine.update(2, 501, {});
    assert(gap.new_events.empty());
    assert(gap.new_uncertain.size() == 1);
    assert(gap.new_uncertain[0].reason == "observation_gap_exceeded");
    const auto summary = engine.close();
    assert(!summary.result_complete);
    assert(summary.take_out_count == 0);
}

void test_same_direction_tracks_merge_inside_window() {
    flow_demo::CrossingEngine engine(config());
    step(engine, 1, 0, 1, 0.15F);
    step(engine, 2, 100, 1, 0.45F);
    step(engine, 3, 200, 1, 0.8F);
    step(engine, 4, 300, 2, 0.15F);
    step(engine, 5, 400, 2, 0.45F);
    const auto update = step(engine, 6, 500, 2, 0.8F);
    assert(update.new_events.size() == 1);
    assert(update.new_events[0].count == 2);
    assert(update.new_events[0].track_ids == (std::vector<int>{1, 2}));
}

void test_non_helmet_observations_do_not_create_actions() {
    flow_demo::CrossingEngine engine(config());
    step(engine, 1, 0, 1, 0.15F);
    engine.update(2, 100, {detection(1, 0.45F, 0.9F, 1)});
    const auto update = engine.update(3, 200, {detection(1, 0.8F, 0.9F, 1)});
    assert(update.new_events.empty());
    assert(engine.close().take_out_count == 0);
}

void test_invalid_pts_and_zone_geometry_are_rejected() {
    flow_demo::CrossingEngine engine(config());
    step(engine, 1, 100, 1, 0.15F);
    bool backwards_rejected = false;
    try { engine.update(2, 99, {}); } catch (const std::invalid_argument &) { backwards_rejected = true; }
    assert(backwards_rejected);

    auto invalid = config();
    invalid.zones.inside.vertices[0].x = 1.1;
    bool zone_rejected = false;
    try { flow_demo::CrossingEngine bad(invalid); } catch (const std::invalid_argument &) { zone_rejected = true; }
    assert(zone_rejected);
}

void test_zone_boundaries_are_ambiguous_not_assigned() {
    flow_demo::CrossingEngine engine(config());
    auto update = engine.update(1, 0, {detection(1, 0.3F)});
    assert(update.new_events.empty());
    assert(engine.close().result_complete);
}

} // namespace

int main() {
    test_inside_transition_outside_confirms_take_out();
    test_reverse_crossing_and_round_trip_are_counted_separately();
    test_unobserved_transition_is_uncertain_never_confirmed();
    test_turn_back_in_transition_is_uncertain();
    test_large_observation_gap_and_session_end_do_not_invent_crossings();
    test_disappearing_track_outside_transition_invalidates_session();
    test_same_direction_tracks_merge_inside_window();
    test_non_helmet_observations_do_not_create_actions();
    test_invalid_pts_and_zone_geometry_are_rejected();
    test_zone_boundaries_are_ambiguous_not_assigned();
}
