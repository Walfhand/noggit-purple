// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/MobaArenaBlueprint.hpp>
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

namespace Noggit::Ai
{
  namespace
  {
    double finiteNumber(nlohmann::json const& object, char const* name,
                        double minimum, double maximum)
    {
      auto const& value = object.at(name);
      if (!value.is_number()) throw std::invalid_argument(std::string{name} + " doit être numérique.");
      auto const number = value.get<double>();
      if (!std::isfinite(number) || number < minimum || number > maximum)
        throw std::invalid_argument(std::string{name} + " est hors limites.");
      return number;
    }

    nlohmann::json point(double u, double v, double height)
    {
      return {{"u", u}, {"v", v}, {"height", height}};
    }

    nlohmann::json scatterPoint(double u, double v)
    {
      return {{"u", u}, {"v", v}};
    }

    nlohmann::json hexArea(double u, double v, double radius, double height)
    {
      return nlohmann::json::array({
        point(u - radius, v, height), point(u - radius * .5, v - radius * .86, height),
        point(u + radius * .5, v - radius * .86, height), point(u + radius, v, height),
        point(u + radius * .5, v + radius * .86, height),
        point(u - radius * .5, v + radius * .86, height)});
    }

    nlohmann::json feature(std::string name, std::string shape,
                           nlohmann::json points, double half_width,
                           double transition, int texture_layer, int priority,
                           std::string height_mode = "absolute",
                           double roughness = 0.0, double texture_strength = 1.0,
                           double width_variation = 0.0)
    {
      return {
        {"name", std::move(name)}, {"shape", std::move(shape)},
        {"height_mode", std::move(height_mode)}, {"points", std::move(points)},
        {"half_width_ratio", half_width}, {"transition_width_ratio", transition},
        {"texture_layer", texture_layer}, {"roughness_amplitude", roughness},
        {"texture_strength", texture_strength},
        {"width_variation_ratio", width_variation}, {"priority", priority}
      };
    }

    nlohmann::json exclusion(std::string shape, nlohmann::json points, double width)
    {
      return {{"shape", std::move(shape)}, {"points", std::move(points)},
              {"half_width_ratio", width}};
    }

    nlohmann::json insetLoop(nlohmann::json const& polygon, double factor, double height)
    {
      auto center_u = 0.0;
      auto center_v = 0.0;
      for (auto const& p : polygon)
      {
        center_u += p.at("u").get<double>();
        center_v += p.at("v").get<double>();
      }
      center_u /= polygon.size();
      center_v /= polygon.size();
      auto result = nlohmann::json::array();
      for (std::size_t i = 0; i < polygon.size(); ++i)
      {
        if (polygon.size() == 6 && i == 1) continue;
        auto const& p = polygon[i];
        result.push_back(point(
          center_u + (p.at("u").get<double>() - center_u) * factor,
          center_v + (p.at("v").get<double>() - center_v) * factor,
          height));
      }
      result.push_back(result.front());
      return result;
    }

  }

  std::optional<std::string> validateMobaArenaFootprint(
    std::vector<std::pair<std::size_t, std::size_t>> const& tiles)
  {
    if (tiles.empty()) return "La carte ouverte ne contient aucune tuile.";

    auto min_x = tiles.front().first;
    auto max_x = min_x;
    auto min_z = tiles.front().second;
    auto max_z = min_z;
    std::set<std::pair<std::size_t, std::size_t>> unique;
    for (auto const& tile : tiles)
    {
      unique.insert(tile);
      min_x = std::min(min_x, tile.first);
      max_x = std::max(max_x, tile.first);
      min_z = std::min(min_z, tile.second);
      max_z = std::max(max_z, tile.second);
    }
    auto const width = max_x - min_x + 1;
    auto const height = max_z - min_z + 1;
    if (width < 3 || height < 3)
      return "Le blueprint MOBA exige une empreinte d'au moins 3x3 tuiles.";
    if (unique.size() != width * height)
      return "Le blueprint MOBA exige un rectangle plein sans tuile manquante.";
    auto const ratio = static_cast<double>(width) / static_cast<double>(height);
    if (ratio < .85 || ratio > 1.15)
      return "Le blueprint MOBA exige une carte presque carrée (ratio entre 0,85 et 1,15).";
    return std::nullopt;
  }

