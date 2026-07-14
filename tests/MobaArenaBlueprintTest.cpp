// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/MobaArenaBlueprint.hpp>
#include <noggit/ai/ProceduralLayout.hpp>
#include <noggit/ai/ProceduralLiquidLayout.hpp>
#include <noggit/ai/ProceduralScatter.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
  void require(bool condition, char const* message)
  {
    if (!condition) throw std::runtime_error(message);
  }

  nlohmann::json specification()
  {
    return {
      {"texture_paths", {"tileset/grass.blp", "tileset/dirt.blp",
                         "tileset/mud.blp", "tileset/rock.blp"}},
      {"liquid_type_id", 1},
      {"assets", nlohmann::json::array({
        {{"path", "world/tree-a.m2"}, {"role", "canopy"}, {"weight", 3},
         {"min_scale", .8}, {"max_scale", 1.2}, {"spacing_multiplier", 1.2}},
        {{"path", "world/tree-b.m2"}, {"role", "canopy"}, {"weight", 2},
         {"min_scale", .7}, {"max_scale", 1.1}, {"spacing_multiplier", 1.0}},
        {{"path", "world/tree-c.m2"}, {"role", "canopy"}, {"weight", 2},
         {"min_scale", .8}, {"max_scale", 1.3}, {"spacing_multiplier", 1.1}},
        {{"path", "world/bush.m2"}, {"role", "understory"}, {"weight", 2},
         {"min_scale", .7}, {"max_scale", 1.2}, {"spacing_multiplier", .6}},
        {{"path", "world/sapling.m2"}, {"role", "understory"}, {"weight", 2},
         {"min_scale", .6}, {"max_scale", 1.1}, {"spacing_multiplier", .5}},
        {{"path", "world/expansion07/doodads/riverzone/8riv_rockwall_tall_01.m2"},
         {"role", "rock"}, {"weight", 1},
         {"min_scale", .9}, {"max_scale", 1.1}, {"spacing_multiplier", 1.0}},
        {{"path", "world/expansion07/doodads/riverzone/8riv_rockwall_tall_03.m2"},
         {"role", "rock"}, {"weight", 1},
         {"min_scale", .85}, {"max_scale", 1.15}, {"spacing_multiplier", .9}},
        {{"path", "world/fern.m2"}, {"role", "detail"}, {"weight", 2},
         {"min_scale", .6}, {"max_scale", 1.0}, {"spacing_multiplier", .4}}
      })},
      {"seed", "moba-test"}, {"base_height", 20}, {"river_depth", 8},
      {"lane_width_ratio", .04}, {"river_width_ratio", .03},
      {"lane_curvature", .6}, {"river_curvature", .5},
      {"jungle_roughness", 5}, {"vegetation_density_per_tile", 64},
      {"ground_effect_texture_id", 17}
    };
  }
}

