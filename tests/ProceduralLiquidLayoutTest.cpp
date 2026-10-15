// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/ProceduralLiquidLayout.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
  void require(bool condition, char const* message)
  {
    if (!condition)
    {
      throw std::runtime_error(message);
    }
  }

  void requireClose(float actual, float expected, char const* message)
  {
    if (std::abs(actual - expected) > 0.0001f)
    {
      throw std::runtime_error(message);
    }
  }

  nlohmann::json corridor(std::string name, int priority = 10)
  {
    return {
      {"name", std::move(name)},
      {"shape", "corridor"},
      {"points", {
        {{"u", 0.1}, {"v", 0.2}, {"height", 10.0}},
        {{"u", 0.9}, {"v", 0.2}, {"height", 20.0}}
      }},
      {"half_width_ratio", 0.05},
      {"transition_width_ratio", 0.05},
      {"liquid_type_id", 5},
      {"depth", 0.8},
      {"priority", priority}
    };
  }

  nlohmann::json area(std::string name, int priority = 20)
  {
    return {
      {"name", std::move(name)},
      {"shape", "area"},
      {"points", {
        {{"u", 0.35}, {"v", 0.1}, {"height", 30.0}},
        {{"u", 0.65}, {"v", 0.1}, {"height", 30.0}},
        {{"u", 0.65}, {"v", 0.35}, {"height", 30.0}},
        {{"u", 0.35}, {"v", 0.35}, {"height", 30.0}}
      }},
      {"half_width_ratio", 0.005},
      {"transition_width_ratio", 0.02},
      {"liquid_type_id", 7},
      {"depth", 0.6},
      {"priority", priority}
    };
  }

  nlohmann::json validArguments()
  {
    return {
      {"replace_existing", true},
      {"edge_noise_ratio", 0.0},
      {"features", {area("lake"), corridor("river")}}
    };
  }
}

