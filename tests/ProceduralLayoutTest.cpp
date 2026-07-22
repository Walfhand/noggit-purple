// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/ProceduralLayout.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

  nlohmann::json validArguments()
  {
    return {
      {"texture_paths", {
        "tileset/does/not/exist_base.blp",
        "tileset/does/not/exist_path.blp",
        "tileset/does/not/exist_steep.blp"
      }},
      {"steep_texture_layer", 2},
      {"slope_start_degrees", 30.0},
      {"slope_full_degrees", 50.0},
      {"edge_noise_ratio", 0.0},
      {"max_slope_degrees", nullptr},
      {"smoothing_strength", 0.0},
      {"features", {
        {
          {"name", "middle_lane"},
          {"shape", "corridor"},
          {"height_mode", "absolute"},
          {"points", {
            {{"u", 0.1}, {"v", 0.5}, {"height", 10.0}},
            {{"u", 0.9}, {"v", 0.5}, {"height", 30.0}}
          }},
          {"half_width_ratio", 0.03},
          {"transition_width_ratio", 0.02},
          {"texture_layer", 1},
          {"roughness_amplitude", 0.0},
          {"texture_strength", 1.0},
          {"width_variation_ratio", 0.0},
          {"priority", 10}
        },
        {
          {"name", "blue_base"},
          {"shape", "corridor"},
          {"height_mode", "absolute"},
          {"points", {
            {{"u", 0.1}, {"v", 0.9}, {"height", 10.0}}
          }},
          {"half_width_ratio", 0.08},
          {"transition_width_ratio", 0.02},
          {"texture_layer", 1},
          {"roughness_amplitude", 0.0},
          {"texture_strength", 1.0},
          {"width_variation_ratio", 0.0},
          {"priority", 20}
        }
      }}
    };
  }

  Noggit::Ai::ProceduralLayout baseLayout()
  {
    Noggit::Ai::ProceduralLayout layout;
    layout.texture_paths = {"base.blp", "path.blp", "steep.blp"};
    return layout;
  }

  Noggit::Ai::ProceduralLayoutFeature platform(
    std::string name,
    float u,
    float v,
    float height,
    float half_width,
    float transition,
    std::size_t texture_layer,
    int priority)
  {
    return {
      std::move(name),
      {{u, v, height}},
      half_width,
      transition,
      texture_layer,
      priority
    };
  }

  int quantizedTotal(Noggit::Ai::ProceduralLayoutSample const& sample)
  {
    return std::accumulate(
      sample.quantized_weights.begin(), sample.quantized_weights.end(), 0);
  }
}

