// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/MobaArenaBlueprint.hpp>
#include <noggit/ai/ProceduralLayout.hpp>
#include <noggit/ai/ProceduralLiquidLayout.hpp>
#include <noggit/ai/ProceduralScatter.hpp>

#include <nlohmann/json.hpp>

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
        {{"path", "world/rock.wmo"}, {"role", "rock"}, {"weight", 1},
         {"min_scale", .9}, {"max_scale", 1.1}, {"spacing_multiplier", 1.0}},
        {{"path", "world/rock2.m2"}, {"role", "rock"}, {"weight", 1},
         {"min_scale", .7}, {"max_scale", 1.2}, {"spacing_multiplier", .8}},
        {{"path", "world/fern.m2"}, {"role", "detail"}, {"weight", 2},
         {"min_scale", .6}, {"max_scale", 1.0}, {"spacing_multiplier", .4}}
      })},
      {"seed", "moba-test"}, {"base_height", 20}, {"river_depth", 8},
      {"lane_width_ratio", .04}, {"river_width_ratio", .03},
      {"lane_curvature", .6}, {"river_curvature", .5},
      {"jungle_roughness", 5}, {"vegetation_density_per_tile", 64}
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
          && first.at("topology").at("jungle_paths") == 8
          && first.at("topology").at("public_entrances_per_base") == 3
          && first.at("topology").at("decorative_defender_gates_per_base") == 2,
          "fixed MOBA topology changed");

  auto const& calls = first.at("next_calls");
  require(calls.size() == 3
          && calls[0].at("name") == "apply_terrain_layout_on_map"
          && calls[1].at("name") == "apply_liquid_layout_on_map"
          && calls[2].at("name") == "scatter_assets_on_map",
          "generic execution pipeline changed");
  auto const terrain = Noggit::Ai::parseProceduralLayout(calls[0].at("arguments"));
  auto const liquid = Noggit::Ai::parseProceduralLiquidLayout(calls[1].at("arguments"));
  auto const scatter = Noggit::Ai::parseProceduralScatter(calls[2].at("arguments"));
  if (!terrain.layout) throw std::runtime_error("terrain: " + terrain.error);
  if (!liquid.layout) throw std::runtime_error("liquid: " + liquid.error);
  if (!scatter.scatter) throw std::runtime_error("scatter: " + scatter.error);
  require(terrain.layout && liquid.layout && scatter.scatter,
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
            *scatter.scatter, .50f, .50f, 1066.0f, 1066.0f),
          "middle lane and river crossing must be protected from decoration");
  require(!Noggit::Ai::proceduralScatterExcluded(
            *scatter.scatter, .50f, .30f, 1066.0f, 1066.0f),
          "jungle interior should remain available for decoration");
  require(Noggit::Ai::proceduralScatterExcluded(
            *scatter.scatter, .50f, .22f, 1066.0f, 1066.0f),
          "camp clearings must remain free of decoration");
  std::size_t candidates_on_two_by_two = 0;
  for (int z = 0; z < 2; ++z)
    for (int x = 0; x < 2; ++x)
      for (auto const& region : scatter.scatter->regions)
        if (Noggit::Ai::proceduralScatterRegionIntersects(
              region, x * .5f, (x + 1) * .5f, z * .5f, (z + 1) * .5f))
          candidates_on_two_by_two += region.density_per_tile;
  require(candidates_on_two_by_two <= 4096,
          "a 2x2 MOBA footprint must fit the global scatter candidate limit");
  std::size_t candidates_on_three_by_three = 0;
  for (int z = 0; z < 3; ++z)
    for (int x = 0; x < 3; ++x)
      for (auto const& region : scatter.scatter->regions)
        if (Noggit::Ai::proceduralScatterRegionIntersects(
              region, x / 3.0f, (x + 1) / 3.0f, z / 3.0f, (z + 1) / 3.0f))
          candidates_on_three_by_three += region.density_per_tile;
  require(candidates_on_three_by_three <= 16384,
          "a 3x3 MOBA footprint must fit the global scatter candidate limit");

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
