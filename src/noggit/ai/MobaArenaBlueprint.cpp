// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/MobaArenaBlueprint.hpp>
#include <noggit/ai/ProceduralProps.hpp>
#include <noggit/ai/ProceduralScatter.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <optional>
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

    nlohmann::json withoutHeight(nlohmann::json const& points)
    {
      auto result = nlohmann::json::array();
      for (auto const& p : points)
        result.push_back(scatterPoint(p.at("u"), p.at("v")));
      return result;
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

    // Shrinks a polygon by a fixed inward distance (mitered edge offset).
    nlohmann::json offsetLoopInward(
      nlohmann::json const& polygon, double distance, double height)
    {
      auto const size = polygon.size();
      auto coordinate = [&](std::size_t index, char const* axis)
      {
        return polygon[index % size].at(axis).get<double>();
      };
      auto buildLoop = [&](double sign)
      {
        auto result = nlohmann::json::array();
        for (std::size_t i = 0; i < size; ++i)
        {
          auto normal = [&](std::size_t from, std::size_t to)
          {
            auto const du = coordinate(to, "u") - coordinate(from, "u");
            auto const dv = coordinate(to, "v") - coordinate(from, "v");
            auto const length = std::hypot(du, dv);
            return std::pair{sign * dv / length, -sign * du / length};
          };
          auto const [nx1, ny1] = normal(i + size - 1, i);
          auto const [nx2, ny2] = normal(i, i + 1);
          auto mx = nx1 + nx2;
          auto my = ny1 + ny2;
          auto const norm = std::hypot(mx, my);
          auto const cosine = std::max(.5, norm * .5);
          result.push_back(point(
            coordinate(i, "u") + mx / norm * distance / cosine,
            coordinate(i, "v") + my / norm * distance / cosine,
            height));
        }
        return result;
      };
      std::vector<ProceduralLayoutPoint> outline;
      for (std::size_t i = 0; i < size; ++i)
        outline.push_back({static_cast<float>(coordinate(i, "u")),
                           static_cast<float>(coordinate(i, "v")), 0.0f});
      auto loop = buildLoop(1.0);
      if (!proceduralScatterContains(
            outline, static_cast<float>(loop[0].at("u").get<double>()),
            static_cast<float>(loop[0].at("v").get<double>())))
        loop = buildLoop(-1.0);
      return loop;
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
    if (width < 2 || height < 2)
      return "Le blueprint MOBA exige une empreinte d'au moins 2x2 tuiles.";
    if (width > 4 || height > 4)
      return "Le blueprint MOBA accepte une empreinte maximale de 4x4 tuiles.";
    if (unique.size() != width * height)
      return "Le blueprint MOBA exige un rectangle plein sans tuile manquante.";
    auto const ratio = static_cast<double>(width) / static_cast<double>(height);
    if (ratio < .85 || ratio > 1.15)
      return "Le blueprint MOBA exige une carte presque carrée (ratio entre 0,85 et 1,15).";
    return std::nullopt;
  }

  nlohmann::json compileMobaArenaBlueprint(
    nlohmann::json const& arguments, std::size_t footprint_side_tiles)
  {
    if (footprint_side_tiles < 2 || footprint_side_tiles > 4)
      throw std::invalid_argument(
        "Le blueprint MOBA exige un côté de carte entre 2 et 4 tuiles.");
    static auto const fields = std::set<std::string>{
      "texture_paths", "liquid_type_id", "assets", "seed", "base_height",
      "river_depth", "lane_width_ratio", "river_width_ratio",
      "lane_curvature", "river_curvature", "jungle_roughness",
      "vegetation_density_per_tile", "ground_effect_texture_id", "prop_paths",
      "skybox_path", "skybox_flags"
    };
    if (!arguments.is_object() || arguments.size() != fields.size())
      throw std::invalid_argument("Le blueprint MOBA exige exactement ses 16 paramètres.");
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
        || role_counts["wall"] < 1 || role_counts["rock"] < 2
        || role_counts["detail"] < 1)
      throw std::invalid_argument(
        "Une arène MOBA exige au moins trois canopées, deux sous-bois, un mur de base collidable, deux rochers et un détail au sol.");
    if (!seed_value.is_string() || seed_value.get_ref<std::string const&>().empty()
        || seed_value.get_ref<std::string const&>().size() > 64)
      throw std::invalid_argument("seed doit contenir entre un et 64 caractères.");
    if (!liquid_value.is_number_integer() || liquid_value.get<int>() < 1
        || liquid_value.get<int>() > 65535)
      throw std::invalid_argument("liquid_type_id est invalide.");
    if (!density_value.is_number_integer() || density_value.get<int>() < 1
        || density_value.get<int>() > 256)
      throw std::invalid_argument("vegetation_density_per_tile doit être dans [1,256].");
    auto const& ground_effect_value = arguments.at("ground_effect_texture_id");
    if (!ground_effect_value.is_number_integer()
        || ground_effect_value.get<int>() < 0)
      throw std::invalid_argument("ground_effect_texture_id doit être positif ou nul pour auto.");
    auto const& skybox_path = arguments.at("skybox_path");
    auto const& skybox_flags = arguments.at("skybox_flags");
    if (!skybox_path.is_string() || skybox_path.get_ref<std::string const&>().empty()
        || skybox_path.get_ref<std::string const&>().size() > 260)
      throw std::invalid_argument("skybox_path doit être un chemin M2 non vide.");
    if (!skybox_flags.is_number_integer()
        || skybox_flags.get<int>() < 0 || skybox_flags.get<int>() > 3)
      throw std::invalid_argument("skybox_flags doit être compris entre 0 et 3.");
    static auto const prop_roles = std::set<std::string>{
      "base_landmark", "objective_landmark", "camp_marker", "lane_lamp",
      "team_left_light", "team_right_light", "river_light", "flame_light",
      "lamp_light"
    };
    auto const& prop_paths = arguments.at("prop_paths");
    if (!prop_paths.is_object() || prop_paths.size() != prop_roles.size())
      throw std::invalid_argument(
        "prop_paths exige exactement base_landmark, objective_landmark, camp_marker, "
        "lane_lamp, team_left_light, team_right_light, river_light, flame_light et lamp_light.");
    for (auto const& [name, value] : prop_paths.items())
    {
      if (!prop_roles.contains(name))
        throw std::invalid_argument("Rôle de prop non autorisé : " + name);
      if (!value.is_string() || value.get_ref<std::string const&>().empty()
          || value.get_ref<std::string const&>().size() > 260)
        throw std::invalid_argument("Chemin de prop invalide pour : " + name);
    }
    auto const propPath = [&](char const* role)
    {
      return prop_paths.at(role).get<std::string>();
    };

    auto const base = finiteNumber(arguments, "base_height", -450.0, 4950.0);
    auto const river_depth = finiteNumber(arguments, "river_depth", 2.0, 30.0);
    auto const lane_width = finiteNumber(arguments, "lane_width_ratio", 0.012, 0.055);
    auto const river_width = finiteNumber(arguments, "river_width_ratio", 0.015, 0.08);
    auto const lane_curve = finiteNumber(arguments, "lane_curvature", 0.0, 1.0);
    auto const river_curve = finiteNumber(arguments, "river_curvature", 0.0, 1.0);
    auto const roughness = finiteNumber(arguments, "jungle_roughness", 1.0, 12.0);
    auto const river_height = base - river_depth;
    // Keep the water column under WoW's swim threshold so the river is
    // always wadeable on foot, however deep the bed is carved.
    auto const water_height = river_height + std::min(1.0, river_depth * 0.25);
    auto const bend = 0.035 + lane_curve * 0.055;
    auto const river_bend = river_curve * 0.055;
    // Elevation tiers: river < lanes (base) < jungle paths < jungle floor,
    // with the base courts raised so lanes ramp up into them.
    auto const jungle_rise = 6.0;
    auto const relief_rise = 20.0;
    auto const path_rise = 3.0;
    auto const apron_rise = 2.0;
    auto const court_rise = 4.0;
    auto const jungle_top = base + jungle_rise;
    auto const relief_top = base + relief_rise;
    auto const path_top = base + path_rise;

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
      nlohmann::json::array({point(.12,.16,jungle_top), point(.24,.075,jungle_top),
        point(.76,.075,jungle_top), point(.88,.16,jungle_top), point(.56,.45,jungle_top),
        point(.44,.45,jungle_top)}),
      nlohmann::json::array({point(.56,.45,jungle_top), point(.88,.16,jungle_top),
        point(.94,.28,jungle_top), point(.94,.72,jungle_top), point(.86,.84,jungle_top),
        point(.56,.55,jungle_top)}),
      nlohmann::json::array({point(.56,.55,jungle_top), point(.88,.84,jungle_top),
        point(.76,.925,jungle_top), point(.24,.925,jungle_top), point(.12,.84,jungle_top),
        point(.44,.55,jungle_top)}),
      nlohmann::json::array({point(.44,.55,jungle_top), point(.12,.84,jungle_top),
        point(.06,.72,jungle_top), point(.06,.28,jungle_top), point(.14,.16,jungle_top),
        point(.44,.45,jungle_top)})
    };
    auto relief_polygons = jungle_polygons;
    for (auto& polygon : relief_polygons)
      polygon = offsetLoopInward(polygon, .055, relief_top);
    struct Camp
    {
      char const* name;
      double u;
      double v;
      double access_u;
      double access_v;
    };
    auto const camps = std::array{
      Camp{"north_wolves", .40, .14, 0, 1},
      Camp{"north_blue", .50, .19, 0, 1},
      Camp{"north_gromp", .64, .22, 0, 1},
      Camp{"east_raptors", .83, .43, 1, 0},
      Camp{"east_red", .68, .50, 1, 0},
      Camp{"east_krugs", .82, .56, 1, 0},
      Camp{"south_wolves", .60, .86, 0, 1},
      Camp{"south_blue", .50, .81, 0, 1},
      Camp{"south_gromp", .36, .78, 0, 1},
      Camp{"west_raptors", .17, .57, 1, 0},
      Camp{"west_red", .32, .50, 1, 0},
      Camp{"west_krugs", .18, .44, 1, 0}
    };
    auto campAccess = [&](Camp const& camp, double height)
    {
      return nlohmann::json::array({
        point(camp.u - camp.access_u * .085,
              camp.v - camp.access_v * .085, height),
        point(camp.u + camp.access_u * .085,
              camp.v + camp.access_v * .085, height)});
    };
    auto const objective_north = hexArea(.34, .27, .055, base - 1);
    auto const objective_south = hexArea(.66, .73, .055, base - 1);
    auto const jungle_paths = std::array<nlohmann::json, 8>{
      nlohmann::json::array({point(.13,.13,path_top), point(.50,.22,path_top), point(.84,.13,path_top)}),
      nlohmann::json::array({point(.34,.27,path_top), point(.50,.22,path_top), point(.56,.45,path_top)}),
      nlohmann::json::array({point(.87,.13,path_top), point(.78,.50,path_top), point(.87,.84,path_top)}),
      nlohmann::json::array({point(.66,.34,path_top), point(.78,.50,path_top), point(.66,.66,path_top)}),
      nlohmann::json::array({point(.87,.87,path_top), point(.50,.78,path_top), point(.13,.87,path_top)}),
      nlohmann::json::array({point(.66,.73,path_top), point(.50,.78,path_top), point(.44,.55,path_top)}),
      nlohmann::json::array({point(.13,.87,path_top), point(.22,.50,path_top), point(.13,.16,path_top)}),
      nlohmann::json::array({point(.34,.66,path_top), point(.22,.50,path_top), point(.34,.34,path_top)})
    };
    auto const left_base_outer = nlohmann::json::array({point(.01,.70,base + apron_rise),
      point(.10,.68,base + apron_rise), point(.24,.76,base + apron_rise),
      point(.30,.90,base + apron_rise), point(.20,.99,base + apron_rise),
      point(.01,.99,base + apron_rise)});
    auto const left_base_inner = nlohmann::json::array({point(.025,.78,base + court_rise),
      point(.09,.75,base + court_rise), point(.18,.81,base + court_rise),
      point(.21,.90,base + court_rise), point(.15,.96,base + court_rise),
      point(.025,.96,base + court_rise)});
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

    auto toLayoutPoints = [](nlohmann::json const& polygon)
    {
      std::vector<ProceduralLayoutPoint> points;
      for (auto const& p : polygon)
        points.push_back({static_cast<float>(p.at("u").get<double>()),
                          static_cast<float>(p.at("v").get<double>()), 0.0f});
      return points;
    };
    auto const left_base_points = toLayoutPoints(left_base_outer);
    auto const right_base_points = toLayoutPoints(right_base_outer);
    auto const left_court_points = toLayoutPoints(left_base_inner);
    auto const right_court_points = toLayoutPoints(right_base_inner);
    auto const insideBase = [&](std::vector<ProceduralLayoutPoint> const& polygon,
                                nlohmann::json const& p)
    {
      return proceduralScatterContains(polygon,
        static_cast<float>(p.at("u").get<double>()),
        static_cast<float>(p.at("v").get<double>()));
    };
    // The corridor sampler interpolates heights along the path, so raising
    // only the points that sit on a base plateau turns the lane ends into
    // ramps instead of trenches carved through the raised courts.
    auto const raiseLaneEnds = [&](nlohmann::json& lane)
    {
      for (auto& p : lane)
      {
        if (insideBase(left_court_points, p) || insideBase(right_court_points, p))
          p["height"] = base + court_rise;
        else if (insideBase(left_base_points, p) || insideBase(right_base_points, p))
          p["height"] = base + apron_rise;
      }
    };
    raiseLaneEnds(top);
    raiseLaneEnds(middle);
    raiseLaneEnds(bottom);

    auto const segmentDistance = [](nlohmann::json const& points, bool closed,
                                    double u, double v)
    {
      auto best = 10.0;
      auto const count = points.size();
      if (count < 2) return best;
      auto const limit = closed ? count : count - 1;
      for (std::size_t i = 0; i < limit; ++i)
      {
        auto const& a = points[i];
        auto const& b = points[(i + 1) % count];
        auto const au = a.at("u").get<double>();
        auto const av = a.at("v").get<double>();
        auto const du = b.at("u").get<double>() - au;
        auto const dv = b.at("v").get<double>() - av;
        auto const length_squared = du * du + dv * dv;
        auto const t = length_squared > 0.0
          ? std::clamp(((u - au) * du + (v - av) * dv) / length_squared, 0.0, 1.0)
          : 0.0;
        best = std::min(best, std::hypot(u - (au + du * t), v - (av + dv * t)));
      }
      return best;
    };

    auto terrain_features = nlohmann::json::array();
    terrain_features.push_back(feature("arena_ground", "area",
      nlohmann::json::array({point(0, 0, base), point(1, 0, base),
        point(1, 1, base), point(0, 1, base)}),
      .005, .02, 0, 5, "absolute", 2.0, .85));
    for (std::size_t i = 0; i < jungle_polygons.size(); ++i)
      terrain_features.push_back(feature("jungle_" + std::to_string(i + 1) + "_mass", "area",
        jungle_polygons[i], .005, .025, 0, 20, "absolute", roughness, .65));
    for (std::size_t i = 0; i < relief_polygons.size(); ++i)
      terrain_features.push_back(feature("jungle_" + std::to_string(i + 1) + "_relief", "area",
        relief_polygons[i], .008, .014, 0, 35, "absolute",
        roughness * .4, .72));
    // A short capsule merges into the continuous jungle relief; the camp
    // floor cuts an enclave instead of a repeated circular crater.
    for (auto const& camp : camps)
    {
      auto const tangent_u = -camp.access_v;
      auto const tangent_v = camp.access_u;
      auto const camp_relief = nlohmann::json::array({
        point(camp.u - tangent_u * .028, camp.v - tangent_v * .028, relief_top),
        point(camp.u + tangent_u * .028, camp.v + tangent_v * .028, relief_top)});
      terrain_features.push_back(feature(std::string{camp.name} + "_camp_relief",
        "corridor", camp_relief, .072, .012, 0, 59,
        "absolute", roughness * .16, .72));
    }
    for (std::size_t i = 0; i < jungle_paths.size(); ++i)
      terrain_features.push_back(feature("jungle_" + std::to_string(i / 2 + 1)
        + (i % 2 == 0 ? "_main_path" : "_branch_path"), "corridor",
        jungle_paths[i], .022, .018, 0, 55, "absolute", .6, .65, .12));
    // Two narrow opposite cuts connect each off-path camp while most of the
    // capsule remains raised as a continuous enclave.
    for (auto const& camp : camps)
    {
      auto access = campAccess(camp, jungle_top);
      terrain_features.push_back(feature(std::string{camp.name} + "_camp_access",
        "corridor", access, .011, .006, 0, 67, "absolute", .3, .5));
    }
    // Narrow deep channel with long submerged banks: the shoreline lands on
    // the gentle underwater slope, so edge water is shallow and fades out
    // instead of meeting the bank at full depth.
    terrain_features.push_back(feature("river_bed", "corridor", river,
      river_width * .5, std::min(.25, river_width * 1.2), 2, 60,
      "absolute", 0, 1, .18));
    terrain_features.push_back(feature("objective_north", "area",
      objective_north, .005, .025, 2, 65));
    terrain_features.push_back(feature("objective_south", "area",
      objective_south, .005, .025, 2, 65));
    // Flat combat spaces win over nearby river/objective transitions while
    // leaving the surrounding relief visible at every camp.
    for (auto const& camp : camps)
      terrain_features.push_back(feature(std::string{camp.name} + "_camp_floor",
        "area", hexArea(camp.u, camp.v, .036, jungle_top), .005, .01, 1, 68,
        "absolute", .5, .5));
    terrain_features.push_back(feature("team_left_base_apron", "area", left_base_outer,
      .005, .018, 3, 40, "absolute", .4, .9));
    terrain_features.push_back(feature("team_right_base_apron", "area", right_base_outer,
      .005, .018, 3, 40, "absolute", .4, .9));
    terrain_features.push_back(feature("team_left_inner_court", "area", left_base_inner,
      .005, .014, 1, 70));
    terrain_features.push_back(feature("team_right_inner_court", "area", right_base_inner,
      .005, .014, 1, 70));
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
      RoleScatter{"understory", 3.0, .0045, 5.0, .38, 0, 42},
      RoleScatter{"rock", 1.5, .008, 4.0, .6, 18, 60},
      RoleScatter{"detail", 5.0, .003, 7.0, .75, 0, 30}
    };
    auto total_factor = 0.0;
    for (auto const& role : role_scatter)
      if (role_counts[role.role] > 0) total_factor += role.density_factor;
    auto const density_scale = std::min(1.0,
      512.0 / (density_value.get<double>() * total_factor));

    nlohmann::json wall_assets = nlohmann::json::array();
    nlohmann::json vegetation_assets = nlohmann::json::array();
    for (auto const& asset : assets)
    {
      if (asset.is_object() && asset.contains("role") && asset.at("role") == "wall")
        wall_assets.push_back(asset);
      else
        vegetation_assets.push_back(asset);
    }

    nlohmann::json wall_regions = nlohmann::json::array();
    nlohmann::json vegetation_regions = nlohmann::json::array();
    for (std::size_t i = 0; i < jungle_polygons.size(); ++i)
    {
      for (auto const& role : role_scatter)
      {
        if (role_counts[role.role] == 0) continue;
        auto const points = withoutHeight(
          std::string{role.role} == "canopy"
            ? relief_polygons[i] : jungle_polygons[i]);
        auto const density = std::max(1, static_cast<int>(std::lround(
          density_value.get<double>() * role.density_factor * density_scale)));
        vegetation_regions.push_back({
          {"name", "jungle_" + std::to_string(i + 1) + "_" + role.role},
          {"role", role.role}, {"points", points}, {"density_per_tile", density},
          {"min_spacing_ratio", role.spacing}, {"min_height", base - 40},
          {"max_height", base + 60}, {"min_slope_degrees", role.min_slope},
          {"max_slope_degrees", role.max_slope}, {"cluster_scale", role.clusters},
          {"cluster_strength", role.strength}});
      }
    }
    auto const base_loops = std::array{
      std::pair{std::string{"team_left_base"}, withoutHeight(left_base_outer)},
      std::pair{std::string{"team_right_base"}, withoutHeight(right_base_outer)}};
    // Architecture belongs to the bases. The jungle boundaries come from
    // terrain relief and vegetation, leaving three readable lane entrances.
    auto const openAt = [&](double u, double v)
    {
      if (segmentDistance(top, false, u, v) <= lane_width + .012) return true;
      if (segmentDistance(middle, false, u, v) <= lane_width + .012) return true;
      if (segmentDistance(bottom, false, u, v) <= lane_width + .012) return true;
      return false;
    };
    struct WallChain { std::string prefix; nlohmann::json points; double length; };
    std::vector<WallChain> wall_chains;
    auto const emitChains = [&](nlohmann::json const& pts,
                                std::string const& prefix,
                                std::vector<WallChain>& target)
    {
      constexpr auto closed = true;
      auto const count = pts.size();
      if (count < 2) return;
      auto const edge_count = closed ? count : count - 1;
      std::vector<double> arc{0.0};
      for (std::size_t e = 0; e < edge_count; ++e)
      {
        auto const& a = pts[e];
        auto const& b = pts[(e + 1) % count];
        arc.push_back(arc.back() + std::hypot(
          b.at("u").get<double>() - a.at("u").get<double>(),
          b.at("v").get<double>() - a.at("v").get<double>()));
      }
      auto const total = arc.back();
      if (total <= 0.0) return;
      auto const pointAt = [&](double t)
      {
        t = std::clamp(closed ? std::fmod(t + total, total) : t, 0.0, total);
        auto e = std::size_t{0};
        while (e + 2 < arc.size() && arc[e + 1] < t) ++e;
        auto const& a = pts[e];
        auto const& b = pts[(e + 1) % count];
        auto const span = arc[e + 1] - arc[e];
        auto const f = span > 0.0 ? (t - arc[e]) / span : 0.0;
        return std::pair{
          a.at("u").get<double>()
            + (b.at("u").get<double>() - a.at("u").get<double>()) * f,
          a.at("v").get<double>()
            + (b.at("v").get<double>() - a.at("v").get<double>()) * f};
      };
      auto const samples = std::max<std::size_t>(8,
        static_cast<std::size_t>(total / .0025));
      std::vector<char> walled(samples);
      for (std::size_t s = 0; s < samples; ++s)
      {
        auto const [u, v] = pointAt((s + .5) * total / samples);
        walled[s] = !openAt(u, v);
      }
      struct Run { double begin; double end; };
      std::vector<Run> runs;
      for (std::size_t s = 0; s < samples; ++s)
      {
        if (!walled[s]) continue;
        auto const t0 = s * total / samples;
        while (s < samples && walled[s]) ++s;
        runs.push_back({t0, s * total / samples});
      }
      if (closed && runs.size() > 1 && walled.front() && walled.back())
      {
        runs.front().begin = runs.back().begin - total;
        runs.pop_back();
      }
      for (auto const& run : runs)
      {
        // Chains shorter than two props read as floating debris; drop them.
        if (run.end - run.begin < .04) continue;
        auto chain = nlohmann::json::array();
        auto const [begin_u, begin_v] = pointAt(run.begin);
        chain.push_back(scatterPoint(begin_u, begin_v));
        std::vector<std::pair<double, std::size_t>> interior;
        for (std::size_t vertex = 0; vertex < edge_count; ++vertex)
        {
          auto position = arc[vertex];
          if (closed && run.begin < 0.0 && position > total + run.begin)
            position -= total;
          if (position > run.begin + .004 && position < run.end - .004)
            interior.emplace_back(position, vertex);
        }
        std::sort(interior.begin(), interior.end());
        for (auto const& [position, vertex] : interior)
        {
          auto const& p = pts[vertex];
          chain.push_back(scatterPoint(p.at("u"), p.at("v")));
        }
        auto const [end_u, end_v] = pointAt(run.end);
        chain.push_back(scatterPoint(end_u, end_v));
        while (chain.size() > 16)
        {
          auto trimmed = nlohmann::json::array();
          for (std::size_t p = 0; p < chain.size(); ++p)
            if (p == 0 || p + 1 == chain.size() || p % 2 == 1)
              trimmed.push_back(chain[p]);
          chain = std::move(trimmed);
        }
        target.push_back({prefix, std::move(chain), run.end - run.begin});
      }
    };
    for (auto const& [name, loop] : base_loops)
      emitChains(loop, name, wall_chains);
    auto const emitRegions = [&](std::vector<WallChain>& chains,
                                 nlohmann::json& regions)
    {
      if (chains.size() > 48)
      {
        std::sort(chains.begin(), chains.end(),
          [](WallChain const& left, WallChain const& right)
          { return left.length > right.length; });
        chains.resize(48);
      }
      constexpr auto tile_size = 1600.0 / 3.0;
      for (std::size_t index = 0; index < chains.size(); ++index)
      {
        auto& chain = chains[index];
        // One baseline candidate per edge, plus one per 32 units of chain, so
        // every edge is covered to its corners at the props' physical length.
        auto const density = std::clamp<std::size_t>(
          static_cast<std::size_t>(std::ceil(chain.length
            * static_cast<double>(footprint_side_tiles) * tile_size / 32.0))
            + chain.points.size() - 1,
          1, 512);
        regions.push_back({
          {"name", chain.prefix + "_chain" + std::to_string(index) + "_wall"},
          {"role", "wall"}, {"points", std::move(chain.points)},
          {"density_per_tile", density},
          {"min_spacing_ratio", .002}, {"min_height", base - 15},
          {"max_height", base + 25}, {"min_slope_degrees", 0},
          {"max_slope_degrees", 75}, {"cluster_scale", 2.5},
          {"cluster_strength", 0.0}});
      }
    };
    emitRegions(wall_chains, wall_regions);
    // Base chains already carry their three public lane openings.
    auto wall_exclusions = nlohmann::json::array();
    auto vegetation_exclusions = nlohmann::json::array({
      exclusion("corridor", withoutHeight(top), lane_width + .025),
      exclusion("corridor", withoutHeight(middle), lane_width + .025),
      exclusion("corridor", withoutHeight(bottom), lane_width + .025),
      exclusion("corridor", withoutHeight(river), river_width + .02),
      exclusion("area", withoutHeight(left_base_outer), .025),
      exclusion("area", withoutHeight(right_base_outer), .025),
      exclusion("area", withoutHeight(objective_north), .025),
      exclusion("area", withoutHeight(objective_south), .025)
    });
    for (std::size_t i = 0; i < jungle_paths.size(); ++i)
      vegetation_exclusions.push_back(
        exclusion("corridor", withoutHeight(jungle_paths[i]), .025));
    for (auto const& camp : camps)
    {
      vegetation_exclusions.push_back(exclusion("area",
        withoutHeight(hexArea(camp.u, camp.v, .035, base)), .012));
      auto access = campAccess(camp, base);
      vegetation_exclusions.push_back(
        exclusion("corridor", withoutHeight(access), .02));
    }

    // --- Connectivity guarantee -------------------------------------------
    // Wall chains follow region loops with openings cut by exclusions, and
    // nothing structural prevents a loop from sealing a jungle pocket.
    // Rasterize the wall bands, flood fill the open ground from the lanes and
    // river, then carve extra openings until every camp, objective and base
    // court is reachable (flood-fill repair + reachability validation; the
    // graph-derived wall rework will later make this a pure safety net).
    constexpr auto grid_size = 160;
    auto const world_size = static_cast<double>(footprint_side_tiles)
      * (1600.0 / 3.0);
    // Full 32-unit wall chain spacing: the wall props are longer than their
    // spacing at their placement scale, so consecutive segments overlap and a
    // gap is only real when at least two adjacent candidates are dropped.
    auto const block_radius = 32.0 / world_size;
    auto const cellCenter = [&](int index)
    {
      return std::pair{(index % grid_size + .5) / grid_size,
                       (index / grid_size + .5) / grid_size};
    };
    auto const cellIndex = [&](double u, double v)
    {
      auto const x = std::clamp(static_cast<int>(u * grid_size), 0, grid_size - 1);
      auto const z = std::clamp(static_cast<int>(v * grid_size), 0, grid_size - 1);
      return z * grid_size + x;
    };
    // Rasterize the walls the scatter pipeline will actually place: the same
    // deterministic candidates, dropped by the same exclusion test, so
    // degenerate loops and physical prop footprints are modeled faithfully.
    auto const rasterizeBlocked = [&]() -> std::optional<std::vector<char>>
    {
      std::vector<char> blocked(grid_size * grid_size, 0);
      auto const markCall = [&](nlohmann::json const& regions,
                                nlohmann::json const& exclusions)
      {
        auto const parsed = parseProceduralScatter({{"seed", seed_value},
          {"assets", wall_assets}, {"regions", regions},
          {"exclusions", exclusions}});
        if (!parsed.scatter) return false;
        for (std::size_t region_index = 0;
             region_index < parsed.scatter->regions.size(); ++region_index)
        {
          auto const density =
            parsed.scatter->regions[region_index].density_per_tile;
          for (std::size_t index = 0; index < density; ++index)
          {
            auto const candidate = proceduralScatterCandidate(
              *parsed.scatter, region_index, 0, 0, index, 0.0f, 1.0f, 0.0f, 1.0f);
            if (!candidate.active) continue;
            if (proceduralScatterExcluded(
                  *parsed.scatter, candidate.u, candidate.v,
                  static_cast<float>(world_size), static_cast<float>(world_size)))
              continue;
            auto const cell_min_x = std::clamp(static_cast<int>(
              (candidate.u - block_radius) * grid_size), 0, grid_size - 1);
            auto const cell_max_x = std::clamp(static_cast<int>(
              (candidate.u + block_radius) * grid_size), 0, grid_size - 1);
            auto const cell_min_z = std::clamp(static_cast<int>(
              (candidate.v - block_radius) * grid_size), 0, grid_size - 1);
            auto const cell_max_z = std::clamp(static_cast<int>(
              (candidate.v + block_radius) * grid_size), 0, grid_size - 1);
            for (int z = cell_min_z; z <= cell_max_z; ++z)
              for (int x = cell_min_x; x <= cell_max_x; ++x)
              {
                auto const [u, v] = cellCenter(z * grid_size + x);
                if (std::hypot(u - candidate.u, v - candidate.v) <= block_radius)
                  blocked[z * grid_size + x] = 1;
              }
          }
        }
        return true;
      };
      if (!markCall(wall_regions, wall_exclusions))
        return std::nullopt;
      return blocked;
    };
    auto const floodFromLanes = [&](std::vector<char> const& blocked)
    {
      std::vector<char> reached(grid_size * grid_size, 0);
      std::deque<int> queue;
      for (int index = 0; index < grid_size * grid_size; ++index)
      {
        if (blocked[index]) continue;
        auto const [u, v] = cellCenter(index);
        if (segmentDistance(top, false, u, v) <= lane_width
            || segmentDistance(middle, false, u, v) <= lane_width
            || segmentDistance(bottom, false, u, v) <= lane_width
            || segmentDistance(river, false, u, v) <= river_width)
        {
          reached[index] = 1;
          queue.push_back(index);
        }
      }
      while (!queue.empty())
      {
        auto const index = queue.front();
        queue.pop_front();
        auto const x = index % grid_size;
        auto const z = index / grid_size;
        for (auto const& [dx, dz] : std::array<std::pair<int, int>, 4>{
               {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}})
        {
          auto const nx = x + dx;
          auto const nz = z + dz;
          if (nx < 0 || nx >= grid_size || nz < 0 || nz >= grid_size) continue;
          auto const neighbor = nz * grid_size + nx;
          if (blocked[neighbor] || reached[neighbor]) continue;
          reached[neighbor] = 1;
          queue.push_back(neighbor);
        }
      }
      return reached;
    };
    struct Poi { std::string name; double u; double v; };
    std::vector<Poi> pois;
    for (auto const& camp : camps) pois.push_back({camp.name, camp.u, camp.v});
    pois.push_back({"objective_north", .34, .27});
    pois.push_back({"objective_south", .66, .73});
    pois.push_back({"team_left_court", .113, .86});
    pois.push_back({"team_right_court", .887, .14});
    std::array<std::vector<ProceduralLayoutPoint>, 4> jungle_points;
    for (std::size_t i = 0; i < jungle_polygons.size(); ++i)
      jungle_points[i] = toLayoutPoints(jungle_polygons[i]);
    auto const inJungle = [&](double u, double v)
    {
      for (auto const& polygon : jungle_points)
        if (proceduralScatterContains(polygon, static_cast<float>(u),
                                      static_cast<float>(v)))
          return true;
      return false;
    };
    // An enclosed pocket is walkable jungle ground cut off from the lane
    // network, even without a POI inside: dead space the walls must not seal.
    auto const analyzeJunglePockets = [&](std::vector<char> const& blocked,
                                          std::vector<char> const& reached)
      -> std::pair<std::optional<int>, std::size_t>
    {
      std::vector<char> seen(grid_size * grid_size, 0);
      std::optional<int> best;
      std::size_t best_size = 0;
      std::size_t total_cells = 0;
      for (int index = 0; index < grid_size * grid_size; ++index)
      {
        if (blocked[index] || reached[index] || seen[index]) continue;
        auto const [u, v] = cellCenter(index);
        if (!inJungle(u, v)) continue;
        std::size_t size = 0;
        std::deque<int> queue{index};
        seen[index] = 1;
        while (!queue.empty())
        {
          auto const current = queue.front();
          queue.pop_front();
          ++size;
          auto const x = current % grid_size;
          auto const z = current / grid_size;
          for (auto const& [dx, dz] : std::array<std::pair<int, int>, 4>{
                 {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}})
          {
            auto const nx = x + dx;
            auto const nz = z + dz;
            if (nx < 0 || nx >= grid_size || nz < 0 || nz >= grid_size)
              continue;
            auto const neighbor = nz * grid_size + nx;
            if (blocked[neighbor] || reached[neighbor] || seen[neighbor])
              continue;
            seen[neighbor] = 1;
            queue.push_back(neighbor);
          }
        }
        if (size >= 4)
        {
          total_cells += size;
          if (size > best_size)
          {
            best_size = size;
            best = index;
          }
        }
      }
      return {best, total_cells};
    };
    auto repair_openings = std::size_t{0};
    auto unreachable_pois = nlohmann::json::array();
    auto enclosed_pocket_cells = std::size_t{0};
    for (int attempt = 0; attempt < 12; ++attempt)
    {
      auto const blocked_cells = rasterizeBlocked();
      if (!blocked_cells) break;
      auto const& blocked = *blocked_cells;
      auto const reached = floodFromLanes(blocked);
      unreachable_pois = nlohmann::json::array();
      std::optional<int> pocket;
      for (auto const& poi : pois)
      {
        if (!reached[cellIndex(poi.u, poi.v)])
        {
          unreachable_pois.push_back(poi.name);
          if (!pocket) pocket = cellIndex(poi.u, poi.v);
        }
      }
      auto const [pocket_seed, pocket_cells] = analyzeJunglePockets(blocked, reached);
      enclosed_pocket_cells = pocket_cells;
      if (!pocket) pocket = pocket_seed;
      if (!pocket) break;
      // Cheapest escape route: cross as few wall cells as possible, then
      // carve one opening in the middle of each wall run along that route.
      constexpr auto wall_cost = 400;
      std::vector<int> cost(grid_size * grid_size,
                            std::numeric_limits<int>::max());
      std::vector<int> previous(grid_size * grid_size, -1);
      std::deque<int> frontier{*pocket};
      cost[*pocket] = 0;
      while (!frontier.empty())
      {
        auto const index = frontier.front();
        frontier.pop_front();
        auto const x = index % grid_size;
        auto const z = index / grid_size;
        for (auto const& [dx, dz] : std::array<std::pair<int, int>, 4>{
               {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}})
        {
          auto const nx = x + dx;
          auto const nz = z + dz;
          if (nx < 0 || nx >= grid_size || nz < 0 || nz >= grid_size) continue;
          auto const neighbor = nz * grid_size + nx;
          auto const step = blocked[neighbor] ? wall_cost : 1;
          if (cost[index] + step >= cost[neighbor]) continue;
          cost[neighbor] = cost[index] + step;
          previous[neighbor] = index;
          if (blocked[neighbor]) frontier.push_back(neighbor);
          else frontier.push_front(neighbor);
        }
      }
      auto target = -1;
      auto best_cost = std::numeric_limits<int>::max();
      for (int index = 0; index < grid_size * grid_size; ++index)
        if (reached[index] && cost[index] < best_cost)
        {
          best_cost = cost[index];
          target = index;
        }
      if (target < 0) break;
      auto run = std::vector<int>{};
      auto openings_this_attempt = std::size_t{0};
      auto const carveRun = [&]
      {
        if (run.empty()) return;
        if (wall_exclusions.size() >= 96)
        {
          run.clear();
          return;
        }
        auto const [u, v] = cellCenter(run[run.size() / 2]);
        auto const opening = withoutHeight(hexArea(u, v, .03, base));
        wall_exclusions.push_back(exclusion("area", opening, .012));
        ++repair_openings;
        ++openings_this_attempt;
        run.clear();
      };
      for (auto index = target; index >= 0; index = previous[index])
      {
        if (blocked[index]) run.push_back(index);
        else carveRun();
      }
      carveRun();
      if (openings_this_attempt == 0) break;
    }

    auto distanceToPolyline = [](nlohmann::json const& path, double u, double v)
    {
      auto best = 10.0;
      for (std::size_t i = 1; i < path.size(); ++i)
      {
        auto const au = path[i - 1].at("u").get<double>();
        auto const av = path[i - 1].at("v").get<double>();
        auto const du = path[i].at("u").get<double>() - au;
        auto const dv = path[i].at("v").get<double>() - av;
        auto const length_squared = du * du + dv * dv;
        auto const t = length_squared > 0.0
          ? std::clamp(((u - au) * du + (v - av) * dv) / length_squared, 0.0, 1.0)
          : 0.0;
        best = std::min(best, std::hypot(u - (au + du * t), v - (av + dv * t)));
      }
      return best;
    };
    nlohmann::json props = nlohmann::json::array();
    auto addProp = [&](std::string name, std::string path, double u, double v,
                       double scale, double yaw, double height_offset)
    {
      props.push_back({{"name", std::move(name)}, {"path", std::move(path)},
        {"u", u}, {"v", v}, {"scale", scale}, {"yaw_degrees", yaw},
        {"height_offset", height_offset}});
    };
    addProp("team_left_landmark", propPath("base_landmark"), .113, .86, 1.2, 45, 0);
    addProp("team_left_glow", propPath("team_left_light"), .113, .86, 1.0, 0, 8);
    addProp("team_right_landmark", propPath("base_landmark"), .887, .14, 1.2, 225, 0);
    addProp("team_right_glow", propPath("team_right_light"), .887, .14, 1.0, 0, 8);
    addProp("objective_north_landmark", propPath("objective_landmark"),
            .34, .27, 1.4, 135, 0);
    addProp("objective_north_glow", propPath("river_light"), .34, .27, 1.0, 0, 6);
    addProp("objective_south_landmark", propPath("objective_landmark"),
            .66, .73, 1.4, 315, 0);
    addProp("objective_south_glow", propPath("river_light"), .66, .73, 1.0, 0, 6);
    addProp("river_glow_west", propPath("river_light"),
            river[1].at("u"), river[1].at("v"), 1.0, 0, 5);
    addProp("river_glow_east", propPath("river_light"),
            river[3].at("u"), river[3].at("v"), 1.0, 0, 5);
    for (auto const& camp : camps)
    {
      addProp(std::string{camp.name} + "_brazier", propPath("camp_marker"),
              camp.u + .02, camp.v - .02, 1.0, 0, 0);
      addProp(std::string{camp.name} + "_flame", propPath("flame_light"),
              camp.u + .02, camp.v - .02, 1.0, 0, 2.5);
    }
    struct EntranceFlank { char const* name; double u; double v; };
    auto const entrance_flanks = std::array{
      EntranceFlank{"top_a", .1215, .677}, EntranceFlank{"top_b", .0285, .713},
      EntranceFlank{"middle_a", .275, .795}, EntranceFlank{"middle_b", .205, .725},
      EntranceFlank{"bottom_a", .248, .985}, EntranceFlank{"bottom_b", .270, .888}
    };
    for (auto const& flank : entrance_flanks)
    {
      addProp("team_left_entrance_" + std::string{flank.name} + "_brazier",
              propPath("camp_marker"), flank.u, flank.v, 1.1, 0, 0);
      addProp("team_left_entrance_" + std::string{flank.name} + "_flame",
              propPath("flame_light"), flank.u, flank.v, 1.0, 0, 2.5);
      addProp("team_right_entrance_" + std::string{flank.name} + "_brazier",
              propPath("camp_marker"), 1.0 - flank.u, 1.0 - flank.v, 1.1, 0, 0);
      addProp("team_right_entrance_" + std::string{flank.name} + "_flame",
              propPath("flame_light"), 1.0 - flank.u, 1.0 - flank.v, 1.0, 0, 2.5);
    }
    auto lamp_count = std::size_t{0};
    auto lamp_light_count = std::size_t{0};
    constexpr auto lamp_step = .07;
    auto const lamp_offset = lane_width + .012;
    for (auto const& [lane_name, lane] : std::array{
           std::pair{"top", &top}, std::pair{"middle", &middle},
           std::pair{"bottom", &bottom}})
    {
      auto walked = 0.0;
      auto next_at = lamp_step * .5;
      auto side = 1.0;
      for (std::size_t i = 1; i < lane->size(); ++i)
      {
        auto const au = (*lane)[i - 1].at("u").get<double>();
        auto const av = (*lane)[i - 1].at("v").get<double>();
        auto const du = (*lane)[i].at("u").get<double>() - au;
        auto const dv = (*lane)[i].at("v").get<double>() - av;
        auto const segment = std::hypot(du, dv);
        while (next_at <= walked + segment && props.size() < 250)
        {
          auto const t = (next_at - walked) / segment;
          next_at += lamp_step;
          auto const center_u = au + du * t;
          auto const center_v = av + dv * t;
          // Lanes hugging the map border lose one shoulder: fall back to the
          // inner side instead of dropping the lamp.
          auto placeLamp = [&](double lamp_side)
          {
            auto const u = center_u + (-dv / segment) * lamp_offset * lamp_side;
            auto const v = center_v + (du / segment) * lamp_offset * lamp_side;
            if (u < .01 || u > .99 || v < .01 || v > .99) return false;
            if (proceduralScatterContains(left_base_points,
                                          static_cast<float>(u), static_cast<float>(v))
                || proceduralScatterContains(right_base_points,
                                             static_cast<float>(u), static_cast<float>(v)))
              return false;
            if (distanceToPolyline(river, u, v) < river_width + .03) return false;
            auto yaw = std::atan2(dv, -du) * 57.29577951308232;
            if (yaw < 0.0) yaw += 360.0;
            if (yaw >= 360.0) yaw = 0.0;
            ++lamp_count;
            addProp("lamp_" + std::string{lane_name} + "_" + std::to_string(lamp_count),
                    propPath("lane_lamp"), u, v, 1.0, yaw, 0);
            if (lamp_count % 2 == 0 && props.size() < 250)
            {
              ++lamp_light_count;
              addProp("lamp_glow_" + std::string{lane_name} + "_"
                        + std::to_string(lamp_count),
                      propPath("lamp_light"), u, v, 1.0, 0, 5);
            }
            return true;
          };
          if (!placeLamp(side)) placeLamp(-side);
          side = -side;
        }
        walked += segment;
      }
    }

    nlohmann::json terrain = {{"texture_paths", textures}, {"steep_texture_layer", 3},
      {"slope_start_degrees", 18}, {"slope_full_degrees", 34},
      {"edge_noise_ratio", .012}, {"max_slope_degrees", 55},
      {"smoothing_strength", .4}, {"features", std::move(terrain_features)}};
    // The water polygon stops on the submerged bank slope, where roughly one
    // third of the water column remains, so shore cells are shallow and the
    // depth attribute fades the edge out.
    nlohmann::json liquid = {{"replace_existing", true}, {"edge_noise_ratio", .004},
      {"features", nlohmann::json::array({{{"name", "central_river"}, {"shape", "corridor"},
        {"points", withoutHeight(river)}, {"half_width_ratio", river_width * .5},
        {"transition_width_ratio", std::min(.25, river_width * .2)}, {"liquid_type_id", liquid_value},
        {"depth", .65}, {"priority", 50}}})}};
    for (auto& p : liquid["features"][0]["points"]) p["height"] = water_height;
    nlohmann::json walls = {{"seed", seed_value}, {"assets", std::move(wall_assets)},
      {"regions", std::move(wall_regions)}, {"exclusions", std::move(wall_exclusions)}};
    nlohmann::json vegetation = {{"seed", seed_value},
      {"assets", std::move(vegetation_assets)},
      {"regions", std::move(vegetation_regions)},
      {"exclusions", std::move(vegetation_exclusions)}};
    nlohmann::json props_call = {{"props", std::move(props)}};
    if (auto const parsed = parseProceduralScatter(walls); !parsed.scatter)
      throw std::invalid_argument(parsed.error);
    if (auto const parsed = parseProceduralScatter(vegetation); !parsed.scatter)
      throw std::invalid_argument(parsed.error);
    if (auto const parsed = parseProceduralProps(props_call); !parsed.props)
      throw std::invalid_argument(parsed.error);

    return {{"ok", true}, {"operation", "create_moba_arena_blueprint"},
      {"connectivity", {{"grid_resolution", grid_size},
                        {"repair_openings", repair_openings},
                        {"enclosed_jungle_cells", enclosed_pocket_cells},
                        {"unreachable_pois", unreachable_pois}}},
      {"topology", {{"lanes", 3}, {"bases", 2}, {"river", 1},
                    {"objective_pits", 2}, {"jungle_regions", 4},
                    {"elevation_tiers", 4}, {"camp_clearings", 12},
                    {"jungle_camps", 12}, {"camp_alcoves", 12},
                    {"camp_entrances", 24},
                    {"jungle_floors", 4},
                    {"jungle_wall_bands", 0}, {"base_wall_bands", 2},
                    {"jungle_path_wall_bands", 0},
                    {"jungle_gates", 0},
                    {"path_wall_gates", 0},
                    {"jungle_paths", 8}, {"fortified_bases", 2},
                    {"public_entrances_per_base", 3},
                    {"landmarks", 4}, {"camp_braziers", 12},
                    {"entrance_braziers", 12}, {"lane_lamps", lamp_count},
                    {"skyboxes", 1},
                    {"dynamic_lights",
                     30 + lamp_light_count}}},
      {"next_calls", nlohmann::json::array({
        {{"name", "apply_terrain_layout_on_map"}, {"arguments", std::move(terrain)}},
        {{"name", "apply_liquid_layout_on_map"}, {"arguments", std::move(liquid)}},
        {{"name", "apply_ground_effect_on_map"}, {"arguments", {
          {"texture_path", textures.at(0)},
          {"effect_id", ground_effect_value}, {"overwrite", true}}}},
        {{"name", "scatter_assets_on_map"}, {"arguments", std::move(walls)}},
        {{"name", "place_props_on_map"}, {"arguments", std::move(props_call)}},
        {{"name", "scatter_assets_on_map"}, {"arguments", std::move(vegetation)}},
        {{"name", "apply_skybox_on_map"}, {"arguments", {
          {"skybox_path", skybox_path}, {"flags", skybox_flags}}}}
      })}};
  }
}
