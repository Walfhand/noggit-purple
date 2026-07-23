// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/MobaArenaBlueprint.hpp>
#include <noggit/ai/MobaArenaAudit.hpp>
#include <noggit/ai/ProceduralLayout.hpp>
#include <noggit/ai/ProceduralLiquidLayout.hpp>
#include <noggit/ai/ProceduralProps.hpp>
#include <noggit/ai/ProceduralScatter.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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

    nlohmann::json hexArea(double u, double v, double radius, double height,
                           double rotation_degrees = 0.0)
    {
      auto result = nlohmann::json::array();
      for (int vertex = 0; vertex < 6; ++vertex)
      {
        auto const angle = (180.0 + rotation_degrees + vertex * 60.0)
          * 0.017453292519943295;
        result.push_back(point(u + std::cos(angle) * radius,
                               v + std::sin(angle) * radius, height));
      }
      return result;
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

    nlohmann::json softenLoop(nlohmann::json const& polygon, double height)
    {
      auto result = nlohmann::json::array();
      for (std::size_t i = 0; i < polygon.size(); ++i)
      {
        auto const& a = polygon[i];
        auto const& b = polygon[(i + 1) % polygon.size()];
        auto const au = a.at("u").get<double>();
        auto const av = a.at("v").get<double>();
        auto const bu = b.at("u").get<double>();
        auto const bv = b.at("v").get<double>();
        result.push_back(point(au * .75 + bu * .25, av * .75 + bv * .25, height));
        result.push_back(point(au * .25 + bu * .75, av * .25 + bv * .75, height));
      }
      return result;
    }

    double fittedValue(double value, double minimum, double scale, bool inverse)
    {
      return minimum + (value - minimum) * (inverse ? 1.0 / scale : scale);
    }

    void fitPoints(nlohmann::json& points, double scale, bool inverse)
    {
      for (auto& value : points)
        for (auto const* axis : {"u", "v"})
          value[axis] = std::clamp(
            .5 + (value.at(axis).get<double>() - .5)
              * (inverse ? 1.0 / scale : scale),
            0.0, 1.0);
    }

    void fitWidth(nlohmann::json& object, char const* name,
                  double minimum, double scale, bool inverse)
    {
      if (object.contains(name))
        object[name] = fittedValue(
          object.at(name).get<double>(), minimum, scale, inverse);
    }

    void fitSlopeAngle(nlohmann::json& object, char const* name,
                       double scale, bool inverse)
    {
      auto const value = object.find(name);
      if (value == object.end() || value->is_null()) return;
      constexpr auto degrees_to_radians = 0.017453292519943295;
      constexpr auto radians_to_degrees = 57.29577951308232;
      auto const tangent = std::tan(value->get<double>() * degrees_to_radians);
      object[name] = std::atan(tangent * (inverse ? scale : 1.0 / scale))
        * radians_to_degrees;
    }

    void transformMobaArenaBlueprint(
      nlohmann::json& blueprint, double scale, bool inverse)
    {
      for (auto& call : blueprint.at("next_calls"))
      {
        auto const& name = call.at("name").get_ref<std::string const&>();
        auto& arguments = call.at("arguments");
        if (name == "apply_terrain_layout_on_map")
        {
          fitWidth(arguments, "edge_noise_ratio", 0.0, scale, inverse);
          fitSlopeAngle(arguments, "max_slope_degrees", scale, inverse);
          auto& features = arguments.at("features");
          if (inverse)
            features.erase(std::remove_if(features.begin(), features.end(),
              [](nlohmann::json const& value)
              { return value.at("name") == "arena_perimeter_relief"; }),
              features.end());
          for (auto& value : features)
          {
            fitPoints(value.at("points"), scale, inverse);
            fitWidth(value, "half_width_ratio", 0.0, scale, inverse);
            fitWidth(value, "transition_width_ratio", 0.0, scale, inverse);
            auto const& feature_name
              = value.at("name").get_ref<std::string const&>();
            if (feature_name == "objective_north"
                || feature_name == "objective_south")
              value["priority"] = inverse ? 70 : 91;
          }
        }
        else if (name == "apply_liquid_layout_on_map")
        {
          fitWidth(arguments, "edge_noise_ratio", 0.0, scale, inverse);
          for (auto& value : arguments.at("features"))
          {
            fitPoints(value.at("points"), scale, inverse);
            fitWidth(value, "half_width_ratio", 0.0, scale, inverse);
            fitWidth(value, "transition_width_ratio", 0.0, scale, inverse);
          }
        }
        else if (name == "scatter_assets_on_map")
        {
          for (auto& value : arguments.at("regions"))
          {
            fitPoints(value.at("points"), scale, inverse);
            fitWidth(value, "min_spacing_ratio", 0.0, scale, inverse);
          }
          for (auto& value : arguments.at("exclusions"))
          {
            fitPoints(value.at("points"), scale, inverse);
            fitWidth(value, "half_width_ratio", 0.0, scale, inverse);
          }
        }
        else if (name == "place_props_on_map")
        {
          for (auto& value : arguments.at("props"))
            for (auto const* axis : {"u", "v"})
              value[axis] = std::clamp(
                .5 + (value.at(axis).get<double>() - .5)
                  * (inverse ? 1.0 / scale : scale),
                0.0, 1.0);
        }
      }

      if (auto semantics = blueprint.find("moba_semantics");
          semantics != blueprint.end())
        for (auto const* collection : {"camps", "objectives", "bases"})
          for (auto& value : semantics->at(collection))
          {
            for (auto const* axis : {"u", "v"})
              value[axis] = std::clamp(
                .5 + (value.at(axis).get<double>() - .5)
                  * (inverse ? 1.0 / scale : scale),
                0.0, 1.0);
            fitWidth(value, "radius", 0.0, scale, inverse);
          }
    }

  }

  nlohmann::json canonicalMobaArenaBlueprint(
    nlohmann::json const& blueprint)
  {
    if (!blueprint.contains("arena_fit")) return blueprint;
    auto canonical = blueprint;
    auto const scale = canonical.at("arena_fit").at("scale_ratio").get<double>();
    transformMobaArenaBlueprint(canonical, scale, true);
    canonical.erase("arena_fit");
    return canonical;
  }

  nlohmann::json defaultMobaArenaSpecification()
  {
    return {
      {"texture_paths", nlohmann::json::array({
        "tileset/expansion07/zuldazarzone/8zul_moss01_512.blp",
        "tileset/expansion07/zuldazarzone/8zul_tile01_512.blp",
        "tileset/expansion07/zuldazarzone/8zul_redjunglefloor_1024.blp",
        "tileset/expansion07/zuldazarzone/8zul_rock01_1024.blp"
      })},
      {"liquid_type_id", 1},
      {"ground_effect_texture_id", 0},
      {"skybox_path", "environments/stars/legionnexus_netherskybox01.m2"},
      {"skybox_flags", 0},
      {"assets", nlohmann::json::array({
        {{"path", "world/expansion07/doodads/zuldazarzone/8zul_jungletree_a01.m2"},
         {"role", "canopy"}, {"weight", 3}, {"min_scale", .8},
         {"max_scale", 1.15}, {"spacing_multiplier", 1.25}},
        {{"path", "world/expansion07/doodads/zuldazarzone/8zul_jungletree_b01.m2"},
         {"role", "canopy"}, {"weight", 2}, {"min_scale", .8},
         {"max_scale", 1.15}, {"spacing_multiplier", 1.2}},
        {{"path", "world/expansion07/doodads/zuldazarzone/8zul_jungletree_b02.m2"},
         {"role", "canopy"}, {"weight", 2}, {"min_scale", .8},
         {"max_scale", 1.2}, {"spacing_multiplier", 1.2}},
        {{"path", "world/expansion07/doodads/zuldazarzone/8zul_bigleafybush_b01.m2"},
         {"role", "understory"}, {"weight", 4}, {"min_scale", .7},
         {"max_scale", 1.15}, {"spacing_multiplier", .55}},
        {{"path", "world/expansion07/doodads/zuldazarzone/8zul_fernbush_b01.m2"},
         {"role", "understory"}, {"weight", 4}, {"min_scale", .7},
         {"max_scale", 1.15}, {"spacing_multiplier", .5}},
        {{"path", "world/expansion07/doodads/bloodtroll/8tr_ancientblood_walltall01.m2"},
         {"role", "wall"}, {"weight", 1}, {"min_scale", 1.0},
         {"max_scale", 1.0}, {"spacing_multiplier", 1.0}},
        {{"path", "world/expansion07/doodads/zuldazarzone/8zul_junglerock_b01.m2"},
         {"role", "rock"}, {"weight", 2}, {"min_scale", .8},
         {"max_scale", 1.35}, {"spacing_multiplier", .8}},
        {{"path", "world/expansion07/doodads/zuldazarzone/8zul_junglerock_b02.m2"},
         {"role", "rock"}, {"weight", 2}, {"min_scale", .8},
         {"max_scale", 1.35}, {"spacing_multiplier", .8}},
        {{"path", "world/expansion07/doodads/zuldazarzone/8zul_fanleaf_a01.m2"},
         {"role", "detail"}, {"weight", 8}, {"min_scale", .65},
         {"max_scale", 1.1}, {"spacing_multiplier", .35}}
      })},
      {"prop_paths", {
        {"base_landmark",
         "world/expansion07/doodads/zandalaritroll/8tr_zandalari_fountain01.m2"},
        {"objective_landmark",
         "world/expansion07/doodads/zandalaritroll/8tr_zandalari_statueloaraptor01.m2"},
        {"camp_marker",
         "world/expansion07/doodads/zandalaritroll/8tr_zandalari_brazier01.m2"},
        {"lane_lamp",
         "world/expansion07/doodads/zandalaritroll/8tr_zandalari_fencerural_lamp01.m2"},
        {"team_left_light", "world/noggit/lights/noggit_light_deepskyblue01.m2"},
        {"team_right_light", "world/noggit/lights/noggit_light_orange01.m2"},
        {"river_light", "world/noggit/lights/noggit_light_purple_withshadows01.m2"},
        {"flame_light", "world/noggit/lights/noggit_light_orange_withshadows01.m2"},
        {"lamp_light", "world/noggit/lights/noggit_light_dimwhite01.m2"}
      }},
      {"seed", "moba-lab-1"},
      {"base_height", 20},
      {"arena_scale_ratio", .32},
      {"river_depth", 8},
      {"lane_width_ratio", .014},
      {"river_width_ratio", .03},
      {"lane_curvature", .6},
      {"river_curvature", .5},
      {"jungle_roughness", 5},
      {"vegetation_density_per_tile", 96}
    };
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
    nlohmann::json const& arguments, std::size_t footprint_side_tiles,
    std::size_t min_tile_x, std::size_t min_tile_z)
  {
    if (footprint_side_tiles < 2 || footprint_side_tiles > 4)
      throw std::invalid_argument(
        "Le blueprint MOBA exige un côté de carte entre 2 et 4 tuiles.");
    static auto const fields = std::set<std::string>{
      "texture_paths", "liquid_type_id", "assets", "seed", "base_height",
      "arena_scale_ratio", "river_depth", "lane_width_ratio", "river_width_ratio",
      "lane_curvature", "river_curvature", "jungle_roughness",
      "vegetation_density_per_tile", "ground_effect_texture_id", "prop_paths",
      "skybox_path", "skybox_flags"
    };
    if (!arguments.is_object() || arguments.size() != fields.size())
      throw std::invalid_argument("Le blueprint MOBA exige exactement ses 17 paramètres.");
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
    auto const arena_scale = finiteNumber(
      arguments, "arena_scale_ratio", .25, 1.0);
    auto const river_depth = finiteNumber(arguments, "river_depth", 2.0, 30.0);
    auto const lane_width = finiteNumber(arguments, "lane_width_ratio", 0.012, 0.055);
    auto const river_width = finiteNumber(arguments, "river_width_ratio", 0.015, 0.08);
    auto const lane_curve = finiteNumber(arguments, "lane_curvature", 0.0, 1.0);
    auto const river_curve = finiteNumber(arguments, "river_curvature", 0.0, 1.0);
    auto const roughness = finiteNumber(arguments, "jungle_roughness", 1.0, 12.0);
    auto const vertical_scale
      = static_cast<double>(footprint_side_tiles) / 4.0;
    auto const effective_river_depth = river_depth * vertical_scale;
    auto const river_height = base - effective_river_depth;
    // Keep the water column under WoW's swim threshold so the river is
    // always wadeable on foot, however deep the bed is carved.
    auto const water_height = river_height
      + std::min(1.0, effective_river_depth * 0.25);
    auto const target_lane_bend = (lane_curve - .6) * .005;
    auto const target_river_bend = (river_curve - .5) * .005;
    // Elevation tiers: river < lanes < jungle paths < broad forest relief,
    // with the base courts raised so lanes ramp up into them.
    // Keep small footprints from becoming needlessly steeper while retaining
    // enough rise for the traced masses to remain physically impassable.
    auto const relief_rise = 13.0 * vertical_scale;
    auto const path_rise = 1.5 * vertical_scale;
    auto const apron_rise = 2.0 * vertical_scale;
    auto const court_rise = 4.0 * vertical_scale;
    auto const relief_top = base + relief_rise;
    auto const path_top = base + path_rise;

    // Centerlines extracted from the supplied SR mask with a hard ten-pixel
    // clearance. The former synthetic curve crossed several reference walls
    // and created the oversized lane/river openings visible in the editor.
    auto top = nlohmann::json::array({
      point(.07,.89,base), point(.09 + target_lane_bend,.65,base),
      point(.086 + target_lane_bend,.25,base),
      point(.20,.10 - target_lane_bend,base),
      point(.65,.09 - target_lane_bend,base), point(.93,.12,base)});
    auto middle = nlohmann::json::array({
      point(.07,.89,base), point(.93,.11,base)});
    auto bottom = nlohmann::json::array();
    auto river = nlohmann::json::array({
      point(.1996 - target_river_bend,.1750 - target_river_bend,river_height),
      point(.2020 - target_river_bend,.2000,river_height),
      point(.2485 - target_river_bend,.3105 + target_river_bend,river_height),
      point(.50,.50,river_height),
      point(.7515 + target_river_bend,.6895 - target_river_bend,river_height),
      point(.7980 + target_river_bend,.8000,river_height),
      point(.8004 + target_river_bend,.8250 + target_river_bend,river_height)});

    auto const mirror = [](nlohmann::json const& points)
    {
      auto result = nlohmann::json::array();
      for (auto const& p : points)
        result.push_back(point(1.0 - p.at("u").get<double>(),
                               1.0 - p.at("v").get<double>(),
                               p.at("height").get<double>()));
      std::reverse(result.begin(), result.end());
      return result;
    };
    bottom = mirror(top);
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
    // Competitive topology stays balanced, but SR's jungle silhouettes are
    // intentionally asymmetric. These 46 islands are traced from the supplied
    // reference mask; selected large/concave islands keep one extra detail level.
    auto const jungle_polygons = std::array<nlohmann::json, 46>{
      // Reference wall 1: source area 2457 px.
      nlohmann::json::array({
        point(0.2520,0.2632,relief_top), point(0.2891,0.2476,relief_top), point(0.3047,0.2183,relief_top),
        point(0.3496,0.2320,relief_top), point(0.3711,0.2246,relief_top), point(0.3828,0.2671,relief_top), point(0.4180,0.3548,relief_top),
        point(0.3926,0.3704,relief_top), point(0.3242,0.3450,relief_top), point(0.3691,0.3119,relief_top),
        point(0.3516,0.2671,relief_top), point(0.3105,0.2710,relief_top), point(0.2930,0.3236,relief_top)
      }),
      // Reference wall 2: source area 2440 px.
      nlohmann::json::array({
        point(0.5898,0.6550,relief_top), point(0.6133,0.6394,relief_top), point(0.6816,0.6647,relief_top),
        point(0.6387,0.6979,relief_top), point(0.6387,0.7193,relief_top), point(0.6562,0.7407,relief_top),
        point(0.6914,0.7388,relief_top), point(0.7129,0.6862,relief_top), point(0.7539,0.7485,relief_top),
        point(0.7168,0.7602,relief_top), point(0.7012,0.7856,relief_top), point(0.6523,0.7719,relief_top),
        point(0.6100,0.7720,relief_top), point(0.5960,0.7360,relief_top), point(0.6113,0.6842,relief_top)
      }),
      // Reference wall 3: source area 1697 px.
      nlohmann::json::array({
        point(0.1875,0.5517,relief_top), point(0.2031,0.5302,relief_top), point(0.2090,0.5673,relief_top),
        point(0.2344,0.5984,relief_top), point(0.2637,0.6140,relief_top), point(0.3125,0.6101,relief_top),
        point(0.3223,0.6296,relief_top), point(0.2773,0.6706,relief_top), point(0.2031,0.6374,relief_top)
      }),
      // Reference wall 4: source area 1575 px.
      nlohmann::json::array({
        point(0.1172,0.4951,relief_top), point(0.1660,0.5244,relief_top), point(0.1621,0.5789,relief_top),
        point(0.1777,0.6062,relief_top), point(0.1680,0.6277,relief_top), point(0.1172,0.6238,relief_top)
      }),
      // Reference wall 5: source area 1506 px.
      nlohmann::json::array({
        point(0.1191,0.2788,relief_top), point(0.1328,0.2378,relief_top), point(0.1758,0.2573,relief_top),
        point(0.2031,0.2846,relief_top), point(0.2305,0.3411,relief_top), point(0.1934,0.3002,relief_top),
        point(0.1641,0.3002,relief_top), point(0.1465,0.3392,relief_top), point(0.1191,0.3509,relief_top)
      }),
      // Reference wall 6: source area 1440 px.
      nlohmann::json::array({
        point(0.2207,0.1365,relief_top), point(0.2500,0.1209,relief_top), point(0.3359,0.1189,relief_top),
        point(0.3320,0.1404,relief_top), point(0.2812,0.1657,relief_top), point(0.2754,0.2183,relief_top),
        point(0.2539,0.2047,relief_top)
      }),
      // Reference wall 7: source area 1393 px.
      nlohmann::json::array({
        point(0.8281,0.3899,relief_top), point(0.8828,0.3801,relief_top), point(0.8828,0.5146,relief_top),
        point(0.8418,0.4893,relief_top), point(0.8477,0.4425,relief_top)
      }),
      // Reference wall 8: source area 1367 px.
      nlohmann::json::array({
        point(0.3027,0.1813,relief_top), point(0.3750,0.1910,relief_top), point(0.4160,0.1754,relief_top),
        point(0.4238,0.1930,relief_top), point(0.4473,0.1969,relief_top), point(0.4707,0.1657,relief_top),
        point(0.4883,0.1813,relief_top), point(0.4883,0.2125,relief_top), point(0.3926,0.2339,relief_top)
      }),
      // Reference wall 9: source area 1319 px.
      nlohmann::json::array({
        point(0.6738,0.3918,relief_top), point(0.7266,0.3392,relief_top), point(0.7988,0.3723,relief_top),
        point(0.7910,0.4055,relief_top), point(0.8164,0.4327,relief_top), point(0.8105,0.4815,relief_top),
        point(0.8008,0.4795,relief_top), point(0.7949,0.4288,relief_top), point(0.7695,0.3977,relief_top),
        point(0.7344,0.3860,relief_top)
      }),
      // Reference wall 10: source area 1271 px.
      nlohmann::json::array({
        point(0.5137,0.8070,relief_top), point(0.5234,0.7914,relief_top), point(0.5801,0.7934,relief_top),
        point(0.6172,0.7797,relief_top), point(0.6973,0.8207,relief_top), point(0.6387,0.8168,relief_top),
        point(0.5938,0.8304,relief_top), point(0.5859,0.8148,relief_top), point(0.5586,0.8109,relief_top),
        point(0.5352,0.8421,relief_top), point(0.5176,0.8324,relief_top)
      }),
      // Reference wall 11: source area 1220 px.
      nlohmann::json::array({
        point(0.4219,0.2651,relief_top), point(0.4355,0.2456,relief_top), point(0.4629,0.2378,relief_top),
        point(0.4512,0.2632,relief_top), point(0.4570,0.2846,relief_top), point(0.4941,0.2943,relief_top),
        point(0.5137,0.2632,relief_top), point(0.5254,0.3021,relief_top), point(0.4863,0.3236,relief_top),
        point(0.4336,0.3236,relief_top)
      }),
      // Reference wall 12: source area 1128 px.
      nlohmann::json::array({
        point(0.4824,0.7076,relief_top), point(0.5234,0.6862,relief_top), point(0.5742,0.6862,relief_top),
        point(0.5840,0.7388,relief_top), point(0.5410,0.7700,relief_top), point(0.5547,0.7544,relief_top),
        point(0.5508,0.7251,relief_top), point(0.5137,0.7154,relief_top), point(0.4941,0.7466,relief_top)
      }),
      // Reference wall 13: source area 1128 px.
      nlohmann::json::array({
        point(0.3594,0.4425,relief_top), point(0.4238,0.4893,relief_top), point(0.4198,0.5265,relief_top),
        point(0.4042,0.5405,relief_top), point(0.3691,0.5107,relief_top)
      }),
      // Reference wall 14: source area 1097 px.
      nlohmann::json::array({
        point(0.4297,0.8616,relief_top), point(0.4492,0.8363,relief_top), point(0.4746,0.8304,relief_top),
        point(0.5312,0.8674,relief_top), point(0.5332,0.8869,relief_top), point(0.4316,0.8869,relief_top)
      }),
      // Reference wall 15: source area 1085 px.
      nlohmann::json::array({
        point(0.4746,0.1326,relief_top), point(0.4766,0.1189,relief_top), point(0.5684,0.1189,relief_top),
        point(0.5781,0.1481,relief_top), point(0.5645,0.1657,relief_top), point(0.5215,0.1735,relief_top)
      }),
      // Reference wall 16: source area 1081 px.
      nlohmann::json::array({
        point(0.5918,0.3197,relief_top), point(0.6230,0.2554,relief_top), point(0.6523,0.2554,relief_top),
        point(0.6758,0.2885,relief_top), point(0.6367,0.3314,relief_top), point(0.5957,0.3353,relief_top)
      }),
      // Reference wall 17: source area 1041 px.
      nlohmann::json::array({
        point(0.4766,0.3567,relief_top), point(0.4980,0.3548,relief_top), point(0.5117,0.3840,relief_top),
        point(0.5391,0.3860,relief_top), point(0.5566,0.3684,relief_top), point(0.5547,0.3255,relief_top),
        point(0.5742,0.3606,relief_top), point(0.6016,0.3645,relief_top), point(0.5332,0.4250,relief_top),
        point(0.4766,0.3821,relief_top)
      }),
      // Reference wall 18: source area 1036 px.
      nlohmann::json::array({
        point(0.5820,0.4854,relief_top), point(0.6051,0.4749,relief_top), point(0.6290,0.4970,relief_top),
        point(0.6387,0.5661,relief_top), point(0.5840,0.5263,relief_top)
      }),
      // Reference wall 19: source area 989 px.
      nlohmann::json::array({
        point(0.7207,0.7992,relief_top), point(0.7539,0.7817,relief_top), point(0.7949,0.8148,relief_top),
        point(0.7852,0.8558,relief_top), point(0.7617,0.8791,relief_top)
      }),
      // Reference wall 20: source area 978 px.
      nlohmann::json::array({
        point(0.1172,0.3801,relief_top), point(0.1523,0.3645,relief_top), point(0.1289,0.4444,relief_top),
        point(0.1836,0.4639,relief_top), point(0.1895,0.4854,relief_top), point(0.1758,0.5010,relief_top),
        point(0.1172,0.4678,relief_top)
      }),
      // Reference wall 21: source area 968 px.
      nlohmann::json::array({
        point(0.5762,0.1891,relief_top), point(0.6035,0.1618,relief_top), point(0.6016,0.1209,relief_top),
        point(0.6230,0.1189,relief_top), point(0.6387,0.2203,relief_top), point(0.6035,0.2378,relief_top)
      }),
      // Reference wall 22: source area 957 px.
      nlohmann::json::array({
        point(0.4102,0.6433,relief_top), point(0.4707,0.5867,relief_top), point(0.5293,0.6296,relief_top),
        point(0.5293,0.6511,relief_top), point(0.5098,0.6530,relief_top), point(0.4961,0.6257,relief_top),
        point(0.4609,0.6257,relief_top), point(0.4473,0.6550,relief_top), point(0.4629,0.6803,relief_top),
        point(0.4453,0.6803,relief_top)
      }),
      // Reference wall 23: source area 909 px.
      nlohmann::json::array({
        point(0.6836,0.5965,relief_top), point(0.7129,0.5965,relief_top), point(0.7695,0.6238,relief_top),
        point(0.7949,0.6569,relief_top), point(0.7930,0.6901,relief_top), point(0.7793,0.6920,relief_top),
        point(0.7656,0.6589,relief_top)
      }),
      // Reference wall 24: source area 899 px.
      nlohmann::json::array({
        point(0.8105,0.7173,relief_top), point(0.8296,0.7123,relief_top), point(0.8812,0.7550,relief_top),
        point(0.8584,0.7843,relief_top), point(0.8223,0.7680,relief_top)
      }),
      // Reference wall 25: source area 889 px.
      nlohmann::json::array({
        point(0.3320,0.7154,relief_top), point(0.3691,0.6823,relief_top), point(0.4141,0.6803,relief_top),
        point(0.3828,0.7485,relief_top), point(0.3633,0.7602,relief_top)
      }),
      // Reference wall 26: source area 875 px.
      nlohmann::json::array({
        point(0.3730,0.7836,relief_top), point(0.4023,0.7719,relief_top), point(0.4336,0.8168,relief_top),
        point(0.4062,0.8460,relief_top), point(0.4062,0.8869,relief_top), point(0.3867,0.8869,relief_top)
      }),
      // Reference wall 27: source area 870 px.
      nlohmann::json::array({
        point(0.1699,0.4094,relief_top), point(0.1855,0.3353,relief_top), point(0.2285,0.4347,relief_top),
        point(0.1875,0.4444,relief_top)
      }),
      // Reference wall 28: source area 825 px.
      nlohmann::json::array({
        point(0.2129,0.4971,relief_top), point(0.2324,0.4795,relief_top), point(0.2520,0.4932,relief_top),
        point(0.2754,0.4815,relief_top), point(0.2773,0.4600,relief_top), point(0.2559,0.4327,relief_top),
        point(0.2988,0.4464,relief_top), point(0.3008,0.4971,relief_top), point(0.2676,0.5088,relief_top)
      }),
      // Reference wall 29: source area 801 px.
      nlohmann::json::array({
        point(0.5156,0.2222,relief_top), point(0.5430,0.2105,relief_top), point(0.5742,0.2281,relief_top),
        point(0.5801,0.2671,relief_top), point(0.5625,0.3080,relief_top), point(0.5449,0.2534,relief_top)
      }),
      // Reference wall 30: source area 790 px.
      nlohmann::json::array({
        point(0.4219,0.7329,relief_top), point(0.4473,0.7018,relief_top), point(0.4785,0.7953,relief_top),
        point(0.4551,0.7973,relief_top), point(0.4297,0.7778,relief_top)
      }),
      // Reference wall 31: source area 771 px.
      nlohmann::json::array({
        point(0.7793,0.5789,relief_top), point(0.7891,0.5673,relief_top), point(0.8301,0.5770,relief_top),
        point(0.8281,0.6706,relief_top)
      }),
      // Reference wall 32: source area 736 px.
      nlohmann::json::array({
        point(0.7031,0.5185,relief_top), point(0.7383,0.5029,relief_top), point(0.7910,0.5068,relief_top),
        point(0.7754,0.5263,relief_top), point(0.7461,0.5185,relief_top), point(0.7285,0.5302,relief_top),
        point(0.7266,0.5556,relief_top), point(0.7480,0.5750,relief_top), point(0.7051,0.5692,relief_top)
      }),
      // Reference wall 33: source area 714 px.
      nlohmann::json::array({
        point(0.6309,0.8519,relief_top), point(0.7129,0.8596,relief_top), point(0.7285,0.8869,relief_top),
        point(0.6504,0.8869,relief_top)
      }),
      // Reference wall 34: source area 654 px.
      nlohmann::json::array({
        point(0.6973,0.4444,relief_top), point(0.7109,0.4191,relief_top), point(0.7363,0.4113,relief_top),
        point(0.7246,0.4308,relief_top), point(0.7285,0.4581,relief_top), point(0.7715,0.4678,relief_top),
        point(0.7539,0.4854,relief_top), point(0.7246,0.4873,relief_top), point(0.7031,0.4717,relief_top)
      }),
      // Reference wall 35: source area 644 px.
      nlohmann::json::array({
        point(0.2363,0.5419,relief_top), point(0.2559,0.5244,relief_top), point(0.2871,0.5244,relief_top),
        point(0.3086,0.5478,relief_top), point(0.3086,0.5731,relief_top), point(0.2695,0.5984,relief_top),
        point(0.2812,0.5556,relief_top)
      }),
      // Reference wall 36: source area 636 px.
      nlohmann::json::array({
        point(0.4258,0.3957,relief_top), point(0.4707,0.4055,relief_top), point(0.5156,0.4444,relief_top),
        point(0.4922,0.4639,relief_top)
      }),
      // Reference wall 37: source area 627 px.
      nlohmann::json::array({
        point(0.8496,0.6764,relief_top), point(0.8613,0.6199,relief_top), point(0.8828,0.6179,relief_top),
        point(0.8828,0.7135,relief_top), point(0.8535,0.6979,relief_top)
      }),
      // Reference wall 38: source area 598 px.
      nlohmann::json::array({
        point(0.2363,0.3645,relief_top), point(0.2461,0.3587,relief_top), point(0.3262,0.4172,relief_top),
        point(0.2637,0.4113,relief_top), point(0.2402,0.3899,relief_top)
      }),
      // Reference wall 39: source area 517 px.
      nlohmann::json::array({
        point(0.8184,0.5224,relief_top), point(0.8340,0.5127,relief_top), point(0.8828,0.5439,relief_top),
        point(0.8828,0.5945,relief_top), point(0.8652,0.5965,relief_top), point(0.8730,0.5595,relief_top),
        point(0.8281,0.5536,relief_top)
      }),
      // Reference wall 40: source area 491 px.
      nlohmann::json::array({
        point(0.3301,0.5088,relief_top), point(0.3457,0.5127,relief_top), point(0.3613,0.5478,relief_top),
        point(0.3496,0.6004,relief_top), point(0.3340,0.5965,relief_top), point(0.3398,0.5458,relief_top)
      }),
      // Reference wall 41: source area 484 px.
      nlohmann::json::array({
        point(0.3711,0.1287,relief_top), point(0.4512,0.1228,relief_top), point(0.4473,0.1404,relief_top),
        point(0.4062,0.1481,relief_top), point(0.3750,0.1481,relief_top)
      }),
      // Reference wall 42: source area 361 px.
      nlohmann::json::array({
        point(0.6486,0.4580,relief_top), point(0.6558,0.4219,relief_top), point(0.6648,0.4174,relief_top),
        point(0.6561,0.4500,relief_top), point(0.6660,0.5010,relief_top), point(0.6543,0.4990,relief_top)
      }),
      // Reference wall 43: source area 331 px.
      nlohmann::json::array({
        point(0.4922,0.5634,relief_top), point(0.5059,0.5536,relief_top), point(0.5762,0.6179,relief_top),
        point(0.5273,0.6023,relief_top)
      }),
      // Reference wall 44: source area 99 px.
      nlohmann::json::array({
        point(0.0841,0.1270,relief_top), point(0.1018,0.1094,relief_top),
        point(0.1135,0.1152,relief_top), point(0.1096,0.1230,relief_top),
        point(0.0939,0.1367,relief_top)
      }),
      // Reference wall 45: source area 276 px.
      nlohmann::json::array({
        point(0.5597,0.8750,relief_top), point(0.6047,0.8633,relief_top),
        point(0.6223,0.8867,relief_top), point(0.5597,0.8867,relief_top)
      }),
      // Reference wall 46: source area 123 px.
      nlohmann::json::array({
        point(0.8865,0.8926,relief_top), point(0.8943,0.8848,relief_top),
        point(0.9080,0.8730,relief_top), point(0.9198,0.8828,relief_top),
        point(0.8982,0.9043,relief_top), point(0.8865,0.8984,relief_top)
      })
    };
    // An area already owns its complete interior, so its core can have zero
    // width. Only ordinary broad masses receive the one-pixel inset needed to
    // compensate the slope transition; objective guards, separators and tiny
    // islands retain their exact traced contour.
    auto wall_polygons = jungle_polygons;
    auto objective_guard = std::array<bool, 46>{};
    auto river_bank_guard = std::array<bool, 46>{};
    for (std::size_t i = 0; i < wall_polygons.size(); ++i)
    {
      objective_guard[i]
        = segmentDistance(wall_polygons[i], true, .333, .281) < .06
        || segmentDistance(wall_polygons[i], true, .667, .719) < .06;
      river_bank_guard[i]
        = segmentDistance(wall_polygons[i], true, .635, .540) < .05
        || segmentDistance(wall_polygons[i], true, .365, .460) < .05;
      if (i == 23 || river_bank_guard[i]) continue;
      auto const very_high_excess = i == 7 || i == 20 || i == 25;
      auto const high_excess_pair = i == 2 || i == 8 || i == 15
        || i == 16 || i == 21 || i == 24;
      auto const moderate_excess = i == 4 || i == 9;
      auto const objective_inset = i == 1 ? .00375 : .002;
      auto const inset = i == 11 ? .003 : (very_high_excess ? .006
        : ((high_excess_pair || moderate_excess) ? .005
        : (i >= 43 ? .004 : (objective_guard[i] ? objective_inset : .004))));
      auto const candidate = offsetLoopInward(
        wall_polygons[i], inset, relief_top);
      auto points = std::vector<ProceduralLayoutPoint>{};
      points.reserve(candidate.size());
      for (auto const& value : candidate)
        points.push_back({value.at("u").get<float>(),
                          value.at("v").get<float>(),
                          value.at("height").get<float>()});
      if (isSimpleProceduralArea(points)) wall_polygons[i] = candidate;
    }
    auto field_polygons = jungle_polygons;
    for (auto& polygon : field_polygons)
    {
      auto const candidate = offsetLoopInward(polygon, .003, relief_top);
      auto points = std::vector<ProceduralLayoutPoint>{};
      points.reserve(candidate.size());
      for (auto const& value : candidate)
        points.push_back({value.at("u").get<float>(),
                          value.at("v").get<float>(),
                          value.at("height").get<float>()});
      if (isSimpleProceduralArea(points)) polygon = candidate;
    }
    // Camps use Summoner's Rift's real normalized positions (both maps share
    // the same frame: blue base bottom-left, mid on the rising diagonal,
    // river on the falling one; see sr-jungle-reference.md). Three sizes
    // like SR: buff hubs, medium camps and small spurs. West and east
    // quadrants sweep an L (wolves -> blue -> gromp between two doors of the
    // same outer lane); north and south chain into the map corner
    // (raptors -> red -> krugs).
    enum class CampKind { Spur, Medium, Hub };
    struct Camp
    {
      char const* name;
      double u;
      double v;
      double radius;
      CampKind kind;
    };
    auto const camps = std::array{
      Camp{"north_raptors", .5245, .3496, .0210, CampKind::Medium},
      Camp{"north_red", .4755, .2637, .0165, CampKind::Hub},
      Camp{"north_krugs", .4344, .1621, .0175, CampKind::Spur},
      Camp{"east_wolves", .7573, .4297, .0190, CampKind::Medium},
      Camp{"east_blue", .7613, .5430, .0160, CampKind::Hub},
      Camp{"east_gromp", .8493, .5703, .0135, CampKind::Spur},
      Camp{"south_raptors", .4755, .6504, .0210, CampKind::Medium},
      Camp{"south_red", .5245, .7363, .0165, CampKind::Hub},
      Camp{"south_krugs", .5656, .8379, .0175, CampKind::Spur},
      Camp{"west_wolves", .2427, .5703, .0190, CampKind::Medium},
      Camp{"west_blue", .2387, .4570, .0160, CampKind::Hub},
      Camp{"west_gromp", .1507, .4297, .0135, CampKind::Spur}
    };
    auto const campRadius = [](Camp const& camp) { return camp.radius; };
    // Objective pits hug the river bank: their hex overlaps the river bed on
    // one side while the jungle wall seals every other side, so drake and
    // nashor are reachable through the river only.
    auto const objective_north = softenLoop(
      hexArea(.333, .281, .040, base - vertical_scale),
      base - vertical_scale);
    auto const objective_south = softenLoop(
      hexArea(.667, .719, .040, base - vertical_scale),
      base - vertical_scale);
    // The circulation network per quadrant, SR-style: the buff is a hub,
    // medium camps sit on the main route and the small camp is a short spur.
    // Five doors per quadrant connect that graph to lane and river.
    struct JunglePath
    {
      char const* name;
      char const* quadrant;
      char const* role;
      nlohmann::json points;
      std::vector<std::string> doors;
    };
    auto const junglePath = [](char const* name, char const* quadrant,
                               char const* role, nlohmann::json points,
                               std::initializer_list<char const*> doors = {})
    {
      auto boundary_names = std::vector<std::string>{};
      boundary_names.reserve(doors.size());
      for (auto const* door : doors) boundary_names.emplace_back(door);
      return JunglePath{name, quadrant, role, std::move(points),
                        std::move(boundary_names)};
    };
    // Centerlines follow the open pixels of the supplied reference. Together
    // they use 51 segments, below the remaining terrain-feature budget.
    auto const north_clear = nlohmann::json::array({
      point(.5245,.3496,path_top), point(.5264,.2598,path_top),
      point(.4755,.2637,path_top), point(.5382,.1914,path_top)});
    auto const north_spur = nlohmann::json::array({
      point(.4755,.2637,path_top), point(.4990,.1719,path_top),
      point(.4423,.1582,path_top), point(.4344,.1621,path_top)});
    auto const north_branch = nlohmann::json::array({
      point(.4207,.4199,base), point(.4286,.3789,path_top),
      point(.5127,.3398,path_top), point(.5245,.3496,path_top)});
    auto const north_lane_door = nlohmann::json::array({
      point(.5793,.4199,base), point(.6928,.2734,path_top),
      point(.7495,.0996,base)});

    auto const east_clear = nlohmann::json::array({
      point(.7573,.4297,path_top), point(.8004,.5254,path_top),
      point(.7613,.5430,path_top), point(.8004,.6504,path_top)});
    auto const east_spur = nlohmann::json::array({
      point(.7613,.5430,path_top), point(.8493,.5703,path_top)});
    auto const east_branch = nlohmann::json::array({
      point(.5793,.5801,base), point(.6536,.5723,path_top),
      point(.6497,.5000,path_top), point(.6771,.5039,path_top),
      point(.7104,.4043,path_top), point(.7573,.4160,path_top),
      point(.7573,.4297,path_top)});
    auto const east_lane_door = nlohmann::json::array({
      point(.9256,.3809,base), point(.8845,.5254,path_top),
      point(.9178,.6367,path_top), point(.9256,.6797,base)});

    auto const south_clear = nlohmann::json::array({
      point(.4755,.6504,path_top), point(.4853,.7539,path_top),
      point(.5245,.7363,path_top), point(.4697,.8086,path_top)});
    auto const south_spur = nlohmann::json::array({
      point(.5245,.7363,path_top), point(.5088,.8398,path_top),
      point(.5577,.8457,path_top), point(.5656,.8379,path_top)});
    auto const south_branch = nlohmann::json::array({
      point(.5793,.5801,base), point(.5753,.6328,path_top),
      point(.4932,.6680,path_top), point(.4755,.6504,path_top)});
    auto const south_lane_door = nlohmann::json::array({
      point(.4207,.5801,base), point(.3170,.7383,path_top),
      point(.2505,.9004,base)});

    auto const west_clear = nlohmann::json::array({
      point(.2427,.5703,path_top), point(.2035,.4863,path_top),
      point(.2387,.4570,path_top), point(.2016,.3496,path_top)});
    auto const west_spur = nlohmann::json::array({
      point(.2387,.4570,path_top), point(.1585,.4375,path_top),
      point(.1507,.4297,path_top)});
    auto const west_branch = nlohmann::json::array({
      point(.4207,.4199,base), point(.2427,.4336,path_top),
      point(.2035,.5117,path_top), point(.2427,.5703,path_top)});
    auto const west_lane_door = nlohmann::json::array({
      point(.0744,.6191,base), point(.1194,.4844,path_top),
      point(.0978,.3691,path_top), point(.0744,.3203,base)});

    auto const jungle_paths = std::array{
      junglePath("jungle_1_clear_path", "north", "clear", north_clear),
      junglePath("jungle_1_spur_path", "north", "spur", north_spur),
      junglePath("jungle_1_branch_path", "north", "branch", north_branch,
                  {"river_bed"}),
      junglePath("jungle_1_lane_door_path", "north", "door", north_lane_door,
                  {"middle_lane", "top_lane"}),
      junglePath("jungle_2_clear_path", "east", "clear", east_clear),
      junglePath("jungle_2_spur_path", "east", "spur", east_spur),
      junglePath("jungle_2_branch_path", "east", "branch", east_branch,
                  {"river_bed"}),
      junglePath("jungle_2_lane_door_path", "east", "door", east_lane_door,
                  {"bottom_lane", "bottom_lane"}),
      junglePath("jungle_3_clear_path", "south", "clear", south_clear),
      junglePath("jungle_3_spur_path", "south", "spur", south_spur),
      junglePath("jungle_3_branch_path", "south", "branch", south_branch,
                  {"river_bed"}),
      junglePath("jungle_3_lane_door_path", "south", "door", south_lane_door,
                  {"middle_lane", "bottom_lane"}),
      junglePath("jungle_4_clear_path", "west", "clear", west_clear),
      junglePath("jungle_4_spur_path", "west", "spur", west_spur),
      junglePath("jungle_4_branch_path", "west", "branch", west_branch,
                  {"river_bed"}),
      junglePath("jungle_4_lane_door_path", "west", "door", west_lane_door,
                  {"top_lane", "top_lane"})
    };

    auto const jungle_wall_cuts = std::array{
      junglePath("jungle_1_mid_wall_cut", "north", "wall_cut",
        nlohmann::json::array({point(.7104,.2891,base),
          point(.6086,.3477,path_top)}), {"middle_lane"}),
      junglePath("jungle_1_outer_wall_cut", "north", "separator",
        nlohmann::json::array({point(.6500,.0900,base),
          point(.6300,.1400,path_top)}), {"top_lane"}),
      junglePath("jungle_2_mid_wall_cut", "east", "wall_cut",
        nlohmann::json::array({point(.6595,.3398,base),
          point(.6751,.4023,path_top)}), {"middle_lane"}),
      junglePath("jungle_2_upper_wall_cut", "east", "separator",
        nlohmann::json::array({point(.7593,.2402,base),
          point(.6673,.3867,path_top)}), {"middle_lane"}),
      junglePath("jungle_3_mid_wall_cut", "south", "wall_cut",
        nlohmann::json::array({point(.2896,.7109,base),
          point(.3914,.6523,path_top)}), {"middle_lane"}),
      junglePath("jungle_3_outer_wall_cut", "south", "separator",
        nlohmann::json::array({point(.3500,.9100,base),
          point(.3700,.8600,path_top)}), {"bottom_lane"}),
      junglePath("jungle_4_mid_wall_cut", "west", "wall_cut",
        nlohmann::json::array({point(.3405,.6602,base),
          point(.3151,.5996,path_top)}), {"middle_lane"}),
      junglePath("jungle_4_upper_wall_cut", "west", "separator",
        nlohmann::json::array({point(.2407,.7598,base),
          point(.3327,.6133,path_top)}), {"middle_lane"})
    };
    auto const left_base_outer = nlohmann::json::array({point(.01,.70,base + apron_rise),
      point(.10,.68,base + apron_rise), point(.24,.76,base + apron_rise),
      point(.30,.90,base + apron_rise), point(.20,.99,base + apron_rise),
      point(.01,.99,base + apron_rise)});
    auto const left_base_inner = nlohmann::json::array({point(.025,.78,base + court_rise),
      point(.09,.75,base + court_rise), point(.18,.81,base + court_rise),
      point(.21,.90,base + court_rise), point(.15,.96,base + court_rise),
      point(.025,.96,base + court_rise)});
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
    auto jungle_wall_points = std::vector<std::vector<ProceduralLayoutPoint>>{};
    jungle_wall_points.reserve(jungle_polygons.size());
    for (auto const& polygon : jungle_polygons)
      jungle_wall_points.push_back(toLayoutPoints(polygon));
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

    auto terrain_features = nlohmann::json::array();
    terrain_features.push_back(feature("arena_ground", "area",
      nlohmann::json::array({point(0, 0, base), point(1, 0, base),
        point(1, 1, base), point(0, 1, base)}),
      .005, .02, 0, 5, "absolute", 2.0, .85));
    for (std::size_t i = 0; i < wall_polygons.size(); ++i)
      terrain_features.push_back(feature(
        "jungle_" + std::to_string(i + 1) + "_wall_mass", "area",
        wall_polygons[i], 0.0, .001, 0, 80,
        "absolute",
        roughness * .25, .8));
    // Most routes follow the open negative space between walls. Door and spur
    // centerlines guarantee the few narrow throats that cross the traced mask.
    for (auto const& path : jungle_paths)
    {
      auto const spur_throat = std::string_view{path.role} == "spur";
      auto const outer_loop_throat = std::string_view{path.role} == "door"
        && (std::string_view{path.quadrant} == "east"
            || std::string_view{path.quadrant} == "west");
      auto const guaranteed_throat = spur_throat || outer_loop_throat;
      terrain_features.push_back(feature(path.name, "corridor",
        path.points, outer_loop_throat ? .009
          : (spur_throat ? .006 : .012),
        (spur_throat || outer_loop_throat) ? .003 : .006, 0,
        guaranteed_throat ? 85 : 55, "absolute", .4, .95, .18));
    }
    for (auto const& cut : jungle_wall_cuts)
      terrain_features.push_back(feature(cut.name, "corridor",
        cut.points, .008, .005, 0, 85, "absolute", .25, .95, .12));
    // Narrow deep channel with long submerged banks: the shoreline lands on
    // the gentle underwater slope, so edge water is shallow and fades out
    // instead of meeting the bank at full depth.
    // The river must win at lane crossings to create one continuous wadeable
    // ford instead of four disconnected MH2O fragments.
    terrain_features.push_back(feature("river_bed", "corridor", river,
      river_width * .5, std::min(.017, river_width * .57), 2, 92,
      "absolute", 0, 1, .18));
    terrain_features.push_back(feature("objective_north", "area",
      objective_north, .005, .025, 2, 70));
    terrain_features.push_back(feature("objective_south", "area",
      objective_south, .005, .025, 2, 70));
    // Flat green combat spaces fill the negative space without erasing the
    // traced wall silhouette. Camp markers carry the gameplay identity; a
    // large orange platform is not part of the supplied SR reference.
    // Each clearing gets its own hex orientation so rooms stop reading as a
    // repeated stamp; mirrored camps share an angle (hexes are point-symmetric
    // so the map stays competitively fair).
    for (std::size_t index = 0; index < camps.size(); ++index)
    {
      auto const& camp = camps[index];
      terrain_features.push_back(feature(std::string{camp.name} + "_camp_floor",
        "area", hexArea(camp.u, camp.v, campRadius(camp), path_top,
                        static_cast<double>(index % 6) * 37.0),
        .005, .001, 0, 100, "absolute", .35, .25));
    }
    terrain_features.push_back(feature("team_left_base_apron", "area", left_base_outer,
      .005, .018, 3, 40, "absolute", .4, .9));
    terrain_features.push_back(feature("team_right_base_apron", "area", right_base_outer,
      .005, .018, 3, 40, "absolute", .4, .9));
    // Courts sit ABOVE the lanes: raiseLaneEnds already puts the lane ends at
    // court height so nothing changes visually, but wide curved lanes
    // converging inside the base can no longer erase the court's effective
    // core (which would abort the in-app blueprint execution).
    terrain_features.push_back(feature("team_left_inner_court", "area", left_base_inner,
      .005, .014, 1, 95));
    terrain_features.push_back(feature("team_right_inner_court", "area", right_base_inner,
      .005, .014, 1, 95));
    terrain_features.push_back(feature("top_lane", "corridor", top,
      lane_width, .015, 1, 90, "absolute", 0, 1, .18));
    terrain_features.push_back(feature("middle_lane", "corridor", middle,
      lane_width, .015, 1, 90, "absolute", 0, 1, .12));
    terrain_features.push_back(feature("bottom_lane", "corridor", bottom,
      lane_width, .015, 1, 90, "absolute", 0, 1, .18));

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
      RoleScatter{"canopy", 8.0, .003, 3.0, 0.0, 0, 75},
      RoleScatter{"understory", 8.0, .001, 5.0, 0.0, 0, 75},
      RoleScatter{"rock", 8.0, .001, 4.0, 0.0, 0, 90},
      RoleScatter{"detail", 8.0, .001, 7.0, 0.0, 0, 75}
    };
    auto const usesRole = [&](std::size_t polygon_index, char const* role)
    {
      auto const role_name = std::string{role};
      if (role_name == "canopy")
        return polygon_index != 0 && polygon_index != 5
          && polygon_index != 22 && polygon_index != 26
          && polygon_index != 30 && polygon_index != 35
          && polygon_index != 38 && polygon_index != 44;
      if (role_name == "understory") return polygon_index == 0;
      if (role_name == "rock") return polygon_index == 5;
      return false;
    };
    auto total_factor = 0.0;
    for (std::size_t i = 0; i < jungle_polygons.size(); ++i)
      for (auto const& role : role_scatter)
        if (role_counts[role.role] > 0 && usesRole(i, role.role))
          total_factor += role.density_factor;
    auto const density_scale = std::min(1.0,
      2048.0 / (density_value.get<double>() * total_factor));

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
        if (role_counts[role.role] == 0 || !usesRole(i, role.role)) continue;
        auto const points = withoutHeight(
          std::string{role.role} == "canopy"
            ? field_polygons[i] : jungle_polygons[i]);
        auto const objective_canopy_multiplier
          = std::string_view{role.role} == "canopy" && objective_guard[i]
          ? 1.5 : 1.0;
        auto const density = std::max(1, static_cast<int>(std::lround(
          density_value.get<double>() * role.density_factor * density_scale
            * objective_canopy_multiplier)));
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
      exclusion("area", withoutHeight(objective_north), .04),
      exclusion("area", withoutHeight(objective_south), .04)
    });
    for (auto const& path : jungle_paths)
      vegetation_exclusions.push_back(
        exclusion("corridor", withoutHeight(path.points), .033));
    for (auto const& cut : jungle_wall_cuts)
      vegetation_exclusions.push_back(
        exclusion("corridor", withoutHeight(cut.points), .024));
    for (std::size_t index = 0; index < camps.size(); ++index)
    {
      auto const& camp = camps[index];
      vegetation_exclusions.push_back(exclusion("area",
        withoutHeight(hexArea(camp.u, camp.v, campRadius(camp) + .008, base,
                              static_cast<double>(index % 6) * 37.0)), .012));
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
            .333, .281, 1.4, 135, 0);
    addProp("objective_north_glow", propPath("river_light"), .333, .281, 1.0, 0, 6);
    addProp("objective_south_landmark", propPath("objective_landmark"),
            .667, .719, 1.4, 315, 0);
    addProp("objective_south_glow", propPath("river_light"), .667, .719, 1.0, 0, 6);
    addProp("river_glow_west", propPath("river_light"),
            river[1].at("u"), river[1].at("v"), 1.0, 0, 5);
    addProp("river_glow_east", propPath("river_light"),
            river[3].at("u"), river[3].at("v"), 1.0, 0, 5);
    for (auto const& camp : camps)
    {
      auto const name = std::string_view{camp.name};
      auto const offset_sign = name.starts_with("north_")
        || name.starts_with("east_") ? 1.0 : -1.0;
      addProp(std::string{camp.name} + "_brazier", propPath("camp_marker"),
              camp.u + offset_sign * campRadius(camp) * .5,
              camp.v - offset_sign * campRadius(camp) * .5, 1.0, 0, 0);
      addProp(std::string{camp.name} + "_flame", propPath("flame_light"),
              camp.u + offset_sign * campRadius(camp) * .5,
              camp.v - offset_sign * campRadius(camp) * .5, 1.0, 0, 2.5);
    }
    struct EntranceFlank { char const* name; double u; double v; };
    auto const entrance_flanks = std::array{
      EntranceFlank{"top_a", .095, .677}, EntranceFlank{"top_b", .0285, .713},
      EntranceFlank{"middle_a", .230, .760}, EntranceFlank{"middle_b", .200, .755},
      EntranceFlank{"bottom_a", .248, .925}, EntranceFlank{"bottom_b", .270, .888}
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
    constexpr auto lamp_step = .07;
    // Keep solid lamp posts inside the flat outer portion of the lane. At the
    // sharp border bends, placing them beyond the lane transition can land a
    // post on the jungle wall even when the polyline normal looks correct.
    auto const lamp_offset = lane_width * .75;
    auto lamp_counts = std::map<std::string, std::size_t>{};
    auto const validLampPosition = [&](double u, double v)
    {
      return u >= .01 && u <= .99 && v >= .01 && v <= .99
        && !proceduralScatterContains(
          left_base_points, static_cast<float>(u), static_cast<float>(v))
        && !proceduralScatterContains(
          right_base_points, static_cast<float>(u), static_cast<float>(v))
        && distanceToPolyline(river, u, v) >= river_width + .03
        && std::none_of(jungle_wall_points.begin(), jungle_wall_points.end(),
          [&](auto const& polygon)
          {
            return proceduralScatterContains(
              polygon, static_cast<float>(u), static_cast<float>(v));
          });
    };
    auto addLamp = [&](std::string const& lane_name, double u, double v,
                       double yaw, bool glow)
    {
      auto const ordinal = ++lamp_counts[lane_name];
      auto const suffix = lane_name + "_" + std::to_string(ordinal);
      addProp("lamp_" + suffix, propPath("lane_lamp"), u, v, 1.0, yaw, 0);
      if (glow)
        addProp("lamp_glow_" + suffix, propPath("lamp_light"),
                u, v, 1.0, 0, 5);
    };
    auto generateLampPairs = [&](std::string const& lane_name,
                                 std::string const& mirror_name,
                                 nlohmann::json const& lane,
                                 bool first_half_only)
    {
      auto total = 0.0;
      for (std::size_t i = 1; i < lane.size(); ++i)
        total += std::hypot(lane[i].at("u").get<double>()
                              - lane[i - 1].at("u").get<double>(),
                            lane[i].at("v").get<double>()
                              - lane[i - 1].at("v").get<double>());
      auto walked = 0.0;
      auto next_at = lamp_step * .5;
      auto side = 1.0;
      auto pair_count = std::size_t{0};
      for (std::size_t i = 1; i < lane.size(); ++i)
      {
        auto const au = lane[i - 1].at("u").get<double>();
        auto const av = lane[i - 1].at("v").get<double>();
        auto const du = lane[i].at("u").get<double>() - au;
        auto const dv = lane[i].at("v").get<double>() - av;
        auto const segment = std::hypot(du, dv);
        while (next_at <= walked + segment
               && (!first_half_only || next_at < total * .5)
               && props.size() + 4 <= procedural_props_max_count)
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
            if (!validLampPosition(u, v)
                || !validLampPosition(1.0 - u, 1.0 - v)) return false;
            auto yaw = std::atan2(dv, -du) * 57.29577951308232;
            if (yaw < 0.0) yaw += 360.0;
            if (yaw >= 360.0) yaw = 0.0;
            auto const glow = ++pair_count % 2 == 0;
            addLamp(lane_name, u, v, yaw, glow);
            addLamp(mirror_name, 1.0 - u, 1.0 - v,
                    std::fmod(yaw + 180.0, 360.0), glow);
            return true;
          };
          if (!placeLamp(side)) placeLamp(-side);
          side = -side;
        }
        walked += segment;
      }
    };
    generateLampPairs("top", "bottom", top, false);
    generateLampPairs("middle", "middle", middle, true);

    nlohmann::json semantics = {
      {"camps", nlohmann::json::array()},
      {"routes", nlohmann::json::array()},
      {"objectives", nlohmann::json::array({
        {{"name", "objective_north"}, {"u", .333}, {"v", .281}},
        {{"name", "objective_south"}, {"u", .667}, {"v", .719}}
      })},
      {"bases", nlohmann::json::array({
        {{"name", "team_left_inner_court"}, {"u", .113}, {"v", .86}},
        {{"name", "team_right_inner_court"}, {"u", .887}, {"v", .14}}
      })},
      {"boundaries", nlohmann::json::array(
        {"top_lane", "middle_lane", "bottom_lane", "river_bed"})}
    };
    for (auto const& camp : camps)
    {
      auto const name = std::string{camp.name};
      auto const separator = name.find('_');
      semantics["camps"].push_back({
        {"name", name}, {"quadrant", name.substr(0, separator)},
        {"kind", camp.kind == CampKind::Hub ? "hub"
          : camp.kind == CampKind::Medium ? "medium" : "spur"},
        {"feature", name + "_camp_floor"},
        {"u", camp.u}, {"v", camp.v}, {"radius", campRadius(camp)}
      });
    }
    for (auto const& path : jungle_paths)
      semantics["routes"].push_back({
        {"name", path.name},
        {"quadrant", path.quadrant}, {"role", path.role},
        {"doors", path.doors}
      });
    for (auto const& cut : jungle_wall_cuts)
      semantics["routes"].push_back({
        {"name", cut.name},
        {"quadrant", cut.quadrant}, {"role", cut.role},
        {"doors", cut.doors}
      });

    nlohmann::json terrain = {{"texture_paths", textures}, {"steep_texture_layer", 3},
      {"slope_start_degrees", 18}, {"slope_full_degrees", 34},
      {"edge_noise_ratio", 0.0}, {"max_slope_degrees", 56},
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

    auto blueprint = nlohmann::json{{"ok", true},
      {"operation", "create_moba_arena_blueprint"},
      {"moba_semantics", std::move(semantics)},
      {"next_calls", nlohmann::json::array({
        {{"name", "validate_moba_footprint"},
         {"arguments", nlohmann::json::object()}},
        {{"name", "apply_terrain_layout_on_map"}, {"arguments", std::move(terrain)}},
        {{"name", "apply_liquid_layout_on_map"}, {"arguments", std::move(liquid)}},
        {{"name", "apply_ground_effect_on_map"}, {"arguments", {
          {"texture_path", textures.at(0)},
          {"effect_id", ground_effect_value}, {"overwrite", true}}}},
        {{"name", "scatter_assets_on_map"}, {"arguments", std::move(walls)}},
        {{"name", "place_props_on_map"}, {"arguments", std::move(props_call)}},
        {{"name", "scatter_assets_on_map"}, {"arguments", std::move(vegetation)}},
        {{"name", "apply_skybox_on_map"}, {"arguments", {
          {"skybox_path", skybox_path}, {"flags", skybox_flags},
          {"lighting_param_index", 3}}}},
        {{"name", "validate_map"}, {"arguments", nlohmann::json::object()}}
      })}};
    auto const audit = auditMobaArenaBlueprint(blueprint, footprint_side_tiles,
      moba_arena_audit_preview_resolution, min_tile_x, min_tile_z);
    if (!audit.ok())
    {
      auto message = std::string{"Le blueprint MOBA "}
        + std::to_string(footprint_side_tiles) + "x"
        + std::to_string(footprint_side_tiles) + " généré a échoué à l'audit :";
      for (auto const& issue : audit.issues)
      {
        message += "\n- [" + issue.code + "]";
        if (!issue.subject.empty()) message += " " + issue.subject;
        message += ": " + issue.message;
      }
      throw std::invalid_argument(message);
    }
    blueprint["audit"] = mobaArenaAuditSummary(audit);

    if (arena_scale < 1.0)
    {
      transformMobaArenaBlueprint(blueprint, arena_scale, false);
      auto& fitted_features
        = blueprint["next_calls"][1]["arguments"]["features"];
      auto const perimeter_height = base + std::max(24.0, relief_rise * 2.0);
      fitted_features.insert(fitted_features.begin(), feature(
        "arena_perimeter_relief", "area",
        nlohmann::json::array({point(0, 0, perimeter_height),
          point(1, 0, perimeter_height), point(1, 1, perimeter_height),
          point(0, 1, perimeter_height)}),
        0.0, .001, 3, 1, "absolute", roughness * .4, 1.0));

      constexpr auto tile_size = 1600.0 / 3.0;
      constexpr auto wow_run_speed = 7.0;
      auto const playable_side = arena_scale
        * static_cast<double>(footprint_side_tiles) * tile_size;
      blueprint["arena_fit"] = {
        {"scale_ratio", arena_scale},
        {"playable_side_world_units", playable_side},
        {"estimated_base_to_base_run_seconds",
          playable_side * std::hypot(.887 - .113, .86 - .14)
            / wow_run_speed},
        {"perimeter_relief_height", perimeter_height}
      };
      blueprint["audit"]["metrics"]["arena_fit"] = blueprint["arena_fit"];

      auto const& terrain_arguments = blueprint["next_calls"][1]["arguments"];
      auto const& liquid_arguments = blueprint["next_calls"][2]["arguments"];
      auto const& wall_arguments = blueprint["next_calls"][4]["arguments"];
      auto const& props_arguments = blueprint["next_calls"][5]["arguments"];
      auto const& vegetation_arguments = blueprint["next_calls"][6]["arguments"];
      if (auto const parsed = parseProceduralLayout(terrain_arguments);
          !parsed.layout)
        throw std::invalid_argument(parsed.error);
      if (auto const parsed = parseProceduralLiquidLayout(liquid_arguments);
          !parsed.layout)
        throw std::invalid_argument(parsed.error);
      if (auto const parsed = parseProceduralScatter(wall_arguments);
          !parsed.scatter)
        throw std::invalid_argument(parsed.error);
      if (auto const parsed = parseProceduralProps(props_arguments);
          !parsed.props)
        throw std::invalid_argument(parsed.error);
      if (auto const parsed = parseProceduralScatter(vegetation_arguments);
          !parsed.scatter)
        throw std::invalid_argument(parsed.error);
    }
    return blueprint;
  }
}
