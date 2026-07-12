// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_AI_PROCEDURAL_SCATTER_HPP
#define NOGGIT_AI_PROCEDURAL_SCATTER_HPP

#include <noggit/ai/ProceduralLayout.hpp>

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Noggit::Ai
{
  struct ProceduralScatterAsset
  {
    std::string path;
    std::string role;
    float weight = 1.0f;
    float min_scale = 1.0f;
    float max_scale = 1.0f;
    float spacing_multiplier = 1.0f;
  };

  struct ProceduralScatterRegion
  {
    std::string name;
    std::string role;
    std::vector<ProceduralLayoutPoint> points;
    std::size_t density_per_tile = 0;
    float min_spacing_ratio = 0.0f;
    float min_height = 0.0f;
    float max_height = 0.0f;
    float min_slope_degrees = 0.0f;
    float max_slope_degrees = 0.0f;
    float cluster_scale = 1.0f;
    float cluster_strength = 0.0f;
  };

  struct ProceduralScatterExclusion
  {
    ProceduralLayoutShape shape = ProceduralLayoutShape::Corridor;
    std::vector<ProceduralLayoutPoint> points;
    float half_width_ratio = 0.0f;
  };

  struct ProceduralScatter
  {
    std::string seed;
    std::vector<ProceduralScatterAsset> assets;
    std::vector<ProceduralScatterRegion> regions;
    std::vector<ProceduralScatterExclusion> exclusions;
  };

  struct ProceduralScatterParseResult
  {
    std::optional<ProceduralScatter> scatter;
    std::string error;
  };

  struct ProceduralScatterCandidate
  {
    float u = 0.0f;
    float v = 0.0f;
    std::size_t asset_index = 0;
    float scale = 1.0f;
    float yaw_degrees = 0.0f;
    bool active = true;
  };

  ProceduralScatterParseResult parseProceduralScatter(
    nlohmann::json const& arguments);

  ProceduralScatterCandidate proceduralScatterCandidate(
    ProceduralScatter const& scatter,
    std::size_t region_index,
    std::size_t tile_x,
    std::size_t tile_z,
    std::size_t candidate_index,
    float u_min,
    float u_max,
    float v_min,
    float v_max);

  bool proceduralScatterContains(
    std::vector<ProceduralLayoutPoint> const& points,
    float u,
    float v);

  bool proceduralScatterRegionIntersects(
    ProceduralScatterRegion const& region,
    float u_min,
    float u_max,
    float v_min,
    float v_max);

  bool proceduralScatterExcluded(
    ProceduralScatter const& scatter,
    float u,
    float v,
    float map_width,
    float map_height);
}

#endif