  nlohmann::json compileMobaArenaBlueprint(nlohmann::json const& arguments)
  {
    static auto const fields = std::set<std::string>{
      "texture_paths", "liquid_type_id", "assets", "seed", "base_height",
      "river_depth", "lane_width_ratio", "river_width_ratio",
      "lane_curvature", "river_curvature", "jungle_roughness",
      "vegetation_density_per_tile"
    };
    if (!arguments.is_object() || arguments.size() != fields.size())
      throw std::invalid_argument("Le blueprint MOBA exige exactement ses 12 paramètres.");
    for (auto const& [name, value] : arguments.items())
    {
      static_cast<void>(value);
      if (!fields.contains(name)) throw std::invalid_argument("Argument MOBA non autorisé : " + name);
    }

    auto const& textures = arguments.at("texture_paths");
    auto const& assets = arguments.at("assets");
    auto const& seed_value = arguments.at("seed");
    auto const& liquid_value = arguments.at("liquid_type_id");
    auto const& density_value = arguments.at("vegetation_density_per_tile");
    std::set<std::string> unique_textures;
    if (textures.is_array())
      for (auto const& texture : textures)
        if (texture.is_string()) unique_textures.insert(texture.get<std::string>());
    if (!textures.is_array() || textures.size() != 4
        || !std::all_of(textures.begin(), textures.end(), [](auto const& value)
           { return value.is_string() && !value.template get<std::string>().empty(); })
        || unique_textures.size() != 4)
      throw std::invalid_argument("texture_paths doit contenir exactement quatre textures uniques.");
    if (!assets.is_array() || assets.empty() || assets.size() > 16)
      throw std::invalid_argument("assets doit contenir entre un et 16 assets.");
    std::map<std::string, std::size_t> role_counts;
    for (auto const& asset : assets)
      if (asset.is_object() && asset.contains("role") && asset.at("role").is_string())
        ++role_counts[asset.at("role").get<std::string>()];
    if (role_counts["canopy"] < 3 || role_counts["understory"] < 2
        || role_counts["rock"] < 2 || role_counts["detail"] < 1)
      throw std::invalid_argument(
        "Une arène MOBA exige au moins trois canopées, deux sous-bois, deux rochers et un détail au sol.");
    if (!seed_value.is_string() || seed_value.get_ref<std::string const&>().empty()
        || seed_value.get_ref<std::string const&>().size() > 64)
      throw std::invalid_argument("seed doit contenir entre un et 64 caractères.");
    if (!liquid_value.is_number_integer() || liquid_value.get<int>() < 1
        || liquid_value.get<int>() > 65535)
      throw std::invalid_argument("liquid_type_id est invalide.");
    if (!density_value.is_number_integer() || density_value.get<int>() < 1
        || density_value.get<int>() > 256)
      throw std::invalid_argument("vegetation_density_per_tile doit être dans [1,256].");

    auto const base = finiteNumber(arguments, "base_height", -450.0, 4950.0);
    auto const river_depth = finiteNumber(arguments, "river_depth", 2.0, 30.0);
    auto const lane_width = finiteNumber(arguments, "lane_width_ratio", 0.025, 0.055);
    auto const river_width = finiteNumber(arguments, "river_width_ratio", 0.015, 0.08);
    auto const lane_curve = finiteNumber(arguments, "lane_curvature", 0.0, 1.0);
    auto const river_curve = finiteNumber(arguments, "river_curvature", 0.0, 1.0);
    auto const roughness = finiteNumber(arguments, "jungle_roughness", 1.0, 12.0);
    auto const river_height = base - river_depth;
    auto const water_height = river_height + std::min(2.0, river_depth * 0.35);
    auto const bend = 0.035 + lane_curve * 0.055;
    auto const river_bend = river_curve * 0.055;

    auto top = nlohmann::json::array({point(.07, .88, base), point(.09, .73, base),
      point(.055, .64, base), point(.06, .28, base), point(.14, .10, base),
      point(.32, .055 - bend * .15, base), point(.70, .06 - bend * .15, base),
      point(.84, .10, base), point(.77, .07, base), point(.93, .12, base)});
    auto middle = nlohmann::json::array({point(.07, .89, base), point(.18, .82, base),
      point(.34, .66 + bend * .12, base), point(.50, .50, base),
      point(.66, .34 - bend * .12, base), point(.82, .18, base), point(.93, .11, base)});
    auto bottom = nlohmann::json::array({point(.07, .90, base), point(.23, .93, base),
      point(.34, .95 + bend * .08, base), point(.66, .94 + bend * .12, base),
      point(.86, .90, base), point(.94, .72, base), point(.94, .36, base),
      point(.91, .27, base), point(.93, .12, base)});
    auto river = nlohmann::json::array({point(.04, .14 - river_bend, river_height),
      point(.26, .30 + river_bend, river_height), point(.50, .50, river_height),
      point(.74, .70 - river_bend, river_height), point(.96, .86 + river_bend, river_height)});

    auto const jungle_polygons = std::array<nlohmann::json, 4>{
      nlohmann::json::array({point(.12,.16,base + 8), point(.24,.075,base + 8),
        point(.76,.075,base + 8), point(.88,.16,base + 8), point(.56,.45,base + 8),
        point(.44,.45,base + 8)}),
      nlohmann::json::array({point(.56,.45,base + 8), point(.88,.16,base + 8),
        point(.94,.28,base + 8), point(.94,.72,base + 8), point(.86,.84,base + 8),
        point(.56,.55,base + 8)}),
      nlohmann::json::array({point(.56,.55,base + 8), point(.88,.84,base + 8),
        point(.76,.925,base + 8), point(.24,.925,base + 8), point(.12,.84,base + 8),
        point(.44,.55,base + 8)}),
      nlohmann::json::array({point(.44,.55,base + 8), point(.12,.84,base + 8),
        point(.06,.72,base + 8), point(.06,.28,base + 8), point(.14,.16,base + 8),
        point(.44,.45,base + 8)})
    };
    struct Camp { char const* name; double u; double v; };
    auto const camps = std::array{
      Camp{"north_wolves", .31, .20}, Camp{"north_blue", .50, .22}, Camp{"north_gromp", .69, .20},
      Camp{"east_raptors", .80, .32}, Camp{"east_red", .78, .50}, Camp{"east_krugs", .80, .68},
      Camp{"south_wolves", .69, .80}, Camp{"south_blue", .50, .78}, Camp{"south_gromp", .32, .80},
      Camp{"west_raptors", .20, .68}, Camp{"west_red", .22, .50}, Camp{"west_krugs", .20, .32}
    };
    auto const objective_north = hexArea(.34, .27, .055, base - 1);
    auto const objective_south = hexArea(.66, .73, .055, base - 1);
    auto const jungle_paths = std::array<nlohmann::json, 8>{
      nlohmann::json::array({point(.13,.13,base), point(.50,.22,base), point(.84,.13,base)}),
      nlohmann::json::array({point(.34,.27,base), point(.50,.22,base), point(.56,.45,base)}),
      nlohmann::json::array({point(.87,.13,base), point(.78,.50,base), point(.87,.84,base)}),
      nlohmann::json::array({point(.66,.34,base), point(.78,.50,base), point(.66,.66,base)}),
      nlohmann::json::array({point(.87,.87,base), point(.50,.78,base), point(.13,.87,base)}),
      nlohmann::json::array({point(.66,.73,base), point(.50,.78,base), point(.44,.55,base)}),
      nlohmann::json::array({point(.13,.87,base), point(.22,.50,base), point(.13,.16,base)}),
      nlohmann::json::array({point(.34,.66,base), point(.22,.50,base), point(.34,.34,base)})
    };
    auto const left_base_outer = nlohmann::json::array({point(.01,.70,base + 30),
      point(.10,.68,base + 30), point(.24,.76,base + 30), point(.30,.90,base + 30),
      point(.20,.99,base + 30), point(.01,.99,base + 30)});
    auto const left_base_inner = nlohmann::json::array({point(.025,.78,base),
      point(.09,.75,base), point(.18,.81,base), point(.21,.90,base),
      point(.15,.96,base), point(.025,.96,base)});
    auto mirror = [](nlohmann::json const& points)
    {
      auto result = nlohmann::json::array();
      for (auto const& p : points)
        result.push_back(point(1.0 - p.at("u").get<double>(),
                               1.0 - p.at("v").get<double>(),
                               p.at("height").get<double>()));
      std::reverse(result.begin(), result.end());
      return result;
    };
    auto const right_base_outer = mirror(left_base_outer);
    auto const right_base_inner = mirror(left_base_inner);

    auto terrain_features = nlohmann::json::array();
    for (std::size_t i = 0; i < jungle_polygons.size(); ++i)
      terrain_features.push_back(feature("jungle_" + std::to_string(i + 1) + "_mass", "area",
        jungle_polygons[i], .005, .035, 0, 20, "absolute", roughness, .55));
    for (std::size_t i = 0; i < jungle_polygons.size(); ++i)
      terrain_features.push_back(feature("jungle_" + std::to_string(i + 1) + "_ridge", "corridor",
        insetLoop(jungle_polygons[i], .78, base + 30), .012, .012, 3, 45,
        "absolute", 1.2, .9, .12));
    for (std::size_t i = 0; i < jungle_paths.size(); ++i)
      terrain_features.push_back(feature("jungle_" + std::to_string(i / 2 + 1)
        + (i % 2 == 0 ? "_main_path" : "_branch_path"), "corridor",
        jungle_paths[i], .022, .018, 0, 55, "absolute", .6, .65, .12));
    terrain_features.push_back(feature("river_bed", "corridor", river,
      river_width, .025, 2, 60, "absolute", 0, 1, .18));
    terrain_features.push_back(feature("objective_north", "area",
      objective_north, .005, .025, 2, 65));
    terrain_features.push_back(feature("objective_south", "area",
      objective_south, .005, .025, 2, 65));
    terrain_features.push_back(feature("team_left_outer_wall", "area", left_base_outer,
      .005, .018, 3, 40, "absolute", .8, .9));
    terrain_features.push_back(feature("team_right_outer_wall", "area", right_base_outer,
      .005, .018, 3, 40, "absolute", .8, .9));
    terrain_features.push_back(feature("team_left_inner_court", "area", left_base_inner,
      .005, .014, 1, 70));
    terrain_features.push_back(feature("team_right_inner_court", "area", right_base_inner,
      .005, .014, 1, 70));
    auto addGate = [&](std::string name, double u0, double v0, double u1, double v1)
    {
      terrain_features.push_back(feature(std::move(name), "corridor",
        nlohmann::json::array({point(u0, v0, base + 30), point(u1, v1, base + 30)}),
        .014, .008, 3, 95, "absolute", 0, 1));
    };
    addGate("team_left_upper_defender_gate", .105, .765, .135, .790);
    addGate("team_left_lower_defender_gate", .200, .865, .220, .885);
    addGate("team_right_upper_defender_gate", .895, .235, .865, .210);
    addGate("team_right_lower_defender_gate", .800, .135, .780, .115);
    terrain_features.push_back(feature("top_lane", "corridor", top,
      lane_width, .03, 1, 90, "absolute", 0, 1, .18));
    terrain_features.push_back(feature("middle_lane", "corridor", middle,
      lane_width, .03, 1, 90, "absolute", 0, 1, .12));
    terrain_features.push_back(feature("bottom_lane", "corridor", bottom,
      lane_width, .03, 1, 90, "absolute", 0, 1, .18));

    struct RoleScatter
    {
      char const* role;
      double density_factor;
      double spacing;
      double clusters;
      double strength;
      double min_slope;
      double max_slope;
    };
    auto const role_scatter = std::array{
      RoleScatter{"canopy", 1.0, .018, 3.0, .68, 0, 34},
      RoleScatter{"understory", 3.0, .006, 5.0, .72, 0, 42},
      RoleScatter{"rock", 1.5, .014, 2.5, .62, 18, 70},
      RoleScatter{"detail", 5.0, .003, 7.0, .75, 0, 30}
    };
    auto total_factor = 0.0;
    for (auto const& role : role_scatter)
      if (role_counts[role.role] > 0) total_factor += role.density_factor;
    auto const density_scale = std::min(1.0,
      512.0 / (density_value.get<double>() * total_factor));

    nlohmann::json scatter_regions = nlohmann::json::array();
    for (std::size_t i = 0; i < jungle_polygons.size(); ++i)
    {
      auto points = nlohmann::json::array();
      for (auto const& p : jungle_polygons[i]) points.push_back(scatterPoint(p.at("u"), p.at("v")));
      for (auto const& role : role_scatter)
      {
        if (role_counts[role.role] == 0) continue;
        auto const density = std::max(1, static_cast<int>(std::lround(
          density_value.get<double>() * role.density_factor * density_scale)));
        scatter_regions.push_back({
          {"name", "jungle_" + std::to_string(i + 1) + "_" + role.role},
          {"role", role.role}, {"points", points}, {"density_per_tile", density},
          {"min_spacing_ratio", role.spacing}, {"min_height", base - 40},
          {"max_height", base + 60}, {"min_slope_degrees", role.min_slope},
          {"max_slope_degrees", role.max_slope}, {"cluster_scale", role.clusters},
          {"cluster_strength", role.strength}});
      }
    }
    auto withoutHeight = [](nlohmann::json const& points)
    {
      auto result = nlohmann::json::array();
      for (auto const& p : points) result.push_back(scatterPoint(p.at("u"), p.at("v")));
      return result;
    };
    auto exclusions = nlohmann::json::array({
      exclusion("corridor", withoutHeight(top), lane_width + .025),
      exclusion("corridor", withoutHeight(middle), lane_width + .025),
      exclusion("corridor", withoutHeight(bottom), lane_width + .025),
      exclusion("corridor", withoutHeight(river), river_width + .02),
      exclusion("area", withoutHeight(left_base_outer), .025),
      exclusion("area", withoutHeight(right_base_outer), .025),
      exclusion("area", withoutHeight(objective_north), .025),
      exclusion("area", withoutHeight(objective_south), .025)
    });
    for (auto const& camp : camps)
      exclusions.push_back(exclusion(
        "area", withoutHeight(hexArea(camp.u, camp.v, .035, base)), .012));

    nlohmann::json terrain = {{"texture_paths", textures}, {"steep_texture_layer", 3},
      {"slope_start_degrees", 18}, {"slope_full_degrees", 34},
      {"edge_noise_ratio", .006}, {"max_slope_degrees", 60},
      {"smoothing_strength", .25}, {"features", std::move(terrain_features)}};
    nlohmann::json liquid = {{"replace_existing", true}, {"edge_noise_ratio", .004},
      {"features", nlohmann::json::array({{{"name", "central_river"}, {"shape", "corridor"},
        {"points", withoutHeight(river)}, {"half_width_ratio", river_width * .78},
        {"transition_width_ratio", .012}, {"liquid_type_id", liquid_value},
        {"depth", .65}, {"priority", 50}}})}};
    for (auto& p : liquid["features"][0]["points"]) p["height"] = water_height;
    nlohmann::json scatter = {{"seed", seed_value}, {"assets", assets},
      {"regions", std::move(scatter_regions)}, {"exclusions", std::move(exclusions)}};
    if (auto const parsed = parseProceduralScatter(scatter); !parsed.scatter)
      throw std::invalid_argument(parsed.error);

    return {{"ok", true}, {"operation", "create_moba_arena_blueprint"},
      {"topology", {{"lanes", 3}, {"bases", 2}, {"river", 1},
                    {"objective_pits", 2}, {"jungle_regions", 4},
                    {"jungle_camps", 12}, {"jungle_masses", 4},
                    {"jungle_ridges", 4},
                    {"jungle_paths", 8}, {"fortified_bases", 2},
                    {"public_entrances_per_base", 3},
                    {"decorative_defender_gates_per_base", 2}}},
      {"next_calls", nlohmann::json::array({
        {{"name", "apply_terrain_layout_on_map"}, {"arguments", std::move(terrain)}},
        {{"name", "apply_liquid_layout_on_map"}, {"arguments", std::move(liquid)}},
        {{"name", "scatter_assets_on_map"}, {"arguments", std::move(scatter)}}
      })}};
  }
}