int main()
{
  auto const parsed = Noggit::Ai::parseProceduralLayout(validArguments());
  require(parsed.layout.has_value(), "valid procedural layout was rejected");
  require(parsed.error.empty(), "valid procedural layout returned an error");
  require(parsed.layout->texture_paths.size() == 3,
          "texture paths were not preserved");
  require(parsed.layout->features.size() == 2,
          "features were not preserved");
  require(parsed.layout->steep && parsed.layout->steep->texture_layer == 2,
          "steep override was not parsed");

  auto no_steep = validArguments();
  no_steep["texture_paths"].erase(no_steep["texture_paths"].begin() + 2);
  no_steep["steep_texture_layer"] = nullptr;
  no_steep["slope_start_degrees"] = nullptr;
  no_steep["slope_full_degrees"] = nullptr;
  require(Noggit::Ai::parseProceduralLayout(no_steep).layout.has_value(),
          "nullable steep override was rejected");

  auto partial_steep = no_steep;
  partial_steep["steep_texture_layer"] = 2;
  require(!Noggit::Ai::parseProceduralLayout(partial_steep).layout,
          "partial steep override was accepted");

  auto bad_noise = validArguments();
  bad_noise["edge_noise_ratio"] = 0.051;
  require(!Noggit::Ai::parseProceduralLayout(bad_noise).layout,
          "out-of-range edge noise was accepted");

  auto bad_max_slope = validArguments();
  bad_max_slope["max_slope_degrees"] = 4.9;
  require(!Noggit::Ai::parseProceduralLayout(bad_max_slope).layout,
          "out-of-range maximum slope was accepted");

  auto bad_smoothing = validArguments();
  bad_smoothing["smoothing_strength"] = 1.1;
  require(!Noggit::Ai::parseProceduralLayout(bad_smoothing).layout,
          "out-of-range smoothing strength was accepted");

  auto extra_field = validArguments();
  extra_field["unexpected"] = true;
  require(!Noggit::Ai::parseProceduralLayout(extra_field).layout,
          "unknown root field was accepted");

  auto bad_coordinate = validArguments();
  bad_coordinate["features"][0]["points"][0]["u"] = 1.1;
  require(!Noggit::Ai::parseProceduralLayout(bad_coordinate).layout,
          "out-of-range normalized coordinate was accepted");

  auto non_finite = validArguments();
  non_finite["features"][0]["points"][0]["height"]
    = std::numeric_limits<double>::infinity();
  require(!Noggit::Ai::parseProceduralLayout(non_finite).layout,
          "non-finite height was accepted");

  auto bad_height = validArguments();
  bad_height["features"][0]["points"][0]["height"] = 5001.0;
  require(!Noggit::Ai::parseProceduralLayout(bad_height).layout,
          "out-of-range height was accepted");

  auto zero_width_area = validArguments();
  zero_width_area["features"][0]["shape"] = "area";
  zero_width_area["features"][0]["points"] = {
    {{"u", 0.1}, {"v", 0.1}, {"height", 10.0}},
    {{"u", 0.9}, {"v", 0.1}, {"height", 10.0}},
    {{"u", 0.5}, {"v", 0.9}, {"height", 10.0}}
  };
  zero_width_area["features"][0]["half_width_ratio"] = 0.0;
  auto const parsed_zero_width_area
    = Noggit::Ai::parseProceduralLayout(zero_width_area);
  require(parsed_zero_width_area.layout.has_value(),
          "zero-width area was rejected");
  requireClose(parsed_zero_width_area.layout->features.front().half_width_ratio,
               0.0f, "zero-width area was not preserved");

  auto zero_width_corridor = validArguments();
  zero_width_corridor["features"][0]["half_width_ratio"] = 0.0;
  require(!Noggit::Ai::parseProceduralLayout(zero_width_corridor).layout,
          "zero-width corridor was accepted");

  auto too_narrow_corridor = validArguments();
  too_narrow_corridor["features"][0]["half_width_ratio"] = 0.004;
  require(!Noggit::Ai::parseProceduralLayout(too_narrow_corridor).layout,
          "corridor narrower than 0.005 was accepted");

  auto minimum_width_corridor = validArguments();
  minimum_width_corridor["features"][0]["half_width_ratio"] = 0.005;
  require(Noggit::Ai::parseProceduralLayout(minimum_width_corridor).layout.has_value(),
          "corridor at the 0.005 minimum was rejected");

  auto bad_priority = validArguments();
  bad_priority["features"][0]["priority"] = 101;
  require(!Noggit::Ai::parseProceduralLayout(bad_priority).layout,
          "out-of-range priority was accepted");

  auto bad_roughness = validArguments();
  bad_roughness["features"][0]["roughness_amplitude"] = 100.1;
  require(!Noggit::Ai::parseProceduralLayout(bad_roughness).layout,
          "out-of-range roughness was accepted");

  auto bad_texture_strength = validArguments();
  bad_texture_strength["features"][0]["texture_strength"] = 0.0;
  require(!Noggit::Ai::parseProceduralLayout(bad_texture_strength).layout,
          "out-of-range texture strength was accepted");

  auto bad_width_variation = validArguments();
  bad_width_variation["features"][0]["width_variation_ratio"] = 0.76;
  require(!Noggit::Ai::parseProceduralLayout(bad_width_variation).layout,
          "out-of-range width variation was accepted");

  auto bad_shape = validArguments();
  bad_shape["features"][0]["shape"] = "spline";
  require(!Noggit::Ai::parseProceduralLayout(bad_shape).layout,
          "unknown feature shape was accepted");

  auto bad_height_mode = validArguments();
  bad_height_mode["features"][0]["height_mode"] = "relative";
  require(!Noggit::Ai::parseProceduralLayout(bad_height_mode).layout,
          "unknown feature height mode was accepted");

  auto bad_layer = validArguments();
  bad_layer["features"][0]["texture_layer"] = 3;
  require(!Noggit::Ai::parseProceduralLayout(bad_layer).layout,
          "feature texture layer outside the palette was accepted");

  auto duplicate_point = validArguments();
  duplicate_point["features"][0]["points"][1]["u"] = 0.1;
  duplicate_point["features"][0]["points"][1]["v"] = 0.5;
  require(!Noggit::Ai::parseProceduralLayout(duplicate_point).layout,
          "duplicate consecutive feature points were accepted");

  auto short_area = validArguments();
  short_area["features"][0]["shape"] = "area";
  require(!Noggit::Ai::parseProceduralLayout(short_area).layout,
          "area with fewer than three points was accepted");

  auto crossed_area = validArguments();
  crossed_area["features"][0]["shape"] = "area";
  crossed_area["features"][0]["points"] = {
    {{"u", 0.1}, {"v", 0.1}, {"height", 10.0}},
    {{"u", 0.9}, {"v", 0.9}, {"height", 10.0}},
    {{"u", 0.1}, {"v", 0.9}, {"height", 10.0}},
    {{"u", 0.9}, {"v", 0.1}, {"height", 10.0}}
  };
  require(!Noggit::Ai::parseProceduralLayout(crossed_area).layout,
          "self-intersecting area was accepted");

  auto varying_area = crossed_area;
  varying_area["features"][0]["points"] = {
    {{"u", 0.1}, {"v", 0.1}, {"height", 10.0}},
    {{"u", 0.9}, {"v", 0.1}, {"height", 11.0}},
    {{"u", 0.5}, {"v", 0.9}, {"height", 10.0}}
  };
  require(!Noggit::Ai::parseProceduralLayout(varying_area).layout,
          "area with ambiguous vertex heights was accepted");

  auto too_many_points = validArguments();
  auto& oversized_points = too_many_points["features"][0]["points"];
  while (oversized_points.size() < 17)
  {
    auto const index = oversized_points.size();
    oversized_points.push_back({
      {"u", static_cast<double>(index) / 16.0},
      {"v", 0.4},
      {"height", 10.0}
    });
  }
  require(!Noggit::Ai::parseProceduralLayout(too_many_points).layout,
          "feature with more than 16 points was accepted");

  auto too_many_segments = validArguments();
  too_many_segments["features"] = nlohmann::json::array();
  for (int feature_index = 0; feature_index < 35; ++feature_index)
  {
    auto feature = validArguments()["features"][0];
    feature["name"] = "oversized_" + std::to_string(feature_index);
    feature["points"] = nlohmann::json::array();
    for (int point = 0; point < 16; ++point)
    {
      feature["points"].push_back({
        {"u", static_cast<double>(point) / 15.0},
        {"v", static_cast<double>(feature_index) / 35.0},
        {"height", 10.0}
      });
    }
    too_many_segments["features"].push_back(std::move(feature));
  }
  require(!Noggit::Ai::parseProceduralLayout(too_many_segments).layout,
          "layout with more than 512 segments was accepted");

  auto bad_name = validArguments();
  bad_name["features"][0]["name"] = "bad\nname";
  require(!Noggit::Ai::parseProceduralLayout(bad_name).layout,
          "non-printable feature name was accepted");

  auto unused_palette = validArguments();
  unused_palette["steep_texture_layer"] = nullptr;
  unused_palette["slope_start_degrees"] = nullptr;
  unused_palette["slope_full_degrees"] = nullptr;
  require(!Noggit::Ai::parseProceduralLayout(unused_palette).layout,
          "unused palette layer was accepted");

  auto sixteen_textures = validArguments();
  sixteen_textures["texture_paths"] = nlohmann::json::array();
  sixteen_textures["features"] = nlohmann::json::array();
  sixteen_textures["steep_texture_layer"] = nullptr;
  sixteen_textures["slope_start_degrees"] = nullptr;
  sixteen_textures["slope_full_degrees"] = nullptr;
  for (int layer = 0; layer < 16; ++layer)
  {
    sixteen_textures["texture_paths"].push_back(
      "tileset/test/texture_" + std::to_string(layer) + ".blp");
    if (layer == 0)
    {
      continue;
    }
    auto feature = validArguments()["features"][0];
    feature["name"] = "palette_" + std::to_string(layer);
    feature["texture_layer"] = layer;
    sixteen_textures["features"].push_back(std::move(feature));
  }
  require(Noggit::Ai::parseProceduralLayout(sixteen_textures).layout.has_value(),
          "valid 16-texture map palette was rejected");
  auto seventeen_textures = sixteen_textures;
  seventeen_textures["texture_paths"].push_back(
    "tileset/test/texture_16.blp");
  require(!Noggit::Ai::parseProceduralLayout(seventeen_textures).layout,
          "map palette with more than 16 textures was accepted");

  auto reversed_priority = validArguments();
  std::swap(reversed_priority["features"][0], reversed_priority["features"][1]);
  auto const sorted = Noggit::Ai::parseProceduralLayout(reversed_priority);
  require(sorted.layout && sorted.layout->features.front().name == "middle_lane",
          "features were not stably sorted by ascending priority");

  auto layout = baseLayout();
  layout.features.push_back(platform("platform", 0.5f, 0.5f, 50.0f,
                                     0.1f, 0.1f, 1, 0));

  auto const core = Noggit::Ai::sampleProceduralLayout(
    layout, 0.5f, 0.5f, 0.0f, 0.0f, 100.0f, 100.0f);
  requireClose(core.height, 50.0f, "platform core did not reach target height");
  requireClose(core.feature_masks[0], 1.0f, "platform core mask is not opaque");
  requireClose(core.semantic_weights[1], 1.0f,
               "platform semantic texture is not opaque");
  require(core.quantized_weights[1] == 255 && quantizedTotal(core) == 255,
          "platform semantic weights were not quantized exactly");

  auto const transition = Noggit::Ai::sampleProceduralLayout(
    layout, 0.65f, 0.5f, 0.0f, 0.0f, 100.0f, 100.0f);
  requireClose(transition.height, 25.0f,
               "platform transition did not blend target height");
  requireClose(transition.feature_masks[0], 0.5f,
               "platform transition mask is not smooth");
  require(transition.semantic_weights[0] > 0.0f
            && transition.semantic_weights[1] > 0.0f
            && quantizedTotal(transition) == 255,
          "transition semantic weights are not a normalized visible mix");

  auto const outside = Noggit::Ai::sampleProceduralLayout(
    layout, 0.9f, 0.9f, 7.0f, 0.0f, 100.0f, 100.0f);
  requireClose(outside.height, 7.0f, "outside terrain height was modified");
  require(outside.quantized_weights[0] == 255,
          "outside terrain did not keep base semantics");

  auto area_layout = baseLayout();
  auto area = Noggit::Ai::ProceduralLayoutFeature{
    "jungle_area",
    {{0.2f, 0.2f, 6.0f}, {0.8f, 0.2f, 6.0f},
     {0.7f, 0.8f, 6.0f}, {0.3f, 0.8f, 6.0f}},
    0.005f,
    0.05f,
    1,
    0
  };
  area.shape = Noggit::Ai::ProceduralLayoutShape::Area;
  area.height_mode = Noggit::Ai::ProceduralLayoutHeightMode::Offset;
  area_layout.features.push_back(area);
  auto const area_inside = Noggit::Ai::sampleProceduralLayout(
    area_layout, 0.5f, 0.5f, 20.0f, 0.0f, 100.0f, 100.0f);
  auto const area_outside = Noggit::Ai::sampleProceduralLayout(
    area_layout, 0.05f, 0.5f, 20.0f, 0.0f, 100.0f, 100.0f);
  requireClose(area_inside.height, 26.0f,
               "relative area did not preserve and offset source relief");
  requireClose(area_inside.feature_masks[0], 1.0f,
               "polygon interior is not a full core");
  requireClose(area_outside.height, 20.0f,
               "polygon modified terrain outside its transition");

  auto blended_area = area_layout;
  blended_area.features.front().texture_strength = 0.5f;
  auto const blended_sample = Noggit::Ai::sampleProceduralLayout(
    blended_area, 0.5f, 0.5f, 20.0f, 0.0f, 100.0f, 100.0f);
  require(blended_sample.semantic_weights[0] > 0.0f
            && blended_sample.semantic_weights[1] > 0.0f,
          "reduced texture strength did not blend feature and base textures");

  auto rough_area = area_layout;
  rough_area.features.front().roughness_amplitude = 6.0f;
  auto minimum_rough_height = std::numeric_limits<float>::max();
  auto maximum_rough_height = std::numeric_limits<float>::lowest();
  for (int z = 3; z <= 7; ++z)
  {
    for (int x = 3; x <= 7; ++x)
    {
      auto const u = static_cast<float>(x) / 10.0f;
      auto const v = static_cast<float>(z) / 10.0f;
      auto const first = Noggit::Ai::sampleProceduralLayout(
        rough_area, u, v, 20.0f, 0.0f, 100.0f, 100.0f);
      auto const second = Noggit::Ai::sampleProceduralLayout(
        rough_area, u, v, 20.0f, 0.0f, 100.0f, 100.0f);
      require(first.height == second.height,
              "feature roughness is not deterministic");
      minimum_rough_height = std::min(minimum_rough_height, first.height);
      maximum_rough_height = std::max(maximum_rough_height, first.height);
    }
  }
  require(maximum_rough_height - minimum_rough_height > 1.0f,
          "feature roughness left a flat area core");
  requireClose(Noggit::Ai::sampleProceduralLayout(
                 rough_area, 0.05f, 0.5f, 20.0f,
                 0.0f, 100.0f, 100.0f).height,
               20.0f,
               "feature roughness leaked outside its area");

  auto bounded_offset = area_layout;
  for (auto& point : bounded_offset.features.front().points)
  {
    point.height = 5000.0f;
  }
  requireClose(Noggit::Ai::sampleProceduralLayout(
                 bounded_offset, 0.5f, 0.5f, 100.0f,
                 0.0f, 100.0f, 100.0f).height,
               5000.0f,
               "relative features escaped the supported height range");

  auto slope_limited = layout;
  slope_limited.max_slope_degrees = 30.0f;
  auto const formerly_outside = Noggit::Ai::sampleProceduralLayout(
    slope_limited, 0.8f, 0.5f, 0.0f, 0.0f, 100.0f, 100.0f);
  require(formerly_outside.height > 0.0f,
          "maximum slope did not widen an abrupt transition");
  requireClose(Noggit::Ai::sampleProceduralLayout(
                 slope_limited, 0.5f, 0.5f, 0.0f, 0.0f, 100.0f, 100.0f).height,
               50.0f,
               "maximum slope prevented the core from reaching its target");

  auto noisy = area_layout;
  noisy.edge_noise_ratio = 0.03f;
  auto noise_changed_boundary = false;
  for (int sample = 0; sample <= 40; ++sample)
  {
    auto const v = static_cast<float>(sample) / 40.0f;
    auto const plain = Noggit::Ai::sampleProceduralLayout(
      area_layout, 0.15f, v, 20.0f, 0.0f, 100.0f, 100.0f);
    auto const natural = Noggit::Ai::sampleProceduralLayout(
      noisy, 0.15f, v, 20.0f, 0.0f, 100.0f, 100.0f);
    noise_changed_boundary = noise_changed_boundary
      || plain.height != natural.height;
    auto const repeated = Noggit::Ai::sampleProceduralLayout(
      noisy, 0.15f, v, 20.0f, 0.0f, 100.0f, 100.0f);
    require(natural.height == repeated.height,
            "edge naturalization is not deterministic");
  }
  require(noise_changed_boundary,
          "edge naturalization did not alter any boundary sample");

  auto tiny_noisy_area = baseLayout();
  auto tiny_area = area;
  tiny_area.points = {
    {0.495f, 0.495f, 2.0f}, {0.505f, 0.495f, 2.0f},
    {0.505f, 0.505f, 2.0f}, {0.495f, 0.505f, 2.0f}
  };
  tiny_area.transition_width_ratio = 0.001f;
  tiny_noisy_area.features.push_back(tiny_area);
  tiny_noisy_area.edge_noise_ratio = 0.05f;
  requireClose(Noggit::Ai::sampleProceduralLayout(
                 tiny_noisy_area, 0.5f, 0.5f, 10.0f,
                 0.0f, 100.0f, 100.0f).feature_masks[0],
               1.0f,
               "edge noise removed the core of a small valid area");

  auto smoothed = layout;
  smoothed.smoothing_strength = 1.0f;
  auto const raw_boundary = Noggit::Ai::sampleProceduralLayout(
    smoothed, 0.625f, 0.5f, 0.0f, 0.0f, 100.0f, 100.0f).height;
  auto const smooth_boundary = Noggit::Ai::sampleSmoothedProceduralLayoutHeight(
    smoothed, 0.625f, 0.5f, 0.0f, 100.0f, 100.0f, 5.0f);
  require(smooth_boundary != raw_boundary,
          "height smoothing did not filter a transition");
  requireClose(Noggit::Ai::sampleSmoothedProceduralLayoutHeight(
                 smoothed, 0.705f, 0.5f, 0.0f, 100.0f, 100.0f, 5.0f),
               0.0f,
               "height smoothing leaked beyond the semantic mask");
  requireClose(Noggit::Ai::sampleSmoothedProceduralLayoutHeight(
                 smoothed, 0.5f, 0.5f, 0.0f, 100.0f, 100.0f, 1.0f),
               50.0f,
               "height smoothing changed a wide feature core");

  auto corridor_layout = baseLayout();
  corridor_layout.features.push_back({
    "sloped_corridor",
    {{0.1f, 0.2f, 10.0f}, {0.9f, 0.2f, 90.0f}},
    0.02f,
    0.02f,
    1,
    0
  });
  auto const corridor_middle = Noggit::Ai::sampleProceduralLayout(
    corridor_layout, 0.5f, 0.2f, 0.0f, 0.0f, 100.0f, 100.0f);
  requireClose(corridor_middle.height, 50.0f,
               "corridor height was not interpolated along its segment");

  auto priority_layout = baseLayout();
  priority_layout.features.push_back(
    platform("low_priority", 0.5f, 0.5f, 10.0f, 0.2f, 0.0f, 1, 1));
  priority_layout.features.push_back(
    platform("high_priority", 0.5f, 0.5f, 80.0f, 0.05f, 0.1f, 2, 2));
  auto const priority_sample = Noggit::Ai::sampleProceduralLayout(
    priority_layout, 0.6f, 0.5f, 0.0f, 0.0f, 100.0f, 100.0f);
  requireClose(priority_sample.feature_masks[0], 0.5f,
               "lower priority feature kept influence hidden by a later feature");
  requireClose(priority_sample.feature_masks[1], 0.5f,
               "high priority transition mask changed");
  requireClose(priority_sample.height, 45.0f,
               "higher priority feature was not composed last");
  requireClose(priority_sample.semantic_weights[1], 0.5f,
               "lower priority core was erased at a higher-priority transition");
  requireClose(priority_sample.semantic_weights[2], 0.5f,
               "higher priority semantic transition was not composed last");

  auto steep_layout = layout;
  steep_layout.steep = Noggit::Ai::ProceduralLayoutSteep{2, 30.0f, 50.0f};
  auto const fully_steep = Noggit::Ai::sampleProceduralLayout(
    steep_layout, 0.5f, 0.5f, 0.0f, 50.0f, 100.0f, 100.0f);
  requireClose(fully_steep.height, 50.0f,
               "slope override unexpectedly changed semantic height");
  require(fully_steep.quantized_weights[2] == 255,
          "full slope did not override the semantic texture");

  auto const mixed_steep = Noggit::Ai::sampleProceduralLayout(
    steep_layout, 0.5f, 0.5f, 0.0f, 40.0f, 100.0f, 100.0f);
  requireClose(mixed_steep.semantic_weights[1], 0.5f,
               "mid-slope semantic weight changed");
  requireClose(mixed_steep.semantic_weights[2], 0.5f,
               "mid-slope override weight changed");
  require(quantizedTotal(mixed_steep) == 255,
          "slope override quantization does not sum to 255");

  auto aspect_layout = baseLayout();
  aspect_layout.features.push_back(
    platform("round_platform", 0.5f, 0.5f, 25.0f, 0.1f, 0.0f, 1, 0));
  auto const wide_x = Noggit::Ai::sampleProceduralLayout(
    aspect_layout, 0.56f, 0.5f, 0.0f, 0.0f, 200.0f, 100.0f);
  auto const wide_z = Noggit::Ai::sampleProceduralLayout(
    aspect_layout, 0.5f, 0.56f, 0.0f, 0.0f, 200.0f, 100.0f);
  auto const square_x = Noggit::Ai::sampleProceduralLayout(
    aspect_layout, 0.56f, 0.5f, 0.0f, 0.0f, 100.0f, 100.0f);
  requireClose(wide_x.height, 0.0f,
               "wide-map X distance ignored map aspect ratio");
  requireClose(wide_z.height, 25.0f,
               "wide-map Z distance was incorrectly stretched");
  requireClose(square_x.height, 25.0f,
               "square-map distance was incorrectly stretched");

  auto direct_oversized = baseLayout();
  for (std::size_t index = 0;
       index < Noggit::Ai::procedural_layout_max_features + 1; ++index)
  {
    direct_oversized.features.push_back(platform(
      "direct_" + std::to_string(index), 0.5f, 0.5f, 1.0f,
      0.01f, 0.01f, 1, 0));
  }
  require(std::isfinite(Noggit::Ai::sampleProceduralLayout(
            direct_oversized, 0.5f, 0.5f, 0.0f, 0.0f, 100.0f, 100.0f).height),
          "direct layout construction bypassed the feature safety bound");

  auto synthetic = baseLayout();
  synthetic.texture_paths.push_back("river.blp");
  synthetic.steep = Noggit::Ai::ProceduralLayoutSteep{2, 30.0f, 50.0f};
  synthetic.features = {
    {
      "top_lane", {{0.1f, 0.2f, 12.0f}, {0.9f, 0.2f, 14.0f}},
      0.025f, 0.015f, 1, 10
    },
    {
      "middle_lane", {{0.1f, 0.5f, 10.0f}, {0.9f, 0.5f, 16.0f}},
      0.025f, 0.015f, 1, 10
    },
    {
      "bottom_lane", {{0.1f, 0.8f, 8.0f}, {0.9f, 0.8f, 12.0f}},
      0.025f, 0.015f, 1, 10
    },
    platform("blue_platform", 0.1f, 0.5f, 10.0f, 0.07f, 0.02f, 1, 20),
    platform("red_platform", 0.9f, 0.5f, 16.0f, 0.07f, 0.02f, 1, 20),
    {
      "river", {{0.5f, 0.05f, 4.0f}, {0.5f, 0.95f, 4.0f}},
      0.02f, 0.015f, 3, 30
    }
  };

  auto const left_seam_u = (0.0f * 100.0f + 100.0f) / 200.0f;
  auto const right_seam_u = (1.0f * 100.0f + 0.0f) / 200.0f;
  auto const left_seam = Noggit::Ai::sampleProceduralLayout(
    synthetic, left_seam_u, 0.5f, 20.0f, 0.0f, 200.0f, 100.0f);
  auto const right_seam = Noggit::Ai::sampleProceduralLayout(
    synthetic, right_seam_u, 0.5f, 20.0f, 0.0f, 200.0f, 100.0f);
  require(left_seam_u == right_seam_u
            && left_seam.height == right_seam.height
            && left_seam.semantic_weights == right_seam.semantic_weights
            && left_seam.quantized_weights == right_seam.quantized_weights
            && left_seam.feature_masks == right_seam.feature_masks,
          "the same world position sampled from adjacent tiles produced a seam");

  std::vector<std::size_t> core_samples(synthetic.features.size());
  std::array<std::size_t, 4> strong_texture_samples{};
  std::uint64_t checksum = 1469598103934665603ULL;
  for (int y = 0; y < 64; ++y)
  {
    for (int x = 0; x < 128; ++x)
    {
      auto const u = static_cast<float>(x) / 127.0f;
      auto const v = static_cast<float>(y) / 63.0f;
      auto const original_height = 20.0f + u * 3.0f - v * 2.0f;
      auto const slope = u < 0.12f ? 60.0f : 0.0f;
      auto const first = Noggit::Ai::sampleProceduralLayout(
        synthetic, u, v, original_height, slope, 200.0f, 100.0f);
      auto const second = Noggit::Ai::sampleProceduralLayout(
        synthetic, u, v, original_height, slope, 200.0f, 100.0f);
      require(first.height == second.height
                && first.semantic_weights == second.semantic_weights
                && first.quantized_weights == second.quantized_weights
                && first.feature_masks == second.feature_masks,
              "procedural layout sampling is not deterministic");

      for (std::size_t feature = 0; feature < synthetic.features.size(); ++feature)
      {
        if (first.feature_masks[feature] >= 0.999f)
        {
          ++core_samples[feature];
        }
      }
      for (std::size_t layer = 0; layer < strong_texture_samples.size(); ++layer)
      {
        if (first.quantized_weights[layer] >= 64)
        {
          ++strong_texture_samples[layer];
        }
        checksum ^= first.quantized_weights[layer];
        checksum *= 1099511628211ULL;
      }
      checksum ^= static_cast<std::uint64_t>(std::lround(first.height * 1000.0f));
      checksum *= 1099511628211ULL;
    }
  }

  require(checksum != 0, "synthetic layout checksum is empty");
  for (auto const count : core_samples)
  {
    require(count > 0, "a synthetic corridor or platform has no core sample");
  }
  for (auto const count : strong_texture_samples)
  {
    require(count >= 64,
            "a synthetic semantic texture is not strong on at least 64 samples");
  }
}
