// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/MobaArenaBlueprint.hpp>
#include <noggit/ai/ProceduralLayout.hpp>
#include <noggit/ai/ProceduralLiquidLayout.hpp>
#include <noggit/ai/ProceduralProps.hpp>
#include <noggit/ai/ProceduralScatter.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
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
        {{"path", "world/expansion07/doodads/bloodtroll/8tr_ancientblood_walltall01.m2"},
         {"role", "wall"}, {"weight", 2},
         {"min_scale", 1.5}, {"max_scale", 1.7}, {"spacing_multiplier", 1.0}},
        {{"path", "world/expansion07/doodads/bloodtroll/8tr_ancientblood_walltall02.m2"},
         {"role", "wall"}, {"weight", 1},
         {"min_scale", 1.5}, {"max_scale", 1.7}, {"spacing_multiplier", 1.0}},
        {{"path", "world/rock-a.m2"}, {"role", "rock"}, {"weight", 1},
         {"min_scale", .9}, {"max_scale", 1.4}, {"spacing_multiplier", .9}},
        {{"path", "world/rock-b.m2"}, {"role", "rock"}, {"weight", 1},
         {"min_scale", .8}, {"max_scale", 1.3}, {"spacing_multiplier", .8}},
        {{"path", "world/fern.m2"}, {"role", "detail"}, {"weight", 2},
         {"min_scale", .6}, {"max_scale", 1.0}, {"spacing_multiplier", .4}}
      })},
      {"prop_paths", {
        {"base_landmark", "world/expansion07/doodads/human/8hu_kultiras_fountain01.m2"},
        {"objective_landmark", "world/expansion06/doodads/dungeon/doodads/7du_tombofsargeras_elunestatue01.m2"},
        {"camp_marker", "world/expansion08/doodads/oribos/9ob_oribos_brazier01.m2"},
        {"lane_lamp", "world/expansion06/doodads/nightelf/7ne_nightelf_streetlight01.m2"},
        {"team_left_light", "world/noggit/lights/noggit_light_deepskyblue01.m2"},
        {"team_right_light", "world/noggit/lights/noggit_light_orange01.m2"},
        {"river_light", "world/noggit/lights/noggit_light_purple_withshadows01.m2"},
        {"flame_light", "world/noggit/lights/noggit_light_orange_withshadows01.m2"},
        {"lamp_light", "world/noggit/lights/noggit_light_dimwhite01.m2"}
      }},
      {"seed", "moba-test"}, {"base_height", 20}, {"river_depth", 8},
      {"lane_width_ratio", .04}, {"river_width_ratio", .03},
      {"lane_curvature", .6}, {"river_curvature", .5},
      {"jungle_roughness", 5}, {"vegetation_density_per_tile", 64},
      {"ground_effect_texture_id", 17},
      {"skybox_path", "environments/stars/tanaan_patch_junglesky01.m2"},
      {"skybox_flags", 1}
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
  require(!Noggit::Ai::validateMobaArenaFootprint(footprint(2, 2)),
          "a full 2x2 footprint must be accepted");
  require(!Noggit::Ai::validateMobaArenaFootprint(footprint(3, 3)),
          "a full 3x3 footprint must be accepted");
  require(!Noggit::Ai::validateMobaArenaFootprint(footprint(4, 4)),
          "a full square footprint must be accepted");
  require(Noggit::Ai::validateMobaArenaFootprint(footprint(5, 5)).has_value(),
          "a footprint larger than the bounded MOBA scatter budget was accepted");
  require(Noggit::Ai::validateMobaArenaFootprint(footprint(1, 1)).has_value(),
          "a footprint smaller than 2x2 was accepted");
  require(Noggit::Ai::validateMobaArenaFootprint(footprint(4, 3)).has_value(),
          "a stretched footprint was accepted");
  auto incomplete = footprint(3, 3);
  incomplete.pop_back();
  require(Noggit::Ai::validateMobaArenaFootprint(incomplete).has_value(),
          "a footprint with a missing tile was accepted");

  auto const first = Noggit::Ai::compileMobaArenaBlueprint(specification(), 4);
  auto const second = Noggit::Ai::compileMobaArenaBlueprint(specification(), 4);
  require(first == second, "blueprint must be deterministic");
  require(first.at("topology").at("lanes") == 3
          && first.at("topology").at("bases") == 2
          && first.at("topology").at("objective_pits") == 2
          && first.at("topology").at("fortified_bases") == 2
          && first.at("topology").at("jungle_camps") == 12
          && first.at("topology").at("camp_alcoves") == 12
          && first.at("topology").at("camp_entrances") == 32
          && first.at("topology").at("jungle_clear_routes") == 4
          && first.at("topology").at("river_branch_paths") == 4
          && first.at("topology").at("jungle_floors") == 4
          && first.at("topology").at("jungle_wall_bands") == 0
          && first.at("topology").at("base_wall_bands") == 2
          && first.at("topology").at("jungle_path_wall_bands") == 0
          && first.at("topology").at("jungle_paths") == 16
          && first.at("topology").at("jungle_wall_cuts") == 8
          && first.at("topology").at("door_paths") == 4
          && first.at("topology").at("jungle_doors") == 20
          && first.at("topology").at("camp_rings") == 0
          && first.at("topology").at("public_entrances_per_base") == 3
          && first.at("topology").at("landmarks") == 4
          && first.at("topology").at("camp_braziers") == 12
          && first.at("topology").at("entrance_braziers") == 12,
          "fixed MOBA topology changed");
  require(first.at("topology").at("jungle_wall_masses").get<std::size_t>() == 16,
          "the jungle needs four separate wall islands per quadrant");
  require(first.at("topology").at("lane_lamps").get<std::size_t>() >= 30
            && first.at("topology").at("dynamic_lights").get<std::size_t>() >= 40,
          "the ambience layer lost its lamps or dynamic lights");

  auto const& calls = first.at("next_calls");
  require(calls.size() == 7
          && calls[0].at("name") == "apply_terrain_layout_on_map"
          && calls[1].at("name") == "apply_liquid_layout_on_map"
          && calls[2].at("name") == "apply_ground_effect_on_map"
          && calls[2].at("arguments").at("texture_path") == "tileset/grass.blp"
          && calls[2].at("arguments").at("effect_id") == 17
          && calls[3].at("name") == "scatter_assets_on_map"
          && calls[4].at("name") == "place_props_on_map"
          && calls[5].at("name") == "scatter_assets_on_map"
          && calls[6].at("name") == "apply_skybox_on_map"
          && calls[6].at("arguments").at("skybox_path")
            == "environments/stars/tanaan_patch_junglesky01.m2"
          && calls[6].at("arguments").at("flags") == 1,
          "generic execution pipeline changed");
  auto const terrain = Noggit::Ai::parseProceduralLayout(calls[0].at("arguments"));
  auto const liquid = Noggit::Ai::parseProceduralLiquidLayout(calls[1].at("arguments"));
  auto const walls = Noggit::Ai::parseProceduralScatter(calls[3].at("arguments"));
  auto const props = Noggit::Ai::parseProceduralProps(calls[4].at("arguments"));
  auto const vegetation = Noggit::Ai::parseProceduralScatter(calls[5].at("arguments"));
  if (!terrain.layout) throw std::runtime_error("terrain: " + terrain.error);
  if (!liquid.layout) throw std::runtime_error("liquid: " + liquid.error);
  if (!walls.scatter) throw std::runtime_error("walls: " + walls.error);
  if (!props.props) throw std::runtime_error("props: " + props.error);
  if (!vegetation.scatter) throw std::runtime_error("vegetation: " + vegetation.error);
  require(terrain.layout && liquid.layout && walls.scatter
            && props.props && vegetation.scatter,
          "blueprint must compile to valid generic tool arguments");
  // base_height 20 and river_depth 8 put the bed bottom at 12; the water
  // column must stay under WoW's swim threshold so the river is wadeable.
  require(liquid.layout->features.front().points.front().height <= 13.01f,
          "the river water column must stay wadeable on foot");

  for (auto const footprint_side : {std::size_t{2}, std::size_t{3}, std::size_t{4}})
  {
    auto const blueprint = Noggit::Ai::compileMobaArenaBlueprint(
      specification(), footprint_side);
    require(blueprint.at("connectivity").at("unreachable_pois").empty(),
            "every camp, objective and base court must stay reachable "
            "from the lane network");
    require(blueprint.at("connectivity").at("enclosed_jungle_cells")
              .get<std::size_t>() == 0,
            "walls must not seal any walkable jungle pocket");
    for (auto const call_index : {std::size_t{3}})
    {
      auto const parsed = Noggit::Ai::parseProceduralScatter(
        blueprint.at("next_calls").at(call_index).at("arguments"));
      require(parsed.scatter.has_value(), "footprint wall scatter is invalid");
      for (std::size_t region_index = 0;
           region_index < parsed.scatter->regions.size(); ++region_index)
      {
        auto const& region = parsed.scatter->regions[region_index];
        auto const density = region.density_per_tile;
        require(density > 0, "a footprint wall chain is empty");
        auto candidates = std::vector<Noggit::Ai::ProceduralScatterCandidate>{};
        candidates.reserve(density);
        for (std::size_t index = 0; index < density; ++index)
          candidates.push_back(Noggit::Ai::proceduralScatterCandidate(
            *parsed.scatter, region_index, 0, 0, index,
            0.0f, 1.0f, 0.0f, 1.0f));
        auto const world_size = static_cast<float>(footprint_side) * (1600.0f / 3.0f);
        require(std::all_of(candidates.begin(), candidates.end(),
                            [](auto const& candidate) { return candidate.active; }),
                "wall density produced an inactive perimeter candidate");
        require(region.name.find("_chain") != std::string::npos,
                "every wall region must be an open graph-derived chain");
        for (std::size_t edge = 0; edge < region.points.size(); ++edge)
        {
          auto const& start = region.points[edge];
          auto const& end = region.points[(edge + 1) % region.points.size()];
          auto const du = end.u - start.u;
          auto const dv = end.v - start.v;
          auto const length = std::hypot(du, dv);
          if (length <= .000001f) continue;
          auto progresses = std::vector<float>{};
          for (auto const& candidate : candidates)
          {
            auto const offset_u = candidate.u - start.u;
            auto const offset_v = candidate.v - start.v;
            auto const cross = std::abs(offset_u * dv - offset_v * du) / length;
            auto const progress = (offset_u * du + offset_v * dv) / (length * length);
            if (cross > .0001f || progress < -.0001f || progress > 1.0001f) continue;
            progresses.push_back(progress);
          }
          if (edge + 1 == region.points.size()) continue;
          if (candidates.size() < region.points.size()) continue;
          require(!progresses.empty(), "a wall edge received no candidates");
          std::sort(progresses.begin(), progresses.end());
          require(progresses.front() * length * world_size <= 16.01f
                    && (1.0f - progresses.back()) * length * world_size <= 16.01f,
                  "every wall edge must reach both corners within 16 metres");
          for (std::size_t i = 1; i < progresses.size(); ++i)
            require((progresses[i] - progresses[i - 1]) * length * world_size <= 32.01f,
                    "wall density must keep physical gaps within 32 metres");
        }
      }
    }
  }

  std::size_t lanes = 0;
  for (auto const& feature : terrain.layout->features)
    if (feature.name.ends_with("_lane")) ++lanes;
  require(lanes == 3, "terrain topology is incomplete");
  require(terrain.layout->features.front().name == "arena_ground",
          "the whole arena needs a flat textured background feature");
  std::size_t base_aprons = 0;
  std::size_t base_inner_courts = 0;
  std::size_t jungle_paths = 0;
  std::size_t jungle_wall_cuts = 0;
  std::size_t jungle_wall_masses = 0;
  std::size_t camp_floors = 0;
  for (auto const& feature : terrain.layout->features)
  {
    if (feature.name.ends_with("_base_apron")) ++base_aprons;
    if (feature.name.ends_with("_inner_court")) ++base_inner_courts;
    if (feature.name.starts_with("jungle_") && feature.name.ends_with("_path"))
    {
      ++jungle_paths;
      auto const influence = feature.half_width_ratio
        * (1.0f + feature.width_variation_ratio)
        + feature.transition_width_ratio;
      require(feature.half_width_ratio >= .010f
                && influence <= .023f
                && feature.texture_layer == 0
                && feature.texture_strength >= .90f,
              "jungle paths must stay narrow and visually legible");
    }
    if (feature.name.starts_with("jungle_")
        && feature.name.ends_with("_wall_cut"))
    {
      ++jungle_wall_cuts;
      require(feature.shape == Noggit::Ai::ProceduralLayoutShape::Corridor
                && feature.half_width_ratio >= .007f
                && feature.half_width_ratio <= .009f
                && feature.transition_width_ratio <= .005f
                && feature.texture_layer == 0
                && feature.texture_strength >= .90f,
              "wall cuts must form thin grass passages through broad masses");
    }
    if (feature.name.starts_with("jungle_")
        && feature.name.ends_with("_wall_mass"))
    {
      ++jungle_wall_masses;
      auto twice_area = 0.0f;
      for (std::size_t i = 0; i < feature.points.size(); ++i)
      {
        auto const& a = feature.points[i];
        auto const& b = feature.points[(i + 1) % feature.points.size()];
        twice_area += a.u * b.v - b.u * a.v;
      }
      auto const area = std::abs(twice_area) * .5f;
      require(feature.shape == Noggit::Ai::ProceduralLayoutShape::Area
                && feature.transition_width_ratio <= .015f
                && area >= .010f && area <= .070f
                && feature.points.front().height >= 33.0f
                && feature.points.front().height <= 35.0f,
              "jungle walls must be medium irregular islands");
    }
    require(!feature.name.starts_with("jungle_strand_")
              && !feature.name.ends_with("_ring"),
            "isolated wall strands and repeated rings must not shape the jungle");
    require(!feature.name.ends_with("_camp_relief")
              && !feature.name.ends_with("_camp_access"),
            "the wall-by-default relief encloses camps without per-camp "
            "capsules or access cuts");
    if (feature.name.ends_with("_camp_floor"))
    {
      ++camp_floors;
      require(feature.priority > 65 && feature.priority < 70
                && feature.transition_width_ratio <= .011f
                && feature.points.size() == 6
                && std::hypot(feature.points[0].u - feature.points[3].u,
                              feature.points[0].v - feature.points[3].v) >= .04f
                && feature.points.front().height >= 21.0f
                && feature.points.front().height <= 22.0f
                && feature.texture_layer == 0
                && feature.texture_strength >= .90f,
              "camp floors must cut compact, playable clearings");
    }
    require(!feature.name.ends_with("_defender_gate"),
            "bases must not rely on artificial defender ridges");
  }
  require(base_aprons == 2 && base_inner_courts == 2,
          "fortified base topology is incomplete");
  require(jungle_paths == 16,
          "jungle routes or gank furrows are incomplete");
  require(jungle_wall_cuts == 8,
          "each jungle quadrant needs two thin cuts through broad wall masses");
  require(jungle_wall_masses == 16,
          "the jungle needs four separate wall islands per quadrant");
  require(camp_floors == 12,
          "every jungle camp needs one flat clearing carved from the wall");
  auto longest_wall_segment = 0.0f;
  auto longest_path_segment = 0.0f;
  for (auto const& feature : terrain.layout->features)
  {
    auto const wall = feature.name.starts_with("jungle_")
      && feature.name.ends_with("_wall_mass");
    auto const path = feature.name.starts_with("jungle_")
      && feature.name.ends_with("_path");
    if ((!wall && !path) || feature.points.size() < 2) continue;
    if (wall)
      require(feature.points.size() >= 12,
              "jungle wall silhouettes must use articulated organic loops");
    auto const edge_count = feature.points.size() - (path ? 1 : 0);
    for (std::size_t i = 0; i < edge_count; ++i)
    {
      auto const& a = feature.points[i];
      auto const& b = feature.points[(i + 1) % feature.points.size()];
      auto const length = std::hypot(b.u - a.u, b.v - a.v);
      (wall ? longest_wall_segment : longest_path_segment)
        = std::max(wall ? longest_wall_segment : longest_path_segment, length);
    }
  }
  require(longest_wall_segment <= .11f && longest_path_segment <= .13f,
          "jungle walls and routes must not contain long straight spokes");
  std::vector<std::size_t> feature_cores(terrain.layout->features.size());
  std::array<std::size_t, Noggit::Ai::procedural_layout_max_texture_paths>
    strong_texture_pixels{};
  for (int z = 0; z < 256; ++z)
    for (int x = 0; x < 256; ++x)
    {
      auto const sample = Noggit::Ai::sampleProceduralLayout(
        *terrain.layout, (x + .5f) / 256.0f, (z + .5f) / 256.0f,
        0.0f, 0.0f, 1066.0f, 1066.0f);
      for (std::size_t i = 0; i < feature_cores.size(); ++i)
        feature_cores[i] += sample.feature_masks[i] >= .999f;
      for (std::size_t layer = 0; layer < terrain.layout->texture_paths.size(); ++layer)
        strong_texture_pixels[layer] += sample.quantized_weights[layer] >= 64;
    }
  // The in-app apply_terrain_layout_on_map aborts the whole blueprint chain
  // when ANY feature keeps no effective core pixel, so this check must stay
  // exemption-free.
  for (std::size_t i = 0; i < feature_cores.size(); ++i)
    if (feature_cores[i] == 0)
      throw std::runtime_error("missing effective core: " + terrain.layout->features[i].name);
  for (std::size_t layer = 0; layer < terrain.layout->texture_paths.size(); ++layer)
    require(strong_texture_pixels[layer] >= 64,
            "a terrain texture cannot satisfy the runtime visibility check");
  auto sampleHeight = [&](float u, float v)
  {
    return Noggit::Ai::sampleProceduralLayout(
      *terrain.layout, u, v, 0.0f, 0.0f, 1600.0f, 1600.0f).height;
  };
  auto requireFlat = [&](float u, float v, char const* message)
  {
    auto const height = sampleHeight(u, v);
    if (height < 12.0f || height > 28.0f)
      throw std::runtime_error(std::string{message} + ": " + std::to_string(height));
  };
  auto const lane_height = sampleHeight(.50f, .50f);
  auto const jungle_height = sampleHeight(.4302f, .2788f);
  auto const path_height = sampleHeight(.165f, .5925f);
  auto const path_core = Noggit::Ai::sampleProceduralLayout(
    *terrain.layout, .2775f, .5375f, 0.0f, 0.0f, 1600.0f, 1600.0f);
  require(lane_height >= 17.0f && lane_height <= 23.0f,
          "mid lane must stay at playable base height");
  require(jungle_height >= lane_height + 8.0f
            && jungle_height <= lane_height + 32.0f,
          "jungle floor must rise above the lanes");
  if (path_height < lane_height + .5f || path_height > lane_height + 3.0f)
    throw std::runtime_error("jungle trails must stay level with the open floor: "
      + std::to_string(path_height));
  require(path_core.height >= lane_height + .5f
            && path_core.height <= lane_height + 3.0f
            && path_core.quantized_weights[0] >= 200
            && path_core.quantized_weights[1] <= 32,
          "internal jungle paths must remain low and visually legible");
  {
    auto framed_samples = std::size_t{0};
    auto route_samples = std::size_t{0};
    for (auto const& path : terrain.layout->features)
    {
      if (!path.name.starts_with("jungle_") || !path.name.ends_with("_path"))
        continue;
      for (std::size_t part = 1; part < path.points.size(); ++part)
      {
        auto const du = path.points[part].u - path.points[part - 1].u;
        auto const dv = path.points[part].v - path.points[part - 1].v;
        auto const length = std::hypot(du, dv);
        if (length <= .000001f) continue;
        for (int step = 1; step < 8; ++step)
        {
          auto const t = static_cast<float>(step) / 8.0f;
          auto const u = path.points[part - 1].u + du * t;
          auto const v = path.points[part - 1].v + dv * t;
          auto const normal_u = -dv / length * .04f;
          auto const normal_v = du / length * .04f;
          ++route_samples;
          framed_samples += sampleHeight(u + normal_u, v + normal_v)
                >= lane_height + 8.0f
            || sampleHeight(u - normal_u, v - normal_v)
                >= lane_height + 8.0f;
        }
      }
    }
    require(framed_samples * 5 >= route_samples * 3,
            "jungle walls must frame routes instead of floating as islands");
  }
  struct CampProbe { float u; float v; int kind; };
  auto const camps = std::array{
    CampProbe{.50f, .335f, 1}, CampProbe{.48f, .25f, 2},
    CampProbe{.435f, .18f, 0},
    CampProbe{.745f, .435f, 1}, CampProbe{.74f, .53f, 2},
    CampProbe{.85f, .565f, 0},
    CampProbe{.50f, .665f, 1}, CampProbe{.52f, .75f, 2},
    CampProbe{.565f, .82f, 0},
    CampProbe{.255f, .565f, 1}, CampProbe{.26f, .47f, 2},
    CampProbe{.15f, .435f, 0}
  };

  for (auto const& camp : camps)
  {
    auto const center = sampleHeight(camp.u, camp.v);
    require(center >= lane_height + .5f && center <= lane_height + 3.0f,
            "camp clearings must stay flat below their surrounding relief");
    std::array<bool, 16> open{};
    auto raised_rim_samples = std::size_t{0};
    for (std::size_t i = 0; i < open.size(); ++i)
    {
      auto const angle = static_cast<float>(i) * 6.283185307f / 16.0f;
      auto const rim_height = sampleHeight(
        camp.u + std::cos(angle) * .06f,
        camp.v + std::sin(angle) * .06f);
      raised_rim_samples += rim_height >= center + 8.0f;
      open[i] = rim_height <= center + 4.0f;
    }
    auto access_groups = std::size_t{0};
    for (std::size_t i = 0; i < open.size(); ++i)
      access_groups += open[i] && !open[(i + open.size() - 1) % open.size()];
    if (raised_rim_samples
          < static_cast<std::size_t>(camp.kind == 0 ? 3 : camp.kind == 2 ? 4 : 5))
      throw std::runtime_error("camp clearing lacks surrounding relief at "
        + std::to_string(camp.u) + "," + std::to_string(camp.v) + ": "
        + std::to_string(raised_rim_samples));
    if (camp.kind == 0 ? (access_groups < 1 || access_groups > 3)
          : camp.kind == 1 ? (access_groups < 2 || access_groups > 3)
          : (access_groups < 2 || access_groups > 4))
      throw std::runtime_error("camp accesses must match the route flow at "
        + std::to_string(camp.u) + "," + std::to_string(camp.v) + ": "
        + std::to_string(access_groups));
    // Buffs and medium camps form the through-route; small camps are spurs.
    auto on_clear_route = false;
    auto on_spur = false;
    auto incident_paths = std::size_t{0};
    auto path_ends_at_camp = false;
    for (auto const& feature : terrain.layout->features)
      if (feature.name.starts_with("jungle_") && feature.name.ends_with("_path"))
      {
        auto const on_path = Noggit::Ai::distanceToProceduralShape(
          feature.points, feature.shape, camp.u, camp.v, 1, 1).distance < .001f;
        incident_paths += on_path;
        if (on_path)
          path_ends_at_camp = std::hypot(feature.points.front().u - camp.u,
                                        feature.points.front().v - camp.v) < .001f
            || std::hypot(feature.points.back().u - camp.u,
                          feature.points.back().v - camp.v) < .001f;
        on_clear_route = on_clear_route
          || (on_path && feature.name.ends_with("_clear_path"));
        on_spur = on_spur || (on_path && feature.name.ends_with("_spur_path"));
      }
    require(incident_paths == 1 && !path_ends_at_camp,
            "camp routes must pass through clearings without star hubs or lollipops");
    require(camp.kind == 0 ? (!on_clear_route && on_spur) : on_clear_route,
            "small camps must be spurs while hubs and medium camps stay on the clear route");
  }
  // Full-clear invariant: the hub/medium stretch never touches a lane;
  // the small camp remains reachable as its short internal spur.
  {
    std::vector<std::vector<Noggit::Ai::ProceduralLayoutPoint>> lane_lines;
    for (auto const& feature : terrain.layout->features)
      if (feature.name.ends_with("_lane"))
        lane_lines.push_back(feature.points);
    auto const laneDistance = [&](float u, float v)
    {
      auto best = 10.0f;
      for (auto const& lane : lane_lines)
        for (std::size_t i = 1; i < lane.size(); ++i)
        {
          auto const du = lane[i].u - lane[i - 1].u;
          auto const dv = lane[i].v - lane[i - 1].v;
          auto const length_squared = du * du + dv * dv;
          auto const t = length_squared > 0.0f
            ? std::clamp(((u - lane[i - 1].u) * du + (v - lane[i - 1].v) * dv)
                           / length_squared, 0.0f, 1.0f)
            : 0.0f;
          best = std::min(best, static_cast<float>(std::hypot(
            u - (lane[i - 1].u + du * t), v - (lane[i - 1].v + dv * t))));
        }
      return best;
    };
    auto clear_routes = std::size_t{0};
    for (auto const& feature : terrain.layout->features)
    {
      if (!feature.name.ends_with("_clear_path")) continue;
      ++clear_routes;
      require(feature.points.size() >= 4 && feature.points.size() <= 7,
              "a clear route must chain lane entrance, medium camp, hub and exit");
      auto first_camp = feature.points.size();
      auto last_camp = std::size_t{0};
      auto camp_vertices = std::size_t{0};
      for (std::size_t point_index = 0;
           point_index < feature.points.size(); ++point_index)
        for (auto const& camp : camps)
          if (camp.kind != 0
              && std::hypot(feature.points[point_index].u - camp.u,
                            feature.points[point_index].v - camp.v) < .001f)
          {
            first_camp = std::min(first_camp, point_index);
            last_camp = std::max(last_camp, point_index);
            ++camp_vertices;
          }
      require(camp_vertices == 2 && first_camp < last_camp,
              "each clear route must visit exactly one medium camp and one hub");
      for (std::size_t i = first_camp; i < last_camp; ++i)
        for (int step = 0; step <= 16; ++step)
        {
          auto const t = static_cast<float>(step) / 16.0f;
          auto const u = feature.points[i].u
            + (feature.points[i + 1].u - feature.points[i].u) * t;
          auto const v = feature.points[i].v
            + (feature.points[i + 1].v - feature.points[i].v) * t;
          require(laneDistance(u, v) > .04f + feature.half_width_ratio,
                  "the inter-camp clear route must stay clear of every lane");
        }
    }
    require(clear_routes == 4, "every jungle quadrant needs one clear route");
  }
  auto const objective_centers = std::array{
    std::pair{.35f, .31f}, std::pair{.65f, .69f}};
  auto const objective_offsets = std::array{
    std::pair{0.0f, 0.0f}, std::pair{-.055f, 0.0f},
    std::pair{-.0275f, -.0473f}, std::pair{.0275f, -.0473f},
    std::pair{.055f, 0.0f}, std::pair{.0275f, .0473f},
    std::pair{-.0275f, .0473f}};
  for (auto const& objective : objective_centers)
    for (auto const& offset : objective_offsets)
    {
      auto const sample = Noggit::Ai::sampleProceduralLayout(
        *terrain.layout, objective.first + offset.first,
        objective.second + offset.second,
        0.0f, 0.0f, 1600.0f, 1600.0f);
      require(std::abs(sample.height - (lane_height - 1.0f)) <= .75f
                && sample.quantized_weights[2] >= 240,
              "objective pits must keep their full flat, muddy footprint");
    }
  // Drake and nashor pits open onto the river only: every rim sample away
  // from the river bed must be sealed by the jungle wall, while the river
  // side keeps an open mouth.
  {
    std::vector<Noggit::Ai::ProceduralLayoutPoint> river_line;
    for (auto const& feature : terrain.layout->features)
      if (feature.name == "river_bed") river_line = feature.points;
    require(!river_line.empty(), "the river bed feature is missing");
    auto const riverDistance = [&](float u, float v)
    {
      auto best = 10.0f;
      for (std::size_t i = 1; i < river_line.size(); ++i)
      {
        auto const du = river_line[i].u - river_line[i - 1].u;
        auto const dv = river_line[i].v - river_line[i - 1].v;
        auto const length_squared = du * du + dv * dv;
        auto const t = length_squared > 0.0f
          ? std::clamp(((u - river_line[i - 1].u) * du
                        + (v - river_line[i - 1].v) * dv) / length_squared,
                       0.0f, 1.0f)
          : 0.0f;
        best = std::min(best, static_cast<float>(std::hypot(
          u - (river_line[i - 1].u + du * t),
          v - (river_line[i - 1].v + dv * t))));
      }
      return best;
    };
    for (auto const& objective : objective_centers)
    {
      auto river_mouth_samples = std::size_t{0};
      for (std::size_t i = 0; i < 16; ++i)
      {
        auto const angle = static_cast<float>(i) * 6.283185307f / 16.0f;
        auto const u = objective.first + std::cos(angle) * .085f;
        auto const v = objective.second + std::sin(angle) * .085f;
        auto const height = sampleHeight(u, v);
        if (riverDistance(u, v) > .105f && height < 32.25f)
          throw std::runtime_error("objective pits must be sealed by the jungle wall at "
            + std::to_string(u) + "," + std::to_string(v) + ": "
            + std::to_string(height));
        else if (height <= 24.0f)
          ++river_mouth_samples;
      }
      require(river_mouth_samples >= 2,
              "objective pits must keep an open mouth onto the river");
    }
  }
  require(sampleHeight(.02f, .98f) >= lane_height + 1.0f,
          "base apron must rise above the lanes");
  require(sampleHeight(.10f, .85f) >= lane_height + 2.5f,
          "base court must rise above the lanes");
  requireFlat(.02f, .98f, "base apron must stay flat without relief walls");
  requireFlat(.12f, .78f, "former defender gate relief must be flattened");
  require(sampleHeight(.26f, .3275f) < 15.0f,
          "the river bed must stay carved below the flat arena");

  require(walls.scatter->regions.size() >= 2
            && walls.scatter->regions.size() <= 16,
          "the two base perimeters must yield wall chains");
  std::set<std::string> base_chain_sides;
  for (auto const& region : walls.scatter->regions)
  {
    require(region.role == "wall", "wall chains must use the dedicated wall role");
    require(Noggit::Ai::proceduralScatterIsWallRegion(region),
            "wall chains must be detected as aligned wall chains");
    require(region.name.find("_chain") != std::string::npos
              && region.name.ends_with("_wall"),
            "collidable wall assets must be reserved for named wall chains");
    require(region.name.starts_with("team_"),
            "constructed walls must be reserved for fortified bases");
    if (region.name.starts_with("team_"))
      base_chain_sides.insert(region.name.substr(0, 9));
    require(region.density_per_tile > 0
              && region.min_height <= 5.0f && region.max_height >= 40.0f
              && region.min_spacing_ratio <= .0021f
              && region.cluster_strength == 0.0f,
            "wall chains must be dense, uninterrupted and anchored to flat ground");
  }
  require(base_chain_sides.size() == 2,
          "base wall chains are incomplete");
  auto const wallCandidates = [](Noggit::Ai::ProceduralScatter const& scatter)
  {
    std::vector<std::pair<float, float>> positions;
    for (std::size_t region_index = 0;
         region_index < scatter.regions.size(); ++region_index)
      for (std::size_t index = 0;
           index < scatter.regions[region_index].density_per_tile; ++index)
      {
        auto const candidate = Noggit::Ai::proceduralScatterCandidate(
          scatter, region_index, 0, 0, index, 0.0f, 1.0f, 0.0f, 1.0f);
        positions.emplace_back(candidate.u, candidate.v);
      }
    return positions;
  };
  auto const wall_positions = wallCandidates(*walls.scatter);
  require(std::all_of(walls.scatter->assets.begin(), walls.scatter->assets.end(),
            [](auto const& asset)
            {
              return asset.role == "wall"
                && std::abs(asset.min_scale - 1.5f) < .001f
                && std::abs(asset.max_scale - 1.7f) < .001f;
          })
          && walls.scatter->assets.size() == 2
          && std::none_of(vegetation.scatter->assets.begin(), vegetation.scatter->assets.end(),
            [](auto const& asset) { return asset.role == "wall"; })
          && std::any_of(vegetation.scatter->assets.begin(), vegetation.scatter->assets.end(),
            [](auto const& asset) { return asset.role == "rock"; }),
          "wall and vegetation assets must be isolated in separate batches");
  require(std::any_of(vegetation.scatter->regions.begin(), vegetation.scatter->regions.end(),
            [](auto const& region) { return region.role == "rock"; }),
          "decorative rocks must be scattered inside the jungles");
  std::map<std::string, std::size_t> jungle_layers;
  auto vegetation_density = std::size_t{0};
  auto canopy_density = std::size_t{0};
  for (auto const& region : vegetation.scatter->regions)
  {
    ++jungle_layers[region.role];
    vegetation_density += region.density_per_tile;
    if (region.role == "canopy") canopy_density += region.density_per_tile;
  }
  require(jungle_layers["canopy"] == jungle_wall_masses
            && jungle_layers["understory"] == jungle_wall_masses
            && jungle_layers["rock"] * 2 == jungle_wall_masses
            && jungle_layers["detail"] * 2 == jungle_wall_masses,
          "every wall island needs vegetation while rocks and details alternate");
  require(canopy_density * 2 >= vegetation_density,
          "the jungle scatter budget must primarily build a visible tree canopy");
  require(walls.scatter->exclusions.size() <= 96
            && vegetation.scatter->exclusions.size() == 44,
          "base chains carry their own openings; only repair openings and the "
          "vegetation exclusions remain");
  // Openings and continuity are structural now: probe candidate positions.
  auto const nearestWallDistance = [&](float u, float v)
  {
    auto best = 10.0f;
    for (auto const& [candidate_u, candidate_v] : wall_positions)
      best = std::min(best, static_cast<float>(
        std::hypot(candidate_u - u, candidate_v - v)));
    return best;
  };
  require(nearestWallDistance(.075f, .695f) >= .018f,
          "the top lane must keep a public entrance through the base wall");
  require(nearestWallDistance(.015f, .85f) <= .02f,
          "the base rear wall chain must stay continuous");
  require(nearestWallDistance(.27f, .84f) <= .02f,
          "the jungle path breach into the base must be sealed by the wall chain");
  require(Noggit::Ai::proceduralScatterExcluded(
            *vegetation.scatter, .50f, .50f, 1066.0f, 1066.0f),
          "middle lane and river crossing must be protected from decoration");
  require(!Noggit::Ai::proceduralScatterExcluded(
            *vegetation.scatter, .28f, .24f, 1066.0f, 1066.0f),
          "jungle interior should remain available for decoration");
  require(Noggit::Ai::proceduralScatterExcluded(
            *vegetation.scatter, .165f, .5925f, 1066.0f, 1066.0f),
          "jungle routes must remain open through the vegetation bands");
  require(Noggit::Ai::proceduralScatterExcluded(
            *vegetation.scatter, .48f, .25f, 1066.0f, 1066.0f),
          "camp clearings must remain free of decoration");
  for (auto const* scatter : {&*walls.scatter})
    for (std::size_t region_index = 0;
         region_index < scatter->regions.size(); ++region_index)
    {
      auto const& region = scatter->regions[region_index];
      auto viable = std::size_t{0};
      for (std::size_t index = 0; index < region.density_per_tile; ++index)
      {
        auto const candidate = Noggit::Ai::proceduralScatterCandidate(
          *scatter, region_index, 0, 0, index, 0.0f, 1.0f, 0.0f, 1.0f);
        auto const is_clear = !Noggit::Ai::proceduralScatterExcluded(
          *scatter, candidate.u, candidate.v, 1600.0f, 1600.0f);
        auto const height = sampleHeight(candidate.u, candidate.v);
        if (candidate.active && is_clear
            && height >= region.min_height && height <= region.max_height)
          ++viable;
      }
      // Chains bake their openings in, so nearly every candidate must place.
      if (viable * 4 < region.density_per_tile * 3)
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
  for (auto const* scatter : {&*walls.scatter, &*vegetation.scatter})
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
  auto const sparse_blueprint = Noggit::Ai::compileMobaArenaBlueprint(sparse_vegetation, 4);
  auto const sparse_walls = Noggit::Ai::parseProceduralScatter(
    sparse_blueprint.at("next_calls").at(3).at("arguments"));
  require(sparse_walls.scatter
            && sparse_walls.scatter->regions.size() == walls.scatter->regions.size()
            && std::equal(sparse_walls.scatter->regions.begin(),
                          sparse_walls.scatter->regions.end(),
                          walls.scatter->regions.begin(),
                          [](auto const& sparse, auto const& normal)
                          { return sparse.density_per_tile == normal.density_per_tile; }),
          "vegetation density must not weaken gameplay wall chains");

  std::set<std::string> prop_names;
  std::size_t lamp_props = 0;
  std::size_t light_props = 0;
  for (auto const& prop : props.props->props)
  {
    require(prop_names.insert(prop.name).second, "prop names must be unique");
    require(prop.u > 0.0f && prop.u < 1.0f && prop.v > 0.0f && prop.v < 1.0f,
            "props must stay inside the map footprint");
    if (prop.name.starts_with("lamp_") && !prop.name.starts_with("lamp_glow_"))
      ++lamp_props;
    if (prop.path.find("noggit_light") != std::string::npos) ++light_props;
  }
  require(props.props->props.size() <= 256, "the prop budget was exceeded");
  require(prop_names.contains("team_left_landmark")
            && prop_names.contains("team_right_landmark")
            && prop_names.contains("objective_north_landmark")
            && prop_names.contains("objective_south_landmark"),
          "base and objective landmarks are missing");
  require(lamp_props == first.at("topology").at("lane_lamps").get<std::size_t>()
            && lamp_props >= 30,
          "lane lamp chains are incomplete");
  require(light_props == first.at("topology").at("dynamic_lights").get<std::size_t>(),
          "every ambience light must come from the Patch-E light pack");

  auto single_wall = specification();
  single_wall["assets"].erase(single_wall["assets"].begin() + 6);
  static_cast<void>(Noggit::Ai::compileMobaArenaBlueprint(single_wall, 4));

  auto missing_walls = specification();
  for (auto& asset : missing_walls["assets"])
    if (asset.at("role") == "wall") asset["role"] = "rock";
  try
  {
    static_cast<void>(Noggit::Ai::compileMobaArenaBlueprint(missing_walls, 4));
    require(false, "a specification without wall assets was accepted");
  }
  catch (std::invalid_argument const&)
  {
  }

  auto invalid = specification();
  invalid["extra"] = true;
  try
  {
    static_cast<void>(Noggit::Ai::compileMobaArenaBlueprint(invalid, 4));
    require(false, "extra root field accepted");
  }
  catch (std::invalid_argument const&)
  {
  }
}
