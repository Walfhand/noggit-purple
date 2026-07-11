// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/ProceduralScatter.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace Noggit::Ai
{
  namespace
  {
    bool exactFields(nlohmann::json const& object, std::set<std::string> const& fields)
    {
      if (!object.is_object() || object.size() != fields.size()) return false;
      for (auto const& [name, value] : object.items())
      {
        static_cast<void>(value);
        if (!fields.contains(name)) return false;
      }
      return true;
    }

    bool finiteFloat(nlohmann::json const& value, float& result)
    {
      if (!value.is_number()) return false;
      auto const number = value.get<double>();
      if (!std::isfinite(number)
          || std::abs(number) > std::numeric_limits<float>::max()) return false;
      result = static_cast<float>(number);
      return true;
    }

    bool printableAscii(std::string const& value)
    {
      return std::all_of(value.begin(), value.end(), [](unsigned char character)
      {
        return character >= 32 && character <= 126;
      });
    }

    std::uint64_t hash(std::string const& seed, std::initializer_list<std::uint64_t> values)
    {
      auto result = std::uint64_t{1469598103934665603ULL};
      for (unsigned char byte : seed) result = (result ^ byte) * 1099511628211ULL;
      for (auto value : values)
      {
        for (unsigned shift = 0; shift < 64; shift += 8)
          result = (result ^ ((value >> shift) & 0xffU)) * 1099511628211ULL;
      }
      result ^= result >> 30;
      result *= 0xbf58476d1ce4e5b9ULL;
      result ^= result >> 27;
      result *= 0x94d049bb133111ebULL;
      return result ^ (result >> 31);
    }

    float unit(std::uint64_t value)
    {
      return static_cast<float>((value >> 40) & 0xffffffU) / 16777216.0f;
    }

    bool parsePoints(nlohmann::json const& values,
                     std::vector<ProceduralLayoutPoint>& points,
                     std::size_t minimum)
    {
      static auto const fields = std::set<std::string>{"u", "v"};
      if (!values.is_array() || values.size() < minimum || values.size() > 16) return false;
      for (auto const& value : values)
      {
        ProceduralLayoutPoint point;
        if (!exactFields(value, fields)
            || !finiteFloat(value.at("u"), point.u)
            || !finiteFloat(value.at("v"), point.v)
            || point.u < 0.0f || point.u > 1.0f
            || point.v < 0.0f || point.v > 1.0f
            || (!points.empty() && points.back().u == point.u && points.back().v == point.v))
          return false;
        points.push_back(point);
      }
      return true;
    }
  }

  ProceduralScatterParseResult parseProceduralScatter(nlohmann::json const& arguments)
  {
    static auto const root_fields = std::set<std::string>{
      "seed", "assets", "regions", "exclusions"};
    static auto const asset_fields = std::set<std::string>{
      "path", "weight", "min_scale", "max_scale"};
    static auto const region_fields = std::set<std::string>{
      "name", "points", "density_per_tile", "min_spacing_ratio",
      "min_height", "max_height", "min_slope_degrees", "max_slope_degrees"};
    static auto const exclusion_fields = std::set<std::string>{
      "shape", "points", "half_width_ratio"};
    auto fail = [](std::string error)
    {
      return ProceduralScatterParseResult{std::nullopt, std::move(error)};
    };

    if (!exactFields(arguments, root_fields))
      return fail("scatter_assets_on_map exige exactement seed, assets, regions et exclusions.");
    if (!arguments.at("seed").is_string()) return fail("seed doit être une chaîne.");

    ProceduralScatter scatter;
    scatter.seed = arguments.at("seed").get<std::string>();
    if (scatter.seed.empty() || scatter.seed.size() > 64 || !printableAscii(scatter.seed))
      return fail("seed doit être ASCII imprimable et contenir entre 1 et 64 caractères.");

    auto const& assets = arguments.at("assets");
    if (!assets.is_array() || assets.empty() || assets.size() > 16)
      return fail("assets doit contenir entre 1 et 16 modèles.");
    std::set<std::string> paths;
    for (auto const& value : assets)
    {
      ProceduralScatterAsset asset;
      if (!exactFields(value, asset_fields) || !value.at("path").is_string()
          || !finiteFloat(value.at("weight"), asset.weight)
          || !finiteFloat(value.at("min_scale"), asset.min_scale)
          || !finiteFloat(value.at("max_scale"), asset.max_scale))
        return fail("Chaque asset exige path, weight, min_scale et max_scale valides.");
      asset.path = value.at("path").get<std::string>();
      if (asset.path.empty() || asset.path.size() > 260 || !printableAscii(asset.path)
          || !paths.insert(asset.path).second
          || asset.weight <= 0.0f || asset.weight > 100.0f
          || asset.min_scale < 0.05f || asset.max_scale > 10.0f
          || asset.min_scale > asset.max_scale)
        return fail("Chemins uniques, poids dans ]0,100] et échelles dans [0.05,10] sont requis.");
      scatter.assets.push_back(std::move(asset));
    }

    auto const& regions = arguments.at("regions");
    if (!regions.is_array() || regions.empty() || regions.size() > 16)
      return fail("regions doit contenir entre 1 et 16 zones.");
    std::set<std::string> names;
    for (auto const& value : regions)
    {
      ProceduralScatterRegion region;
      if (!exactFields(value, region_fields) || !value.at("name").is_string()
          || !value.at("density_per_tile").is_number_integer()
          || !finiteFloat(value.at("min_spacing_ratio"), region.min_spacing_ratio)
          || !finiteFloat(value.at("min_height"), region.min_height)
          || !finiteFloat(value.at("max_height"), region.max_height)
          || !finiteFloat(value.at("min_slope_degrees"), region.min_slope_degrees)
          || !finiteFloat(value.at("max_slope_degrees"), region.max_slope_degrees))
        return fail("Chaque région contient des paramètres invalides.");
      region.name = value.at("name").get<std::string>();
      auto const density = value.at("density_per_tile").get<std::int64_t>();
      if (region.name.empty() || region.name.size() > 64 || !printableAscii(region.name)
          || !names.insert(region.name).second || density < 1 || density > 512
          || region.min_spacing_ratio < 0.001f || region.min_spacing_ratio > 0.25f
          || region.min_height < -500.0f || region.max_height > 5000.0f
          || region.min_height > region.max_height
          || region.min_slope_degrees < 0.0f || region.max_slope_degrees > 90.0f
          || region.min_slope_degrees > region.max_slope_degrees)
        return fail("Nom, densité, espacement, hauteur ou pente de région invalide.");
      region.density_per_tile = static_cast<std::size_t>(density);
      if (!parsePoints(value.at("points"), region.points, 3)
          || !isSimpleProceduralArea(region.points))
        return fail("Chaque région doit être un polygone simple de 3 à 16 points.");
      scatter.regions.push_back(std::move(region));
    }

    auto const& exclusions = arguments.at("exclusions");
    if (!exclusions.is_array() || exclusions.size() > 32)
      return fail("exclusions doit contenir au plus 32 formes.");
    for (auto const& value : exclusions)
    {
      ProceduralScatterExclusion exclusion;
      if (!exactFields(value, exclusion_fields) || !value.at("shape").is_string()
          || !finiteFloat(value.at("half_width_ratio"), exclusion.half_width_ratio)
          || exclusion.half_width_ratio < 0.001f || exclusion.half_width_ratio > 0.25f)
        return fail("Chaque exclusion exige shape, points et half_width_ratio valides.");
      auto const shape = value.at("shape").get<std::string>();
      if (shape == "corridor") exclusion.shape = ProceduralLayoutShape::Corridor;
      else if (shape == "area") exclusion.shape = ProceduralLayoutShape::Area;
      else return fail("La forme d'exclusion doit valoir corridor ou area.");
      if (!parsePoints(value.at("points"), exclusion.points,
                       exclusion.shape == ProceduralLayoutShape::Area ? 3 : 1)
          || (exclusion.shape == ProceduralLayoutShape::Area
              && !isSimpleProceduralArea(exclusion.points)))
        return fail("Les points d'une exclusion sont invalides.");
      scatter.exclusions.push_back(std::move(exclusion));
    }
    return {std::move(scatter), {}};
  }

  ProceduralScatterCandidate proceduralScatterCandidate(
    ProceduralScatter const& scatter, std::size_t region_index,
    std::size_t tile_x, std::size_t tile_z, std::size_t candidate_index,
    float u_min, float u_max, float v_min, float v_max)
  {
    auto const base = hash(scatter.seed, {region_index, tile_x, tile_z, candidate_index});
    ProceduralScatterCandidate candidate;
    candidate.u = u_min + unit(hash(scatter.seed, {base, 1})) * (u_max - u_min);
    candidate.v = v_min + unit(hash(scatter.seed, {base, 2})) * (v_max - v_min);
    auto total_weight = 0.0f;
    for (auto const& asset : scatter.assets) total_weight += asset.weight;
    auto choice = unit(hash(scatter.seed, {base, 3})) * total_weight;
    for (std::size_t index = 0; index < scatter.assets.size(); ++index)
    {
      candidate.asset_index = index;
      choice -= scatter.assets[index].weight;
      if (choice <= 0.0f) break;
    }
    auto const& asset = scatter.assets[candidate.asset_index];
    candidate.scale = asset.min_scale
      + unit(hash(scatter.seed, {base, 4})) * (asset.max_scale - asset.min_scale);
    candidate.yaw_degrees = unit(hash(scatter.seed, {base, 5})) * 360.0f;
    return candidate;
  }

  bool proceduralScatterContains(
    std::vector<ProceduralLayoutPoint> const& points, float u, float v)
  {
    return distanceToProceduralShape(
      points, ProceduralLayoutShape::Area, u, v, 1.0f, 1.0f).distance <= 0.0f;
  }

  bool proceduralScatterExcluded(
    ProceduralScatter const& scatter, float u, float v,
    float map_width, float map_height)
  {
    auto const short_side = std::min(map_width, map_height);
    return std::any_of(scatter.exclusions.begin(), scatter.exclusions.end(),
      [&](ProceduralScatterExclusion const& exclusion)
      {
        auto const distance = distanceToProceduralShape(
          exclusion.points, exclusion.shape, u * map_width, v * map_height,
          map_width, map_height).distance;
        return distance <= exclusion.half_width_ratio * short_side;
      });
  }
}
