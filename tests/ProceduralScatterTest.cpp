// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/ProceduralScatter.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
  void require(bool condition, char const* message)
  {
    if (!condition) throw std::runtime_error(message);
  }
}

int main()
{
  auto specification = nlohmann::json::parse(R"({
    "seed":"jungle-v1",
    "assets":[
      {"path":"world/tree.m2","role":"canopy","weight":3,"min_scale":0.8,"max_scale":1.2,"spacing_multiplier":1.2},
      {"path":"world/tree2.m2","role":"canopy","weight":2,"min_scale":0.7,"max_scale":1.1,"spacing_multiplier":1.0},
      {"path":"world/rock.wmo","role":"rock","weight":1,"min_scale":0.9,"max_scale":1.1,"spacing_multiplier":1.0}
    ],
    "regions":[{
      "name":"jungle","role":"canopy","points":[{"u":0.1,"v":0.1},{"u":0.9,"v":0.1},{"u":0.9,"v":0.9},{"u":0.1,"v":0.9}],
      "density_per_tile":64,"min_spacing_ratio":0.02,
      "min_height":-100,"max_height":100,"min_slope_degrees":0,"max_slope_degrees":35,
      "cluster_scale":3,"cluster_strength":0.75
    }],
    "exclusions":[{
      "shape":"corridor","points":[{"u":0.5,"v":0.0},{"u":0.5,"v":1.0}],"half_width_ratio":0.05
    }]
  })");
  auto parsed = Noggit::Ai::parseProceduralScatter(specification);
  require(parsed.scatter.has_value(), "valid scatter rejected");
  auto const first = Noggit::Ai::proceduralScatterCandidate(
    *parsed.scatter, 0, 26, 26, 4, 0.0f, 0.5f, 0.0f, 1.0f);
  auto const repeated = Noggit::Ai::proceduralScatterCandidate(
    *parsed.scatter, 0, 26, 26, 4, 0.0f, 0.5f, 0.0f, 1.0f);
  require(first.u == repeated.u && first.v == repeated.v
          && first.asset_index == repeated.asset_index
          && first.scale == repeated.scale && first.yaw_degrees == repeated.yaw_degrees
          && first.active == repeated.active,
          "scatter must be deterministic");
  require(first.u >= 0.0f && first.u <= 0.5f && first.v >= 0.0f && first.v <= 1.0f,
          "candidate escaped its tile");
  bool first_species = false;
  bool second_species = false;
  bool clearing = false;
  for (std::size_t index = 0; index < 512; ++index)
  {
    auto const candidate = Noggit::Ai::proceduralScatterCandidate(
      *parsed.scatter, 0, 26, 26, index, 0.0f, 1.0f, 0.0f, 1.0f);
    first_species |= candidate.asset_index == 0;
    second_species |= candidate.asset_index == 1;
    clearing |= !candidate.active;
    require(candidate.asset_index != 2, "region selected an asset from another role");
  }
  require(first_species && second_species && clearing,
          "species patches or low-frequency clearings disappeared");
  auto wall_specification = specification;
  wall_specification["regions"].push_back({
    {"name", "jungle_1_wall"}, {"role", "rock"},
    {"points", nlohmann::json::array({{{"u", .1}, {"v", .1}},
      {{"u", .9}, {"v", .1}}, {{"u", .9}, {"v", .9}}, {{"u", .1}, {"v", .9}}})},
    {"density_per_tile", 64}, {"min_spacing_ratio", .004},
    {"min_height", -100}, {"max_height", 100},
    {"min_slope_degrees", 0}, {"max_slope_degrees", 90},
    {"cluster_scale", 2}, {"cluster_strength", 0}
  });
  auto const wall_scatter = Noggit::Ai::parseProceduralScatter(wall_specification);
  require(wall_scatter.scatter.has_value(), "valid wall scatter rejected");
  auto previous_wall_candidate = Noggit::Ai::ProceduralScatterCandidate{};
  for (std::size_t index = 0; index < 64; ++index)
  {
    auto const candidate = Noggit::Ai::proceduralScatterCandidate(
      *wall_scatter.scatter, 1, 26, 26, index, 0.0f, 1.0f, 0.0f, 1.0f);
    auto const on_wall = std::abs(candidate.u - .1f) < .001f
      || std::abs(candidate.u - .9f) < .001f
      || std::abs(candidate.v - .1f) < .001f
      || std::abs(candidate.v - .9f) < .001f;
    require(candidate.active && on_wall,
            "wall candidates must form a deterministic perimeter chain");
    if (index > 0)
      require(std::hypot(candidate.u - previous_wall_candidate.u,
                         candidate.v - previous_wall_candidate.v) <= .051f,
              "wall chain contains an unbounded gap");
    previous_wall_candidate = candidate;
    auto const yaw = candidate.yaw_degrees * 3.14159265358979323846f / 180.0f;
    auto const horizontal_edge = std::abs(candidate.v - .1f) < .001f
      || std::abs(candidate.v - .9f) < .001f;
    require(horizontal_edge ? std::abs(std::cos(yaw)) > .999f
                            : std::abs(std::sin(yaw)) > .999f,
            "wall model long axes must align with their polygon edges");
  }
  std::array<std::size_t, 64> wall_candidate_visits{};
  std::array<Noggit::Ai::ProceduralScatterCandidate, 64> tiled_wall_candidates{};
  for (std::size_t tile_z = 0; tile_z < 4; ++tile_z)
    for (std::size_t tile_x = 0; tile_x < 4; ++tile_x)
      for (std::size_t index = 0; index < 64; ++index)
      {
        auto const candidate = Noggit::Ai::proceduralScatterCandidate(
          *wall_scatter.scatter, 1, tile_x, tile_z, index,
          tile_x / 4.0f, (tile_x + 1) / 4.0f,
          tile_z / 4.0f, (tile_z + 1) / 4.0f);
        if (!candidate.active) continue;
        ++wall_candidate_visits[index];
        tiled_wall_candidates[index] = candidate;
      }
  for (std::size_t index = 0; index < 64; ++index)
  {
    require(wall_candidate_visits[index] == 1,
            "a tiled wall candidate was lost or placed more than once");
    auto const& current = tiled_wall_candidates[index];
    auto const& next = tiled_wall_candidates[(index + 1) % 64];
    require(std::hypot(current.u - next.u, current.v - next.v) <= .051f,
            "the tiled wall chain contains a gap, including at loop closure");
  }
  auto role_wall_specification = specification;
  for (auto const* path : {"world/wall_a.m2", "world/wall_b.m2",
                           "world/wall_c.m2", "world/wall_d.m2"})
    role_wall_specification["assets"].push_back({
      {"path", path}, {"role", "wall"}, {"weight", 1},
      {"min_scale", 1.0}, {"max_scale", 1.3}, {"spacing_multiplier", 1.0}});
  role_wall_specification["assets"][3]["weight"] = 3;
  role_wall_specification["assets"][4]["weight"] = 3;
  role_wall_specification["regions"].push_back({
    {"name", "base_perimeter"}, {"role", "wall"},
    {"points", nlohmann::json::array({{{"u", .2}, {"v", .2}},
      {{"u", .8}, {"v", .2}}, {{"u", .8}, {"v", .8}}, {{"u", .2}, {"v", .8}}})},
    {"density_per_tile", 256}, {"min_spacing_ratio", .004},
    {"min_height", -100}, {"max_height", 100},
    {"min_slope_degrees", 0}, {"max_slope_degrees", 90},
    {"cluster_scale", 2}, {"cluster_strength", 0}
  });
  auto const role_wall_scatter = Noggit::Ai::parseProceduralScatter(role_wall_specification);
  require(role_wall_scatter.scatter.has_value(), "valid wall-role scatter rejected");
  require(Noggit::Ai::proceduralScatterIsWallRegion(role_wall_scatter.scatter->regions[1])
          && !Noggit::Ai::proceduralScatterIsWallRegion(role_wall_scatter.scatter->regions[0]),
          "wall-role regions must be detected without the _wall name suffix");
  std::array<bool, 4> wall_variants{};
  auto previous_wall_asset = std::numeric_limits<std::size_t>::max();
  auto current_wall_run = std::size_t{0};
  auto longest_wall_run = std::size_t{0};
  for (std::size_t index = 0; index < 256; ++index)
  {
    auto const candidate = Noggit::Ai::proceduralScatterCandidate(
      *role_wall_scatter.scatter, 1, 26, 26, index, 0.0f, 1.0f, 0.0f, 1.0f);
    auto const on_wall = std::abs(candidate.u - .2f) < .001f
      || std::abs(candidate.u - .8f) < .001f
      || std::abs(candidate.v - .2f) < .001f
      || std::abs(candidate.v - .8f) < .001f;
    require(candidate.active && on_wall,
            "wall-role candidates must form a deterministic perimeter chain");
    require(role_wall_scatter.scatter->assets[candidate.asset_index].role == "wall",
            "wall-role regions must only place wall assets");
    require(candidate.asset_index >= 3 && candidate.asset_index < 7,
            "wall-role region selected an unexpected asset");
    wall_variants[candidate.asset_index - 3] = true;
    current_wall_run = candidate.asset_index == previous_wall_asset
      ? current_wall_run + 1 : 1;
    longest_wall_run = std::max(longest_wall_run, current_wall_run);
    previous_wall_asset = candidate.asset_index;
  }
  require(std::all_of(wall_variants.begin(), wall_variants.end(), [](bool seen) { return seen; }),
          "wall chains must use every supplied model variant");
  require(longest_wall_run <= 8,
          "wall variants must not form long repeated patches");
  require(Noggit::Ai::proceduralScatterContains(parsed.scatter->regions[0].points, 0.2f, 0.2f)
          && !Noggit::Ai::proceduralScatterContains(parsed.scatter->regions[0].points, 0.05f, 0.2f),
          "polygon containment changed");
  require(Noggit::Ai::proceduralScatterRegionIntersects(
            parsed.scatter->regions[0], 0.0f, 0.5f, 0.0f, 0.5f)
          && !Noggit::Ai::proceduralScatterRegionIntersects(
            parsed.scatter->regions[0], 0.91f, 1.0f, 0.0f, 0.09f),
          "region/tile preflight intersection changed");
  require(Noggit::Ai::proceduralScatterExcluded(*parsed.scatter, 0.5f, 0.5f, 1000.0f, 500.0f)
          && !Noggit::Ai::proceduralScatterExcluded(*parsed.scatter, 0.1f, 0.5f, 1000.0f, 500.0f),
          "corridor exclusion changed");

  specification["regions"][0]["extra"] = true;
  require(!Noggit::Ai::parseProceduralScatter(specification).scatter,
          "strict nested validation accepted an extra field");
  specification["regions"][0].erase("extra");
  specification["assets"][0]["weight"] = std::numeric_limits<double>::infinity();
  require(!Noggit::Ai::parseProceduralScatter(specification).scatter,
          "non-finite weight accepted");
}