int main()
{
  auto const parsed = Noggit::Ai::parseProceduralLiquidLayout(validArguments());
  require(parsed.layout.has_value(), "valid liquid layout was rejected");
  require(parsed.error.empty(), "valid liquid layout returned an error");
  require(parsed.layout->replace_existing, "replace_existing was not parsed");
  require(parsed.layout->features.size() == 2,
          "liquid features were not preserved");
  require(parsed.layout->features.front().name == "river"
            && parsed.layout->features.back().name == "lake",
          "liquid features were not sorted by priority");

  auto extra_root = validArguments();
  extra_root["unexpected"] = true;
  require(!Noggit::Ai::parseProceduralLiquidLayout(extra_root).layout,
          "unknown root field was accepted");

  auto bad_replace = validArguments();
  bad_replace["replace_existing"] = 1;
  require(!Noggit::Ai::parseProceduralLiquidLayout(bad_replace).layout,
          "non-boolean replace_existing was accepted");

  auto bad_noise = validArguments();
  bad_noise["edge_noise_ratio"] = 0.051;
  require(!Noggit::Ai::parseProceduralLiquidLayout(bad_noise).layout,
          "out-of-range edge noise was accepted");

  auto non_finite_noise = validArguments();
  non_finite_noise["edge_noise_ratio"]
    = std::numeric_limits<double>::infinity();
  require(!Noggit::Ai::parseProceduralLiquidLayout(non_finite_noise).layout,
          "non-finite edge noise was accepted");

  auto empty_features = validArguments();
  empty_features["features"] = nlohmann::json::array();
  require(!Noggit::Ai::parseProceduralLiquidLayout(empty_features).layout,
          "empty liquid layout was accepted");

  auto too_many_features = validArguments();
  too_many_features["features"] = nlohmann::json::array();
  for (std::size_t index = 0;
       index < Noggit::Ai::procedural_layout_max_features + 1; ++index)
  {
    too_many_features["features"].push_back(
      corridor("feature_" + std::to_string(index)));
  }
  require(!Noggit::Ai::parseProceduralLiquidLayout(too_many_features).layout,
          "liquid layout with more features than the cap was accepted");

  auto too_many_liquid_types = validArguments();
  too_many_liquid_types["features"] = nlohmann::json::array();
  for (int index = 0; index < 15; ++index)
  {
    auto feature = corridor("type_" + std::to_string(index));
    feature["liquid_type_id"] = index + 1;
    too_many_liquid_types["features"].push_back(std::move(feature));
  }
  require(!Noggit::Ai::parseProceduralLiquidLayout(too_many_liquid_types).layout,
          "liquid layout with more than 14 distinct types was accepted");

  auto extra_feature = validArguments();
  extra_feature["features"][0]["unexpected"] = true;
  require(!Noggit::Ai::parseProceduralLiquidLayout(extra_feature).layout,
          "unknown liquid feature field was accepted");

  auto bad_shape = validArguments();
  bad_shape["features"][0]["shape"] = "spline";
  require(!Noggit::Ai::parseProceduralLiquidLayout(bad_shape).layout,
          "unknown liquid shape was accepted");

  auto short_area = validArguments();
  short_area["features"][0]["points"].erase(
    short_area["features"][0]["points"].begin() + 2,
    short_area["features"][0]["points"].end());
  require(!Noggit::Ai::parseProceduralLiquidLayout(short_area).layout,
          "area with fewer than three points was accepted");

  auto crossed_area = validArguments();
  crossed_area["features"][0]["points"] = {
    {{"u", 0.1}, {"v", 0.1}, {"height", 30.0}},
    {{"u", 0.9}, {"v", 0.9}, {"height", 30.0}},
    {{"u", 0.1}, {"v", 0.9}, {"height", 30.0}},
    {{"u", 0.9}, {"v", 0.1}, {"height", 30.0}}
  };
  require(!Noggit::Ai::parseProceduralLiquidLayout(crossed_area).layout,
          "self-intersecting liquid area was accepted");

  auto varying_area = validArguments();
  varying_area["features"][0]["points"][1]["height"] = 31.0;
  require(!Noggit::Ai::parseProceduralLiquidLayout(varying_area).layout,
          "liquid area with varying levels was accepted");

  auto bad_coordinate = validArguments();
  bad_coordinate["features"][1]["points"][0]["u"] = -0.01;
  require(!Noggit::Ai::parseProceduralLiquidLayout(bad_coordinate).layout,
          "out-of-range liquid coordinate was accepted");

  auto bad_height = validArguments();
  bad_height["features"][1]["points"][0]["height"] = 5001.0;
  require(!Noggit::Ai::parseProceduralLiquidLayout(bad_height).layout,
          "out-of-range liquid level was accepted");

  auto non_finite_height = validArguments();
  non_finite_height["features"][1]["points"][0]["height"]
    = std::numeric_limits<double>::quiet_NaN();
  require(!Noggit::Ai::parseProceduralLiquidLayout(non_finite_height).layout,
          "non-finite liquid level was accepted");

  auto duplicate_point = validArguments();
  duplicate_point["features"][1]["points"][1]
    = duplicate_point["features"][1]["points"][0];
  require(!Noggit::Ai::parseProceduralLiquidLayout(duplicate_point).layout,
          "duplicate consecutive liquid points were accepted");

  auto bad_width = validArguments();
  bad_width["features"][1]["half_width_ratio"] = 0.00124;
  require(!Noggit::Ai::parseProceduralLiquidLayout(bad_width).layout,
          "out-of-range liquid half-width was accepted");
  auto minimum_width = validArguments();
  minimum_width["features"][1]["half_width_ratio"] = 0.00125;
  minimum_width["features"][1]["transition_width_ratio"] = 0.00025;
  require(Noggit::Ai::parseProceduralLiquidLayout(minimum_width).layout.has_value(),
          "compact liquid widths were rejected");

  auto bad_transition = validArguments();
  bad_transition["features"][1]["transition_width_ratio"] = 0.00024;
  require(!Noggit::Ai::parseProceduralLiquidLayout(bad_transition).layout,
          "out-of-range liquid transition was accepted");

  auto bad_type = validArguments();
  bad_type["features"][1]["liquid_type_id"] = 65536;
  require(!Noggit::Ai::parseProceduralLiquidLayout(bad_type).layout,
          "liquid type outside MH2O uint16 was accepted");

  auto non_integer_type = validArguments();
  non_integer_type["features"][1]["liquid_type_id"] = 5.5;
  require(!Noggit::Ai::parseProceduralLiquidLayout(non_integer_type).layout,
          "non-integer liquid type was accepted");

  auto bad_depth = validArguments();
  bad_depth["features"][1]["depth"] = 0.0;
  require(!Noggit::Ai::parseProceduralLiquidLayout(bad_depth).layout,
          "zero liquid depth was accepted");

  auto non_finite_depth = validArguments();
  non_finite_depth["features"][1]["depth"]
    = std::numeric_limits<double>::infinity();
  require(!Noggit::Ai::parseProceduralLiquidLayout(non_finite_depth).layout,
          "non-finite liquid depth was accepted");

  auto bad_priority = validArguments();
  bad_priority["features"][1]["priority"] = 101;
  require(!Noggit::Ai::parseProceduralLiquidLayout(bad_priority).layout,
          "out-of-range liquid priority was accepted");

  auto too_many_points = validArguments();
  auto& points = too_many_points["features"][1]["points"];
  while (points.size() < 17)
  {
    auto const index = points.size();
    points.push_back({
      {"u", static_cast<double>(index) / 16.0},
      {"v", 0.2},
      {"height", 10.0}
    });
  }
  require(!Noggit::Ai::parseProceduralLiquidLayout(too_many_points).layout,
          "liquid feature with more than 16 points was accepted");

  auto too_many_segments = validArguments();
  too_many_segments["features"] = nlohmann::json::array();
  for (int feature_index = 0; feature_index < 9; ++feature_index)
  {
    auto feature = corridor("long_" + std::to_string(feature_index));
    feature["points"] = nlohmann::json::array();
    for (int point = 0; point < 16; ++point)
    {
      feature["points"].push_back({
        {"u", static_cast<double>(point) / 15.0},
        {"v", static_cast<double>(feature_index) / 8.0},
        {"height", 10.0}
      });
    }
    too_many_segments["features"].push_back(std::move(feature));
  }
  require(!Noggit::Ai::parseProceduralLiquidLayout(too_many_segments).layout,
          "liquid layout with more than 128 segments was accepted");

  Noggit::Ai::ProceduralLiquidLayout corridor_layout;
  corridor_layout.features.push_back({
    "river", {{0.1f, 0.2f, 10.0f}, {0.9f, 0.2f, 20.0f}},
    0.05f, 0.05f, 5, 0.8f, 10,
    Noggit::Ai::ProceduralLayoutShape::Corridor
  });
  auto const core = Noggit::Ai::sampleProceduralLiquidLayout(
    corridor_layout, 0.5f, 0.2f, 100.0f, 100.0f);
  require(core.has_liquid && core.liquid_type_id == 5,
          "liquid corridor core is absent");
  requireClose(core.height, 15.0f,
               "liquid corridor level was not interpolated");
  requireClose(core.mask, 1.0f, "liquid corridor core mask changed");
  requireClose(core.depth, 0.8f, "liquid corridor core depth changed");

  auto const transition = Noggit::Ai::sampleProceduralLiquidLayout(
    corridor_layout, 0.5f, 0.275f, 100.0f, 100.0f);
  require(transition.has_liquid, "liquid transition is absent");
  requireClose(transition.mask, 0.5f, "liquid transition is not smooth");
  requireClose(transition.depth, 0.4f,
               "liquid depth was not multiplied by its mask");

  auto const outside = Noggit::Ai::sampleProceduralLiquidLayout(
    corridor_layout, 0.5f, 0.4f, 100.0f, 100.0f);
  require(!outside.has_liquid
            && outside.feature_index == Noggit::Ai::procedural_liquid_no_feature,
          "liquid appeared outside every feature");
  auto const projected_outside = Noggit::Ai::sampleProceduralLiquidFeature(
    corridor_layout, 0, 0.5f, 0.4f, 100.0f, 100.0f);
  require(!projected_outside.has_liquid
            && projected_outside.feature_index == 0,
          "feature-only liquid sample lost its identity outside the shore");
  requireClose(projected_outside.height, 15.0f,
               "feature-only liquid sample lost its projected height");
  requireClose(projected_outside.depth, 0.0f,
               "feature-only liquid sample retained depth outside the shore");

  auto priority_layout = corridor_layout;
  priority_layout.features.push_back({
    "crossing", {{0.5f, 0.1f, 40.0f}, {0.5f, 0.3f, 40.0f}},
    0.02f, 0.08f, 9, 0.6f, 20,
    Noggit::Ai::ProceduralLayoutShape::Corridor
  });
  auto const crossing = Noggit::Ai::sampleProceduralLiquidLayout(
    priority_layout, 0.55f, 0.2f, 100.0f, 100.0f);
  require(crossing.has_liquid && crossing.liquid_type_id == 9
            && crossing.feature_index == 1,
          "higher-priority liquid did not win an overlap");
  requireClose(crossing.mask, 0.68359375f,
               "winning liquid transition mask changed");
  requireClose(crossing.depth, 0.41015625f,
               "winning liquid depth did not follow its mask");

  Noggit::Ai::ProceduralLiquidLayout area_layout;
  area_layout.features.push_back({
    "lake", {{0.2f, 0.2f, 30.0f}, {0.8f, 0.2f, 30.0f},
             {0.8f, 0.8f, 30.0f}, {0.2f, 0.8f, 30.0f}},
    0.005f, 0.02f, 7, 0.7f, 5,
    Noggit::Ai::ProceduralLayoutShape::Area
  });
  auto const area_core = Noggit::Ai::sampleProceduralLiquidLayout(
    area_layout, 0.5f, 0.5f, 200.0f, 100.0f);
  require(area_core.has_liquid && area_core.liquid_type_id == 7,
          "liquid area interior is absent");
  requireClose(area_core.height, 30.0f, "liquid area level changed");

  auto noisy = corridor_layout;
  noisy.edge_noise_ratio = 0.03f;
  auto noise_changed_boundary = false;
  for (int index = 0; index <= 40; ++index)
  {
    auto const u = static_cast<float>(index) / 40.0f;
    auto const plain = Noggit::Ai::sampleProceduralLiquidLayout(
      corridor_layout, u, 0.275f, 100.0f, 100.0f);
    auto const natural = Noggit::Ai::sampleProceduralLiquidLayout(
      noisy, u, 0.275f, 100.0f, 100.0f);
    auto const repeated = Noggit::Ai::sampleProceduralLiquidLayout(
      noisy, u, 0.275f, 100.0f, 100.0f);
    require(natural.has_liquid == repeated.has_liquid
              && natural.height == repeated.height
              && natural.depth == repeated.depth
              && natural.mask == repeated.mask,
            "liquid edge noise is not deterministic");
    noise_changed_boundary = noise_changed_boundary
      || plain.mask != natural.mask;
  }
  require(noise_changed_boundary,
          "liquid edge noise did not alter any boundary sample");

  auto const seam_u_left = (0.0f * 100.0f + 100.0f) / 200.0f;
  auto const seam_u_right = (1.0f * 100.0f + 0.0f) / 200.0f;
  auto const seam_left = Noggit::Ai::sampleProceduralLiquidLayout(
    noisy, seam_u_left, 0.2f, 200.0f, 100.0f);
  auto const seam_right = Noggit::Ai::sampleProceduralLiquidLayout(
    noisy, seam_u_right, 0.2f, 200.0f, 100.0f);
  require(seam_u_left == seam_u_right
            && seam_left.has_liquid == seam_right.has_liquid
            && seam_left.feature_index == seam_right.feature_index
            && seam_left.liquid_type_id == seam_right.liquid_type_id
            && seam_left.height == seam_right.height
            && seam_left.depth == seam_right.depth
            && seam_left.mask == seam_right.mask,
          "same liquid world position produced a tile seam");
}
