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

    float smoothNoise(std::string const& seed, float u, float v,
                      float scale, std::uint64_t channel)
    {
      auto const x = u * scale;
      auto const y = v * scale;
      auto const x0 = static_cast<std::int64_t>(std::floor(x));
      auto const y0 = static_cast<std::int64_t>(std::floor(y));
      auto const smooth = [](float value) { return value * value * (3.0f - 2.0f * value); };
      auto const tx = smooth(x - std::floor(x));
      auto const ty = smooth(y - std::floor(y));
      auto sample = [&](std::int64_t sx, std::int64_t sy)
      {
        return unit(hash(seed, {channel, static_cast<std::uint64_t>(sx),
                                static_cast<std::uint64_t>(sy)}));
      };
      auto const a = std::lerp(sample(x0, y0), sample(x0 + 1, y0), tx);
      auto const b = std::lerp(sample(x0, y0 + 1), sample(x0 + 1, y0 + 1), tx);
      return std::lerp(a, b, ty);
    }

    bool validRole(std::string const& role)
    {
      return role == "canopy" || role == "understory"
          || role == "rock" || role == "wall" || role == "detail";
    }

    struct WallSample
    {
      float u = 0.0f;
      float v = 0.0f;
      float yaw_degrees = 0.0f;
    };

    WallSample wallSample(std::vector<ProceduralLayoutPoint> const& points,
                          std::size_t index, std::size_t count,
                          bool skip_corridor_end_caps, bool open_chain)
    {
      struct Edge
      {
        std::size_t start = 0;
        float du = 0.0f;
        float dv = 0.0f;
        float length = 0.0f;
        std::size_t samples = 1;
      };
      auto edges = std::vector<Edge>{};
      auto perimeter = 0.0f;
      for (std::size_t edge = 0; edge < points.size(); ++edge)
      {
        if (skip_corridor_end_caps
            && (edge + 1 == points.size() / 2 || edge + 1 == points.size()))
          continue;
        if (open_chain && edge + 1 == points.size()) continue;
        auto const& first = points[edge];
        auto const& second = points[(edge + 1) % points.size()];
        auto const du = second.u - first.u;
        auto const dv = second.v - first.v;
        auto const length = std::hypot(du, dv);
        if (length <= 0.0f) continue;
        edges.push_back({edge, du, dv, length, 1});
        perimeter += length;
      }
      if (edges.empty() || count == 0) return {};

      if (count < edges.size())
      {
        auto remaining = (static_cast<float>(index % count) + 0.5f)
          / static_cast<float>(count) * perimeter;
        for (auto const& edge : edges)
        {
          if (remaining > edge.length)
          {
            remaining -= edge.length;
            continue;
          }
          auto const progress = std::clamp(remaining / edge.length, 0.0f, 1.0f);
          auto yaw = std::atan2(edge.dv, -edge.du) * 57.29577951308232f;
          if (yaw < 0.0f) yaw += 360.0f;
          auto const& first = points[edge.start];
          return {first.u + edge.du * progress,
                  first.v + edge.dv * progress, yaw};
        }
        return {points.front().u, points.front().v, 0.0f};
      }

      for (auto assigned = edges.size(); assigned < count; ++assigned)
      {
        auto best = edges.begin();
        for (auto edge = edges.begin() + 1; edge != edges.end(); ++edge)
          if (edge->length / static_cast<float>(edge->samples)
              > best->length / static_cast<float>(best->samples))
            best = edge;
        ++best->samples;
      }
      auto local_index = index % count;
      for (auto const& edge : edges)
      {
        if (local_index >= edge.samples)
        {
          local_index -= edge.samples;
          continue;
        }
        auto const progress = (static_cast<float>(local_index) + 0.5f)
          / static_cast<float>(edge.samples);
        auto yaw = std::atan2(edge.dv, -edge.du) * 57.29577951308232f;
        if (yaw < 0.0f) yaw += 360.0f;
        auto const& first = points[edge.start];
        return {first.u + edge.du * progress,
                first.v + edge.dv * progress, yaw};
      }
      return {points.front().u, points.front().v, 0.0f};
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
      "path", "role", "weight", "min_scale", "max_scale", "spacing_multiplier"};
    static auto const region_fields = std::set<std::string>{
      "name", "role", "points", "density_per_tile", "min_spacing_ratio",
      "min_height", "max_height", "min_slope_degrees", "max_slope_degrees",
      "cluster_scale", "cluster_strength"};
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
          || !value.at("role").is_string()
          || !finiteFloat(value.at("weight"), asset.weight)
          || !finiteFloat(value.at("min_scale"), asset.min_scale)
          || !finiteFloat(value.at("max_scale"), asset.max_scale)
          || !finiteFloat(value.at("spacing_multiplier"), asset.spacing_multiplier))
        return fail("Chaque asset exige path, role, weight, échelles et espacement valides.");
      asset.path = value.at("path").get<std::string>();
      asset.role = value.at("role").get<std::string>();
      if (asset.path.empty() || asset.path.size() > 260 || !printableAscii(asset.path)
          || !paths.insert(asset.path).second
          || !validRole(asset.role)
          || asset.weight <= 0.0f || asset.weight > 100.0f
          || asset.min_scale < 0.05f || asset.max_scale > 10.0f
          || asset.min_scale > asset.max_scale
          || asset.spacing_multiplier < 0.25f || asset.spacing_multiplier > 4.0f)
        return fail("Chemins uniques, rôles valides, poids, échelles et espacements cohérents sont requis.");
      scatter.assets.push_back(std::move(asset));
    }

    auto const& regions = arguments.at("regions");
    if (!regions.is_array() || regions.empty() || regions.size() > 48)
      return fail("regions doit contenir entre 1 et 48 zones.");
    std::set<std::string> names;
    for (auto const& value : regions)
    {
      ProceduralScatterRegion region;
      if (!exactFields(value, region_fields) || !value.at("name").is_string()
          || !value.at("role").is_string()
          || !value.at("density_per_tile").is_number_integer()
          || !finiteFloat(value.at("min_spacing_ratio"), region.min_spacing_ratio)
          || !finiteFloat(value.at("min_height"), region.min_height)
          || !finiteFloat(value.at("max_height"), region.max_height)
          || !finiteFloat(value.at("min_slope_degrees"), region.min_slope_degrees)
          || !finiteFloat(value.at("max_slope_degrees"), region.max_slope_degrees)
          || !finiteFloat(value.at("cluster_scale"), region.cluster_scale)
          || !finiteFloat(value.at("cluster_strength"), region.cluster_strength))
        return fail("Chaque région contient des paramètres invalides.");
      region.name = value.at("name").get<std::string>();
      region.role = value.at("role").get<std::string>();
      auto const density = value.at("density_per_tile").get<std::int64_t>();
      if (region.name.empty() || region.name.size() > 64 || !printableAscii(region.name)
          || !names.insert(region.name).second || density < 1 || density > 512
          || region.min_spacing_ratio < 0.00025f || region.min_spacing_ratio > 0.25f
          || region.min_height < -500.0f || region.max_height > 5000.0f
          || region.min_height > region.max_height
          || region.min_slope_degrees < 0.0f || region.max_slope_degrees > 90.0f
          || region.min_slope_degrees > region.max_slope_degrees
          || !validRole(region.role)
          || region.cluster_scale < 0.5f || region.cluster_scale > 16.0f
          || region.cluster_strength < 0.0f || region.cluster_strength > 1.0f
          || std::none_of(scatter.assets.begin(), scatter.assets.end(), [&](auto const& asset)
             { return asset.role == region.role; }))
        return fail("Nom, densité, espacement, hauteur ou pente de région invalide.");
      region.density_per_tile = static_cast<std::size_t>(density);
      // Wall chains are open polylines walked end to end, not closed areas.
      auto const open_chain = region.role == "wall"
        && region.name.find("_chain") != std::string::npos;
      if (open_chain)
      {
        if (!parsePoints(value.at("points"), region.points, 2))
          return fail("Chaque chaîne de mur doit être une polyligne de 2 à 16 points.");
      }
      else if (!parsePoints(value.at("points"), region.points, 3)
          || !isSimpleProceduralArea(region.points))
        return fail("Chaque région doit être un polygone simple de 3 à 16 points.");
      scatter.regions.push_back(std::move(region));
    }

    auto const& exclusions = arguments.at("exclusions");
    if (!exclusions.is_array() || exclusions.size() > 96)
      return fail("exclusions doit contenir au plus 96 formes.");
    for (auto const& value : exclusions)
    {
      ProceduralScatterExclusion exclusion;
      if (!exactFields(value, exclusion_fields) || !value.at("shape").is_string()
          || !finiteFloat(value.at("half_width_ratio"), exclusion.half_width_ratio)
          || exclusion.half_width_ratio < 0.00025f
          || exclusion.half_width_ratio > 0.25f)
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
    auto const& region = scatter.regions[region_index];
    auto const is_wall = proceduralScatterIsWallRegion(region);
    if (is_wall)
    {
      auto const sample = wallSample(
        region.points, candidate_index, region.density_per_tile,
        region.name.ends_with("_path_wall"),
        region.name.find("_chain") != std::string::npos);
      candidate.u = sample.u;
      candidate.v = sample.v;
      candidate.yaw_degrees = sample.yaw_degrees;
      if (candidate.u < u_min || candidate.u >= u_max
          || candidate.v < v_min || candidate.v >= v_max)
      {
        candidate.active = false;
        return candidate;
      }
    }
    auto region_u_min = 1.0f;
    auto region_u_max = 0.0f;
    auto region_v_min = 1.0f;
    auto region_v_max = 0.0f;
    for (auto const& point : region.points)
    {
      region_u_min = std::min(region_u_min, point.u);
      region_u_max = std::max(region_u_max, point.u);
      region_v_min = std::min(region_v_min, point.v);
      region_v_max = std::max(region_v_max, point.v);
    }
    if (!is_wall)
    {
      u_min = std::max(u_min, region_u_min);
      u_max = std::min(u_max, region_u_max);
      v_min = std::max(v_min, region_v_min);
      v_max = std::min(v_max, region_v_max);
      if (u_min >= u_max || v_min >= v_max)
      {
        candidate.active = false;
        return candidate;
      }
      candidate.u = u_min + unit(hash(scatter.seed, {base, 1})) * (u_max - u_min);
      candidate.v = v_min + unit(hash(scatter.seed, {base, 2})) * (v_max - v_min);
    }
    auto const massif = smoothNoise(scatter.seed, candidate.u, candidate.v,
                                    region.cluster_scale, region_index + 101);
    auto const clustered_density = std::clamp((massif - 0.25f) / 0.55f, 0.0f, 1.0f);
    auto const probability = std::lerp(1.0f, clustered_density, region.cluster_strength);
    candidate.active = unit(hash(scatter.seed, {base, 3})) <= probability;

    auto best_score = -1.0f;
    for (std::size_t index = 0; index < scatter.assets.size(); ++index)
    {
      auto const& asset = scatter.assets[index];
      if (asset.role != region.role) continue;
      auto const patch = smoothNoise(scatter.seed, candidate.u, candidate.v,
                                     region.cluster_scale * 1.8f, 1000 + index * 37);
      auto const local_variation = unit(hash(scatter.seed, {base, 2000 + index}));
      auto const score = (is_wall ? local_variation
                                  : patch * 0.8f + local_variation * 0.2f)
        + std::log(asset.weight) * 0.05f;
      if (score > best_score)
      {
        best_score = score;
        candidate.asset_index = index;
      }
    }
    auto const& asset = scatter.assets[candidate.asset_index];
    candidate.scale = asset.min_scale
      + unit(hash(scatter.seed, {base, 4})) * (asset.max_scale - asset.min_scale);
    if (!is_wall)
      candidate.yaw_degrees = unit(hash(scatter.seed, {base, 5})) * 360.0f;
    return candidate;
  }

  bool proceduralScatterIsWallRegion(ProceduralScatterRegion const& region)
  {
    return region.role == "wall" || region.name.ends_with("_wall");
  }

  bool proceduralScatterContains(
    std::vector<ProceduralLayoutPoint> const& points, float u, float v)
  {
    return distanceToProceduralShape(
      points, ProceduralLayoutShape::Area, u, v, 1.0f, 1.0f).distance <= 0.0f;
  }

  bool proceduralScatterRegionIntersects(
    ProceduralScatterRegion const& region,
    float u_min, float u_max, float v_min, float v_max)
  {
    auto region_u_min = 1.0f;
    auto region_u_max = 0.0f;
    auto region_v_min = 1.0f;
    auto region_v_max = 0.0f;
    for (auto const& point : region.points)
    {
      region_u_min = std::min(region_u_min, point.u);
      region_u_max = std::max(region_u_max, point.u);
      region_v_min = std::min(region_v_min, point.v);
      region_v_max = std::max(region_v_max, point.v);
    }
    return region_u_max >= u_min && region_u_min <= u_max
        && region_v_max >= v_min && region_v_min <= v_max;
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
