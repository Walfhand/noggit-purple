// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_AI_PROCEDURAL_LIQUID_LAYOUT_HPP
#define NOGGIT_AI_PROCEDURAL_LIQUID_LAYOUT_HPP

#include <noggit/ai/ProceduralLayout.hpp>

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Noggit::Ai
{
  inline constexpr std::size_t procedural_liquid_no_feature
    = procedural_layout_max_features;
  inline constexpr std::size_t procedural_liquid_max_distinct_types = 14;

  struct ProceduralLiquidLayoutFeature
  {
    std::string name;
    std::vector<ProceduralLayoutPoint> points;
    float half_width_ratio = 0.0f;
    float transition_width_ratio = 0.0f;
    std::uint16_t liquid_type_id = 0;
    float depth = 0.0f;
    int priority = 0;
    ProceduralLayoutShape shape = ProceduralLayoutShape::Corridor;
  };

  struct ProceduralLiquidLayout
  {
    bool replace_existing = false;
    float edge_noise_ratio = 0.0f;
    std::vector<ProceduralLiquidLayoutFeature> features;
  };

  struct ProceduralLiquidLayoutParseResult
  {
    std::optional<ProceduralLiquidLayout> layout;
    std::string error;
  };

  struct ProceduralLiquidLayoutSample
  {
    bool has_liquid = false;
    std::size_t feature_index = procedural_liquid_no_feature;
    std::uint16_t liquid_type_id = 0;
    float height = 0.0f;
    float depth = 0.0f;
    float mask = 0.0f;
  };

  ProceduralLiquidLayoutParseResult parseProceduralLiquidLayout(
    nlohmann::json const& arguments);

  ProceduralLiquidLayoutSample sampleProceduralLiquidLayout(
    ProceduralLiquidLayout const& layout,
    float u,
    float v,
    float map_width,
    float map_height);

  // Samples one feature without applying crossing priorities. The projected
  // height remains available outside its mask so callers can build continuous
  // MH2O corner grids at shore and chunk boundaries.
  ProceduralLiquidLayoutSample sampleProceduralLiquidFeature(
    ProceduralLiquidLayout const& layout,
    std::size_t feature_index,
    float u,
    float v,
    float map_width,
    float map_height);
}

#endif