int main()
{
  auto footprint = [](std::size_t width, std::size_t height)
  {
    std::vector<std::pair<std::size_t, std::size_t>> tiles;
    for (std::size_t z = 0; z < height; ++z)
      for (std::size_t x = 0; x < width; ++x) tiles.emplace_back(x + 20, z + 20);
    return tiles;
  };
  require(!Noggit::Ai::validateMobaArenaFootprint(footprint(3, 3)),
          "a full 3x3 footprint must be accepted");
  require(!Noggit::Ai::validateMobaArenaFootprint(footprint(4, 4)),
          "a full square footprint must be accepted");
  require(Noggit::Ai::validateMobaArenaFootprint(footprint(5, 5)).has_value(),
          "a footprint larger than the bounded MOBA scatter budget was accepted");
  require(Noggit::Ai::validateMobaArenaFootprint(footprint(2, 2)).has_value(),
          "a footprint smaller than 3x3 was accepted");
  require(Noggit::Ai::validateMobaArenaFootprint(footprint(4, 3)).has_value(),
          "a stretched footprint was accepted");
  auto incomplete = footprint(3, 3);
  incomplete.pop_back();
  require(Noggit::Ai::validateMobaArenaFootprint(incomplete).has_value(),
          "a footprint with a missing tile was accepted");

  auto const first = Noggit::Ai::compileMobaArenaBlueprint(specification());
  auto const second = Noggit::Ai::compileMobaArenaBlueprint(specification());
  require(first == second, "blueprint must be deterministic");
  require(first.at("topology").at("lanes") == 3
          && first.at("topology").at("bases") == 2
          && first.at("topology").at("objective_pits") == 2
          && first.at("topology").at("fortified_bases") == 2
          && first.at("topology").at("jungle_camps") == 12
          && first.at("topology").at("jungle_masses") == 4
          && first.at("topology").at("jungle_ridges") == 4
          && first.at("topology").at("jungle_wall_bands") == 4
          && first.at("topology").at("jungle_path_wall_bands") == 8
          && first.at("topology").at("jungle_paths") == 8
          && first.at("topology").at("public_entrances_per_base") == 3
          && first.at("topology").at("decorative_defender_gates_per_base") == 2,
          "fixed MOBA topology changed");

  auto const& calls = first.at("next_calls");
  require(calls.size() == 6
          && calls[0].at("name") == "apply_terrain_layout_on_map"
          && calls[1].at("name") == "apply_liquid_layout_on_map"
          && calls[2].at("name") == "apply_ground_effect_on_map"
          && calls[2].at("arguments").at("texture_path") == "tileset/grass.blp"
          && calls[2].at("arguments").at("effect_id") == 17
          && calls[3].at("name") == "scatter_assets_on_map"
          && calls[4].at("name") == "scatter_assets_on_map"
          && calls[5].at("name") == "scatter_assets_on_map",
          "generic execution pipeline changed");
  auto const terrain = Noggit::Ai::parseProceduralLayout(calls[0].at("arguments"));
  auto const liquid = Noggit::Ai::parseProceduralLiquidLayout(calls[1].at("arguments"));
  auto const walls = Noggit::Ai::parseProceduralScatter(calls[3].at("arguments"));
  auto const path_walls = Noggit::Ai::parseProceduralScatter(calls[4].at("arguments"));
  auto const vegetation = Noggit::Ai::parseProceduralScatter(calls[5].at("arguments"));
  if (!terrain.layout) throw std::runtime_error("terrain: " + terrain.error);
  if (!liquid.layout) throw std::runtime_error("liquid: " + liquid.error);
  if (!walls.scatter) throw std::runtime_error("walls: " + walls.error);
  if (!path_walls.scatter) throw std::runtime_error("path walls: " + path_walls.error);
  if (!vegetation.scatter) throw std::runtime_error("vegetation: " + vegetation.error);
  require(terrain.layout && liquid.layout && walls.scatter
            && path_walls.scatter && vegetation.scatter,
          "blueprint must compile to valid generic tool arguments");

  std::size_t lanes = 0;
  for (auto const& feature : terrain.layout->features)
    if (feature.name.ends_with("_lane")) ++lanes;
  require(lanes == 3 && terrain.layout->features.size() == 30,
          "terrain topology is incomplete");
  std::size_t base_outer_walls = 0;
  std::size_t base_inner_courts = 0;
  std::size_t defender_gates = 0;
  std::size_t jungle_masses = 0;
  std::size_t jungle_paths = 0;
  std::size_t jungle_ridges = 0;
  for (auto const& feature : terrain.layout->features)
  {
    if (feature.name.ends_with("_outer_wall")) ++base_outer_walls;
    if (feature.name.ends_with("_inner_court")) ++base_inner_courts;
    if (feature.name.ends_with("_defender_gate")) ++defender_gates;
    if (feature.name.starts_with("jungle_") && feature.name.ends_with("_mass")) ++jungle_masses;
    if (feature.name.starts_with("jungle_") && feature.name.ends_with("_path")) ++jungle_paths;
    if (feature.name.starts_with("jungle_") && feature.name.ends_with("_ridge")) ++jungle_ridges;
  }
  require(base_outer_walls == 2 && base_inner_courts == 2 && defender_gates == 4,
          "fortified base topology is incomplete");
  require(jungle_masses == 4 && jungle_paths == 8 && jungle_ridges == 4,
          "jungle floors, ridges or paths are incomplete");
  std::vector<std::size_t> feature_cores(terrain.layout->features.size());
  for (int z = 0; z < 256; ++z)
    for (int x = 0; x < 256; ++x)
    {
      auto const sample = Noggit::Ai::sampleProceduralLayout(
        *terrain.layout, (x + .5f) / 256.0f, (z + .5f) / 256.0f,
        0.0f, 0.0f, 1066.0f, 1066.0f);
      for (std::size_t i = 0; i < feature_cores.size(); ++i)
        feature_cores[i] += sample.feature_masks[i] >= .999f;
    }
  for (std::size_t i = 0; i < feature_cores.size(); ++i)
    if (feature_cores[i] == 0)
      throw std::runtime_error("missing effective core: " + terrain.layout->features[i].name);
  auto sampleHeight = [&](float u, float v)
  {
    return Noggit::Ai::sampleProceduralLayout(
      *terrain.layout, u, v, 0.0f, 0.0f, 1600.0f, 1600.0f).height;
  };
  require(sampleHeight(.50f, .12f) > 42.0f,
          "jungle ridge is not high enough to block passage");
  require(sampleHeight(.50f, .22f) < 22.0f,
          "jungle main path was not carved back to playable height");
  std::size_t jungle_wall_bands = 0;
  for (auto const& region : walls.scatter->regions)
  {
    require(region.role == "rock", "wall scatter accepted a decorative role");
    ++jungle_wall_bands;
    require(region.name.ends_with("_wall"),
            "collidable rock assets must be reserved for named jungle walls");
    require(region.density_per_tile == 256
              && region.min_height >= 38.0f && region.min_spacing_ratio <= .0021f
              && region.cluster_strength == 0.0f,
            "jungle wall bands must be dense and limited to raised ridges");
    require(sampleHeight(.50f, .12f) >= region.min_height
              && sampleHeight(.50f, .22f) < region.min_height,
            "wall height filter must keep ridge pieces and reject path openings");
  }
  require(jungle_wall_bands == 4, "the four jungles need a collidable wall band");
  require(path_walls.scatter->regions.size() == 8,
          "both sides of the eight jungle paths need wall bands");
  for (std::size_t region_index = 0;
       region_index < path_walls.scatter->regions.size(); ++region_index)
  {
    auto const& region = path_walls.scatter->regions[region_index];
    require(region.role == "rock" && region.name.ends_with("_path_wall")
              && region.density_per_tile == 256 && region.points.size() == 6
              && region.min_height <= 10.0f,
            "jungle path walls must follow both sides at playable ground height");
    auto const side_width = std::hypot(
      region.points[1].u - region.points[4].u,
      region.points[1].v - region.points[4].v);
    require(side_width >= .06f,
            "jungle path wall loop does not cover both sides of the path");
    auto viable = std::size_t{0};
    for (std::size_t index = 0; index < region.density_per_tile; ++index)
    {
      auto const candidate = Noggit::Ai::proceduralScatterCandidate(
        *path_walls.scatter, region_index, 0, 0, index,
        0.0f, 1.0f, 0.0f, 1.0f);
      auto const height = sampleHeight(candidate.u, candidate.v);
      viable += candidate.active
        && !Noggit::Ai::proceduralScatterExcluded(
          *path_walls.scatter, candidate.u, candidate.v, 1600.0f, 1600.0f)
        && height >= region.min_height && height <= region.max_height;
    }
    require(viable >= 64,
            "jungle path wall band has too few usable collidable pieces");
  }
  require(std::all_of(walls.scatter->assets.begin(), walls.scatter->assets.end(),
            [](auto const& asset)
            { return asset.role == "rock" && asset.min_scale >= 2.0f; })
          && std::all_of(path_walls.scatter->assets.begin(), path_walls.scatter->assets.end(),
            [](auto const& asset)
            { return asset.role == "rock" && asset.min_scale >= 2.0f; })
          && std::none_of(vegetation.scatter->assets.begin(), vegetation.scatter->assets.end(),
            [](auto const& asset) { return asset.role == "rock"; }),
          "wall and vegetation assets must be isolated in separate batches");
  require(walls.scatter->exclusions.size() == 28
            && path_walls.scatter->exclusions.size() == 28
            && vegetation.scatter->exclusions.size() == 28,
          "lanes, river, bases, objectives, camps and jungle paths need clear openings");
  require(sampleHeight(.02f, .98f) > 42.0f,
          "base rear wall is not protected");
  auto const gate_a = sampleHeight(.09f, .73f);
  auto const gate_b = sampleHeight(.18f, .82f);
  auto const gate_c = sampleHeight(.23f, .93f);
  if (!(gate_a < 22.0f && gate_b < 22.0f && gate_c < 22.0f))
    throw std::runtime_error("the three public base entrances are not open: "
      + std::to_string(gate_a) + "," + std::to_string(gate_b) + ","
      + std::to_string(gate_c));
  require(sampleHeight(.12f, .78f) > 42.0f,
          "decorative defender gate became a public entrance");
  require(Noggit::Ai::proceduralScatterExcluded(
            *vegetation.scatter, .50f, .50f, 1066.0f, 1066.0f),
          "middle lane and river crossing must be protected from decoration");
  require(!Noggit::Ai::proceduralScatterExcluded(
            *vegetation.scatter, .40f, .34f, 1066.0f, 1066.0f),
          "jungle interior should remain available for decoration");
  require(Noggit::Ai::proceduralScatterExcluded(
            *vegetation.scatter, .50f, .30f, 1066.0f, 1066.0f),
          "jungle paths must remain open through the wall bands");
  require(Noggit::Ai::proceduralScatterExcluded(
            *vegetation.scatter, .50f, .22f, 1066.0f, 1066.0f),
          "camp clearings must remain free of decoration");
  for (std::size_t region_index = 0;
       region_index < walls.scatter->regions.size(); ++region_index)
  {
    auto const& region = walls.scatter->regions[region_index];
    auto viable = std::size_t{0};
    for (std::size_t index = 0; index < region.density_per_tile; ++index)
    {
      auto const candidate = Noggit::Ai::proceduralScatterCandidate(
        *walls.scatter, region_index, 0, 0, index, 0.0f, 1.0f, 0.0f, 1.0f);
      auto const is_clear = !Noggit::Ai::proceduralScatterExcluded(
        *walls.scatter, candidate.u, candidate.v, 1600.0f, 1600.0f);
      auto const is_high = sampleHeight(candidate.u, candidate.v) >= region.min_height;
      if (candidate.active && is_clear && is_high)
        ++viable;
    }
    if (viable < 40)
      throw std::runtime_error("insufficient deterministic wall chain for "
        + region.name + ": " + std::to_string(viable));
  }
  auto candidateCount = [](Noggit::Ai::ProceduralScatter const& scatter, int tiles)
  {
    auto count = std::size_t{0};
    for (int z = 0; z < tiles; ++z)
      for (int x = 0; x < tiles; ++x)
        for (auto const& region : scatter.regions)
          if (Noggit::Ai::proceduralScatterRegionIntersects(
                region, x / static_cast<float>(tiles), (x + 1) / static_cast<float>(tiles),
                z / static_cast<float>(tiles), (z + 1) / static_cast<float>(tiles)))
            count += region.density_per_tile;
    return count;
  };
  for (auto const* scatter : {&*walls.scatter, &*path_walls.scatter,
                              &*vegetation.scatter})
  {
    require(candidateCount(*scatter, 2) <= 4096,
            "a 2x2 MOBA batch must fit the compact scatter budget");
    require(candidateCount(*scatter, 3) <= 16384,
            "a 3x3 MOBA batch must fit the global scatter candidate limit");
    require(candidateCount(*scatter, 4) <= 16384,
            "a 4x4 MOBA batch must fit the global scatter candidate limit");
  }

  auto sparse_vegetation = specification();
  sparse_vegetation["vegetation_density_per_tile"] = 1;
  auto const sparse_blueprint = Noggit::Ai::compileMobaArenaBlueprint(sparse_vegetation);
  auto const sparse_walls = Noggit::Ai::parseProceduralScatter(
    sparse_blueprint.at("next_calls").at(3).at("arguments"));
  require(sparse_walls.scatter
            && std::all_of(sparse_walls.scatter->regions.begin(),
                           sparse_walls.scatter->regions.end(),
                           [](auto const& region)
                           { return region.density_per_tile == 256; }),
          "vegetation density must not weaken gameplay wall chains");

  auto invalid = specification();
  invalid["extra"] = true;
  try
  {
    static_cast<void>(Noggit::Ai::compileMobaArenaBlueprint(invalid));
    require(false, "extra root field accepted");
  }
  catch (std::invalid_argument const&)
  {
  }
}
