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
  inline constexpr std::size_t procedural_layout_max_features = 32;

  struct ProceduralLayoutPoint
  {
    float u = 0.0f;
    float v = 0.0f;
    float height = 0.0f;
  };

  struct ProceduralLayoutFeature
  {
    std::string name;
    std::vector<ProceduralLayoutPoint> points;
    float half_width_ratio = 0.0f;
    float transition_width_ratio = 0.0f;
    std::size_t texture_layer = 0;
    int priority = 0;
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
  };

  struct ProceduralLayoutParseResult
  {
    std::optional<ProceduralLayout> layout;
    std::string error;
  };

  struct ProceduralLayoutSample
  {
    float height = 0.0f;
    std::array<float, 4> semantic_weights{};
    std::array<std::uint8_t, 4> quantized_weights{};
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
}

#endif
