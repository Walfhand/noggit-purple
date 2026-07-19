// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_AI_PROCEDURAL_LAYOUT_HPP
#define NOGGIT_AI_PROCEDURAL_LAYOUT_HPP

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Noggit::Ai
{
  inline constexpr std::size_t procedural_layout_max_features = 96;
  inline constexpr std::size_t procedural_layout_max_texture_paths = 16;

  enum class ProceduralLayoutShape : std::uint8_t
  {
    Corridor,
    Area
  };

  enum class ProceduralLayoutHeightMode : std::uint8_t
  {
    Absolute,
    Offset
  };

  struct ProceduralLayoutPoint
  {
    float u = 0.0f;
    float v = 0.0f;
    float height = 0.0f;
  };

  struct ProceduralShapeDistance
  {
    float distance = 0.0f;
    float height = 0.0f;
  };

  bool isSimpleProceduralArea(std::vector<ProceduralLayoutPoint> const& points);

  ProceduralShapeDistance distanceToProceduralShape(
    std::vector<ProceduralLayoutPoint> const& points,
    ProceduralLayoutShape shape,
    float sample_x,
    float sample_z,
    float scale_x,
    float scale_z);

  float proceduralShapeMask(
    float half_width_ratio,
    float transition_width_ratio,
    float distance);

  float proceduralEdgeNoise(float x, float z, std::string const& name);

  struct ProceduralLayoutFeature
  {
    std::string name;
    std::vector<ProceduralLayoutPoint> points;
    float half_width_ratio = 0.0f;
    float transition_width_ratio = 0.0f;
    std::size_t texture_layer = 0;
    int priority = 0;
    ProceduralLayoutShape shape = ProceduralLayoutShape::Corridor;
    ProceduralLayoutHeightMode height_mode = ProceduralLayoutHeightMode::Absolute;
    float roughness_amplitude = 0.0f;
    float texture_strength = 1.0f;
    float width_variation_ratio = 0.0f;
    // Sampling cull bounds, tightened by parseProceduralLayout. The defaults
    // cover the whole layout so hand-built features stay correct (no cull).
    float min_u = 0.0f;
    float max_u = 1.0f;
    float min_v = 0.0f;
    float max_v = 1.0f;
    float min_height = -500.0f;
    float max_height = 5000.0f;
  };

  struct ProceduralLayoutSteep
  {
    std::size_t texture_layer = 0;
    float slope_start_degrees = 0.0f;
    float slope_full_degrees = 0.0f;
  };

  struct ProceduralLayout
  {
    std::vector<std::string> texture_paths;
    std::optional<ProceduralLayoutSteep> steep;
    std::vector<ProceduralLayoutFeature> features;
    float edge_noise_ratio = 0.0f;
    std::optional<float> max_slope_degrees;
    float smoothing_strength = 0.0f;
  };

  struct ProceduralLayoutParseResult
  {
    std::optional<ProceduralLayout> layout;
    std::string error;
  };

  struct ProceduralLayoutSample
  {
    float height = 0.0f;
    std::array<float, procedural_layout_max_texture_paths> semantic_weights{};
    std::array<std::uint8_t, procedural_layout_max_texture_paths> quantized_weights{};
    std::array<float, procedural_layout_max_features> feature_masks{};
  };

  ProceduralLayoutParseResult parseProceduralLayout(nlohmann::json const& arguments);

  ProceduralLayoutSample sampleProceduralLayout(
    ProceduralLayout const& layout,
    float u,
    float v,
    float original_height,
    float slope_degrees,
    float map_width,
    float map_height);

  float sampleSmoothedProceduralLayoutHeight(
    ProceduralLayout const& layout,
    float u,
    float v,
    float original_height,
    float map_width,
    float map_height,
    float sample_spacing_world);
}

#endif
