// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/MobaArenaAudit.hpp>
#include <noggit/ai/MobaArenaBlueprint.hpp>
#include <noggit/ai/ProceduralLayout.hpp>
#include <noggit/ai/ProceduralLiquidLayout.hpp>
#include <noggit/ai/ProceduralProps.hpp>
#include <noggit/ai/ProceduralScatter.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace Noggit::Ai
{
  namespace
  {
    constexpr float tile_size = 1600.0f / 3.0f;
    constexpr float unitsize = tile_size / 128.0f;
    constexpr float walkable_slope_degrees = 40.0f;
    constexpr float marginal_slope_degrees = 50.0f;
    constexpr float maximum_floor_rise = 5.0f;
    constexpr float radians_to_degrees = 57.29577951308232f;

    enum class Semantic : std::uint8_t
    {
      Ground,
      Lane,
      River,
      JunglePath,
      Camp,
      Objective,
      Base,
      Wall
    };

    struct Camp
    {
      std::string name;
      std::string quadrant;
      std::string kind;
      std::string feature;
      float u = 0.0f;
      float v = 0.0f;
      float radius = 0.0f;
    };

    struct Route
    {
      std::string name;
      std::string quadrant;
      std::string role;
      std::vector<std::string> doors;
      ProceduralLayoutFeature const* feature = nullptr;
    };

    struct Door
    {
      float u = 0.0f;
      float v = 0.0f;
      std::string boundary;
      std::string route;
    };

    struct PointOfInterest
    {
      std::string name;
      float u = 0.0f;
      float v = 0.0f;
      ProceduralLayoutFeature const* feature = nullptr;
    };

    void addIssue(MobaArenaAuditReport& report, std::string code,
                  std::string message, std::string subject = {})
    {
      report.issues.push_back(
        {std::move(code), std::move(subject), std::move(message)});
    }

    std::optional<std::reference_wrapper<nlohmann::json const>> findCall(
      nlohmann::json const& blueprint, std::string_view name)
    {
      if (!blueprint.contains("next_calls") || !blueprint.at("next_calls").is_array())
        return std::nullopt;
      for (auto const& call : blueprint.at("next_calls"))
        if (call.is_object() && call.value("name", std::string{}) == name
            && call.contains("arguments"))
          return std::cref(call.at("arguments"));
      return std::nullopt;
    }

    std::vector<std::reference_wrapper<nlohmann::json const>> findCalls(
      nlohmann::json const& blueprint, std::string_view name)
    {
      auto result = std::vector<std::reference_wrapper<nlohmann::json const>>{};
      if (!blueprint.contains("next_calls") || !blueprint.at("next_calls").is_array())
        return result;
      for (auto const& call : blueprint.at("next_calls"))
        if (call.is_object() && call.value("name", std::string{}) == name
            && call.contains("arguments"))
          result.push_back(std::cref(call.at("arguments")));
      return result;
    }

    ProceduralLayoutFeature const* findFeature(
      ProceduralLayout const& layout, std::string_view name)
    {
      auto const found = std::find_if(layout.features.begin(), layout.features.end(),
        [&](auto const& feature) { return feature.name == name; });
      return found == layout.features.end() ? nullptr : &*found;
    }

    std::pair<float, float> centroid(ProceduralLayoutFeature const& feature)
    {
      auto u = 0.0f;
      auto v = 0.0f;
      for (auto const& point : feature.points)
      {
        u += point.u;
        v += point.v;
      }
      auto const count = static_cast<float>(feature.points.size());
      return {u / count, v / count};
    }

    float percentile(std::vector<float> values, float ratio)
    {
      if (values.empty()) return 0.0f;
      std::sort(values.begin(), values.end());
      auto const index = static_cast<std::size_t>(std::clamp(ratio, 0.0f, 1.0f)
        * static_cast<float>(values.size() - 1));
      return values[index];
    }

    std::vector<float> squaredEuclideanDistance(
      std::vector<std::uint8_t> const& sources,
      std::size_t width,
      std::size_t height)
    {
      constexpr auto far = 1.0e12f;
      auto transform = [](std::vector<float> const& input)
      {
        auto const size = input.size();
        auto output = std::vector<float>(size);
        auto locations = std::vector<std::size_t>(size);
        auto boundaries = std::vector<float>(size + 1);
        auto last = std::size_t{0};
        locations[0] = 0;
        boundaries[0] = -std::numeric_limits<float>::infinity();
        boundaries[1] = std::numeric_limits<float>::infinity();
        for (std::size_t q = 1; q < size; ++q)
        {
          auto separation = 0.0f;
          while (true)
          {
            auto const location = locations[last];
            separation = ((input[q] + static_cast<float>(q * q))
              - (input[location] + static_cast<float>(location * location)))
              / static_cast<float>(2 * q - 2 * location);
            if (separation > boundaries[last] || last == 0) break;
            --last;
          }
          if (separation <= boundaries[last] && last == 0) separation = boundaries[0];
          ++last;
          locations[last] = q;
          boundaries[last] = separation;
          boundaries[last + 1] = std::numeric_limits<float>::infinity();
        }
        last = 0;
        for (std::size_t q = 0; q < size; ++q)
        {
          while (boundaries[last + 1] < static_cast<float>(q)) ++last;
          auto const delta = static_cast<float>(q)
            - static_cast<float>(locations[last]);
          output[q] = delta * delta + input[locations[last]];
        }
        return output;
      };

      auto horizontal = std::vector<float>(width * height);
      for (std::size_t y = 0; y < height; ++y)
      {
        auto row = std::vector<float>(width);
        for (std::size_t x = 0; x < width; ++x)
          row[x] = sources[y * width + x] ? 0.0f : far;
        auto const distances = transform(row);
        std::copy(distances.begin(), distances.end(),
                  horizontal.begin() + static_cast<std::ptrdiff_t>(y * width));
      }
      auto result = std::vector<float>(width * height);
      for (std::size_t x = 0; x < width; ++x)
      {
        auto column = std::vector<float>(height);
        for (std::size_t y = 0; y < height; ++y)
          column[y] = horizontal[y * width + x];
        auto const distances = transform(column);
        for (std::size_t y = 0; y < height; ++y)
          result[y * width + x] = distances[y];
      }
      return result;
    }

    float angleDegrees(std::pair<float, float> center,
                       std::pair<float, float> first,
                       std::pair<float, float> second)
    {
      auto const ax = first.first - center.first;
      auto const ay = first.second - center.second;
      auto const bx = second.first - center.first;
      auto const by = second.second - center.second;
      auto const denominator = std::hypot(ax, ay) * std::hypot(bx, by);
      return denominator > 0.0f
        ? std::acos(std::clamp((ax * bx + ay * by) / denominator,
                               -1.0f, 1.0f))
            * radians_to_degrees
        : 0.0f;
    }

    struct FeatureMirrorError
    {
      float position = std::numeric_limits<float>::infinity();
      float height = std::numeric_limits<float>::infinity();
      bool attributes_match = false;
    };

    FeatureMirrorError mirroredFeatureError(ProceduralLayoutFeature const& first,
                                            ProceduralLayoutFeature const& second)
    {
      if (first.shape != second.shape
          || first.height_mode != second.height_mode
          || first.points.size() != second.points.size()
          || first.texture_layer != second.texture_layer
          || first.priority != second.priority
          || std::abs(first.half_width_ratio - second.half_width_ratio) > .000001f
          || std::abs(first.transition_width_ratio
                      - second.transition_width_ratio) > .000001f
          || std::abs(first.roughness_amplitude
                      - second.roughness_amplitude) > .000001f
          || std::abs(first.texture_strength - second.texture_strength) > .000001f
          || std::abs(first.width_variation_ratio
                      - second.width_variation_ratio) > .000001f)
        return {};
      auto best = FeatureMirrorError{};
      auto best_score = std::numeric_limits<float>::infinity();
      auto const count = first.points.size();
      for (std::size_t offset = 0; offset < count; ++offset)
        for (auto const direction : {1, -1})
        {
          if (first.shape == ProceduralLayoutShape::Corridor
              && !((direction == 1 && offset == 0)
                   || (direction == -1 && offset + 1 == count)))
            continue;
          auto position_error = 0.0f;
          auto height_error = 0.0f;
          for (std::size_t i = 0; i < count; ++i)
          {
            auto const& a = first.points[i];
            auto const signed_index = static_cast<long long>(offset)
              + static_cast<long long>(direction) * static_cast<long long>(i);
            auto const wrapped = static_cast<std::size_t>(
              (signed_index % static_cast<long long>(count)
               + static_cast<long long>(count)) % static_cast<long long>(count));
            auto const& b = second.points[wrapped];
            position_error = std::max(position_error,
              std::hypot((1.0f - a.u) - b.u, (1.0f - a.v) - b.v));
            height_error = std::max(height_error, std::abs(a.height - b.height));
          }
          auto const score = std::max(position_error / .00002f,
                                      height_error / .0001f);
          if (score < best_score)
          {
            best_score = score;
            best = {position_error, height_error, true};
          }
        }
      return best;
    }

    bool mirrorErrorAcceptable(FeatureMirrorError const& error)
    {
      return error.attributes_match
        && error.position <= .00002f && error.height <= .0001f;
    }

    float mirrorErrorMagnitude(FeatureMirrorError const& error)
    {
      if (!error.attributes_match) return std::numeric_limits<float>::infinity();
      return std::max(error.position, error.height);
    }

    std::string mirrorErrorDescription(FeatureMirrorError const& error)
    {
      if (!error.attributes_match) return "attributs différents";
      return "écart_xy=" + std::to_string(error.position)
        + ", écart_hauteur=" + std::to_string(error.height);
    }

    bool nearFeature(ProceduralLayoutFeature const& feature,
                     float u, float v, float padding)
    {
      auto const reach = feature.half_width_ratio
        * (1.0f + feature.width_variation_ratio)
        + feature.transition_width_ratio + padding;
      if (u < feature.min_u - reach || u > feature.max_u + reach
          || v < feature.min_v - reach || v > feature.max_v + reach)
        return false;
      return distanceToProceduralShape(
        feature.points, feature.shape, u, v, 1.0f, 1.0f).distance <= reach;
    }

    Semantic semanticForName(std::string const& name, bool walkable)
    {
      if (!walkable) return Semantic::Wall;
      if (name.ends_with("_lane")) return Semantic::Lane;
      if (name == "river_bed") return Semantic::River;
      if (name.ends_with("_camp_floor")) return Semantic::Camp;
      if (name.starts_with("objective_")) return Semantic::Objective;
      if (name.ends_with("_inner_court") || name.ends_with("_base_apron"))
        return Semantic::Base;
      if ((name.starts_with("jungle_") && name.ends_with("_path"))
          || name.ends_with("_wall_cut"))
        return Semantic::JunglePath;
      return Semantic::Ground;
    }

    std::array<std::uint8_t, 3> semanticColor(Semantic semantic)
    {
      switch (semantic)
      {
        case Semantic::Lane: return {145, 112, 76};
        case Semantic::River: return {55, 150, 190};
        case Semantic::JunglePath: return {72, 128, 62};
        case Semantic::Camp: return {156, 156, 70};
        case Semantic::Objective: return {208, 104, 38};
        case Semantic::Base: return {166, 145, 118};
        case Semantic::Wall: return {20, 35, 25};
        case Semantic::Ground: return {48, 82, 43};
      }
      return {255, 0, 255};
    }

    std::string joined(std::vector<std::string> const& values)
    {
      std::ostringstream stream;
      for (std::size_t i = 0; i < values.size(); ++i)
      {
        if (i) stream << ", ";
        stream << values[i];
      }
      return stream.str();
    }

    std::string lowerAscii(std::string value)
    {
      std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character)
        { return static_cast<char>(std::tolower(character)); });
      return value;
    }
  }

  MobaArenaAuditReport auditMobaArenaBlueprint(
    nlohmann::json const& blueprint,
    std::size_t footprint_side_tiles,
    std::size_t preview_resolution,
    std::size_t min_tile_x,
    std::size_t min_tile_z)
  {
    if (blueprint.contains("arena_fit"))
      return auditMobaArenaBlueprint(
        canonicalMobaArenaBlueprint(blueprint), footprint_side_tiles,
        preview_resolution, min_tile_x, min_tile_z);

    MobaArenaAuditReport report;
    report.preview_resolution = preview_resolution;
    report.preview_width = preview_resolution * 2;
    report.preview_height = preview_resolution;
    if (preview_resolution < 64 || preview_resolution > 1024)
    {
      addIssue(report, "audit.preview_resolution",
               "la résolution doit être comprise entre 64 et 1024");
      return report;
    }
    if (footprint_side_tiles < 2 || footprint_side_tiles > 4)
    {
      addIssue(report, "audit.footprint", "le côté doit être compris entre 2 et 4 tuiles");
      return report;
    }
    if (min_tile_x > 64 - footprint_side_tiles
        || min_tile_z > 64 - footprint_side_tiles)
    {
      addIssue(report, "audit.tile_origin",
               "l'empreinte doit rester dans la grille de tuiles [0,63]");
      return report;
    }

    auto const terrain_call = findCall(blueprint, "apply_terrain_layout_on_map");
    if (!terrain_call)
    {
      addIssue(report, "audit.terrain", "appel terrain absent");
      return report;
    }
    auto parsed = parseProceduralLayout(terrain_call->get());
    if (!parsed.layout)
    {
      addIssue(report, "audit.terrain", parsed.error);
      return report;
    }
    auto const& layout = *parsed.layout;
    if (layout.texture_paths.empty())
    {
      addIssue(report, "texture.jungle_identity", "texture de jungle absente");
    }
    else
    {
      auto const path = lowerAscii(layout.texture_paths.front());
      if (path.find("grass") == std::string::npos
          && path.find("moss") == std::string::npos
          && path.find("green") == std::string::npos
          && path.find("leaf") == std::string::npos
          && path.find("lichen") == std::string::npos)
        addIssue(report, "texture.jungle_identity",
                 "la couche 0 doit désigner explicitement une texture végétale verte",
                 layout.texture_paths.front());
    }
    auto liquid_layout = std::optional<ProceduralLiquidLayout>{};
    auto const liquid_calls = findCalls(blueprint, "apply_liquid_layout_on_map");
    if (liquid_calls.size() != 1)
      addIssue(report, "audit.liquid",
               "un seul appel apply_liquid_layout_on_map est obligatoire");
    else
    {
      auto liquid = parseProceduralLiquidLayout(liquid_calls.front().get());
      if (!liquid.layout)
        addIssue(report, "audit.liquid", liquid.error);
      else
        liquid_layout = std::move(*liquid.layout);
    }
    auto const scatter_calls = findCalls(blueprint, "scatter_assets_on_map");
    std::optional<ProceduralScatter> wall_scatter;
    std::optional<ProceduralScatter> vegetation_scatter;
    if (scatter_calls.size() != 2)
      addIssue(report, "scatter.calls", "deux appels scatter sont obligatoires");
    else
    {
      auto walls = parseProceduralScatter(scatter_calls[0].get());
      auto vegetation = parseProceduralScatter(scatter_calls[1].get());
      if (!walls.scatter)
        addIssue(report, "scatter.walls", walls.error);
      else
        wall_scatter = std::move(*walls.scatter);
      if (!vegetation.scatter)
        addIssue(report, "scatter.vegetation", vegetation.error);
      else
        vegetation_scatter = std::move(*vegetation.scatter);
    }
    auto procedural_props = std::optional<ProceduralProps>{};
    auto const props_calls = findCalls(blueprint, "place_props_on_map");
    if (props_calls.size() != 1)
      addIssue(report, "props.call", "un seul appel place_props_on_map est obligatoire");
    else
    {
      auto props = parseProceduralProps(props_calls.front().get());
      if (!props.props)
        addIssue(report, "props.call", props.error);
      else
        procedural_props = std::move(*props.props);
    }
    auto scatter_call_indices = std::vector<std::size_t>{};
    auto props_call_indices = std::vector<std::size_t>{};
    if (blueprint.contains("next_calls") && blueprint.at("next_calls").is_array())
      for (std::size_t index = 0; index < blueprint.at("next_calls").size(); ++index)
      {
        auto const& call = blueprint.at("next_calls").at(index);
        if (!call.is_object() || !call.contains("name")
            || !call.at("name").is_string())
          continue;
        auto const& name = call.at("name").get_ref<std::string const&>();
        if (name == "scatter_assets_on_map") scatter_call_indices.push_back(index);
        if (name == "place_props_on_map") props_call_indices.push_back(index);
      }
    if (scatter_call_indices.size() == 2 && props_call_indices.size() == 1
        && !(scatter_call_indices[0] < props_call_indices[0]
             && props_call_indices[0] < scatter_call_indices[1]))
      addIssue(report, "scatter.execution_order",
               "l'ordre obligatoire est murs, props, végétation");
    auto const* arena = findFeature(layout, "arena_ground");
    if (!arena || arena->points.empty())
    {
      addIssue(report, "audit.arena_ground", "feature arena_ground absente");
      return report;
    }
    auto const base_height = arena->points.front().height;
    auto const world_size = static_cast<float>(footprint_side_tiles) * tile_size;
    auto const cells = footprint_side_tiles * std::size_t{128};
    auto const triangle_count = cells * cells * 4;

    auto const cornerIndex = [cells](std::size_t x, std::size_t z)
    {
      return z * (cells + 1) + x;
    };
    auto const centerIndex = [cells](std::size_t x, std::size_t z)
    {
      return z * cells + x;
    };
    auto const triangleIndex = [cells](std::size_t x, std::size_t z, int side)
    {
      return (z * cells + x) * 4 + static_cast<std::size_t>(side);
    };
    auto const triangleCell = [cells](std::size_t index)
    {
      auto const cell = index / 4;
      return std::array<std::size_t, 3>{cell % cells, cell / cells, index % 4};
    };
    auto const triangleCenter = [cells, &triangleCell](std::size_t index)
    {
      auto const cell = triangleCell(index);
      static constexpr auto offsets = std::array<std::array<float, 2>, 4>{
        std::array<float, 2>{.5f, 1.0f / 6.0f},
        std::array<float, 2>{5.0f / 6.0f, .5f},
        std::array<float, 2>{.5f, 5.0f / 6.0f},
        std::array<float, 2>{1.0f / 6.0f, .5f}};
      return std::pair{
        (static_cast<float>(cell[0]) + offsets[cell[2]][0]) / static_cast<float>(cells),
        (static_cast<float>(cell[1]) + offsets[cell[2]][1]) / static_cast<float>(cells)};
    };
    auto forNeighbors = [cells, &triangleCell, &triangleIndex](
      std::size_t index, auto&& visitor)
    {
      auto const cell = triangleCell(index);
      auto const x = cell[0];
      auto const z = cell[1];
      auto const side = static_cast<int>(cell[2]);
      visitor(triangleIndex(x, z, (side + 3) % 4));
      visitor(triangleIndex(x, z, (side + 1) % 4));
      if (side == 0 && z > 0) visitor(triangleIndex(x, z - 1, 2));
      if (side == 1 && x + 1 < cells) visitor(triangleIndex(x + 1, z, 3));
      if (side == 2 && z + 1 < cells) visitor(triangleIndex(x, z + 1, 0));
      if (side == 3 && x > 0) visitor(triangleIndex(x - 1, z, 1));
    };

    auto const sampleHeight = [&](float u, float v)
    {
      return sampleSmoothedProceduralLayoutHeight(
        layout, u, v, base_height, world_size, world_size, unitsize * .5f);
    };
    std::vector<float> corner_heights((cells + 1) * (cells + 1));
    std::vector<float> center_heights(cells * cells);
    for (std::size_t z = 0; z <= cells; ++z)
      for (std::size_t x = 0; x <= cells; ++x)
        corner_heights[cornerIndex(x, z)] = sampleHeight(
          static_cast<float>(x) / static_cast<float>(cells),
          static_cast<float>(z) / static_cast<float>(cells));
    for (std::size_t z = 0; z < cells; ++z)
      for (std::size_t x = 0; x < cells; ++x)
        center_heights[centerIndex(x, z)] = sampleHeight(
          (static_cast<float>(x) + .5f) / static_cast<float>(cells),
          (static_cast<float>(z) + .5f) / static_cast<float>(cells));

    std::vector<float> slopes(triangle_count);
    std::vector<std::uint8_t> raw_walkable(triangle_count);
    auto marginal_triangles = std::size_t{0};
    auto triangleSlope = [](std::array<float, 3> const& p0,
                            std::array<float, 3> const& p1,
                            std::array<float, 3> const& p2)
    {
      auto const ax = p1[0] - p0[0];
      auto const ay = p1[1] - p0[1];
      auto const az = p1[2] - p0[2];
      auto const bx = p2[0] - p0[0];
      auto const by = p2[1] - p0[1];
      auto const bz = p2[2] - p0[2];
      auto const nx = ay * bz - az * by;
      auto const ny = az * bx - ax * bz;
      auto const nz = ax * by - ay * bx;
      auto const length = std::sqrt(nx * nx + ny * ny + nz * nz);
      auto const up = length > 0.0f
        ? std::clamp(std::abs(ny) / length, 0.0f, 1.0f) : 1.0f;
      return std::acos(up) * radians_to_degrees;
    };
    for (std::size_t z = 0; z < cells; ++z)
      for (std::size_t x = 0; x < cells; ++x)
      {
        auto const center = std::array<float, 3>{
          (static_cast<float>(x) + .5f) * unitsize,
          center_heights[centerIndex(x, z)],
          (static_cast<float>(z) + .5f) * unitsize};
        auto const tl = std::array<float, 3>{static_cast<float>(x) * unitsize,
          corner_heights[cornerIndex(x, z)], static_cast<float>(z) * unitsize};
        auto const tr = std::array<float, 3>{static_cast<float>(x + 1) * unitsize,
          corner_heights[cornerIndex(x + 1, z)], static_cast<float>(z) * unitsize};
        auto const bl = std::array<float, 3>{static_cast<float>(x) * unitsize,
          corner_heights[cornerIndex(x, z + 1)], static_cast<float>(z + 1) * unitsize};
        auto const br = std::array<float, 3>{static_cast<float>(x + 1) * unitsize,
          corner_heights[cornerIndex(x + 1, z + 1)], static_cast<float>(z + 1) * unitsize};
        auto const points = std::array{
          std::pair{tr, tl}, std::pair{br, tr},
          std::pair{bl, br}, std::pair{tl, bl}};
        for (int side = 0; side < 4; ++side)
        {
          auto const index = triangleIndex(x, z, side);
          auto const slope = triangleSlope(center, points[side].first, points[side].second);
          auto const maximum_height = std::max({center[1], points[side].first[1],
                                                points[side].second[1]});
          slopes[index] = slope;
          raw_walkable[index] = slope <= walkable_slope_degrees
            && maximum_height <= base_height + maximum_floor_rise;
          marginal_triangles += slope > walkable_slope_degrees
            && slope <= marginal_slope_degrees
            && maximum_height <= base_height + maximum_floor_rise;
        }
      }

    std::vector<std::uint8_t> walkable = raw_walkable;
    for (std::size_t index = 0; index < triangle_count; ++index)
    {
      if (!raw_walkable[index]) continue;
      auto touches_blocked = false;
      forNeighbors(index, [&](std::size_t neighbor)
      {
        touches_blocked = touches_blocked || !raw_walkable[neighbor];
      });
      if (touches_blocked) walkable[index] = 0;
    }

    auto const triangleAt = [cells, &triangleIndex](float u, float v)
    {
      auto const scaled_x = std::clamp(u, 0.0f, .999999f) * static_cast<float>(cells);
      auto const scaled_z = std::clamp(v, 0.0f, .999999f) * static_cast<float>(cells);
      auto const x = static_cast<std::size_t>(scaled_x);
      auto const z = static_cast<std::size_t>(scaled_z);
      auto const local_u = scaled_x - static_cast<float>(x);
      auto const local_v = scaled_z - static_cast<float>(z);
      auto side = 3;
      if (local_v <= local_u && local_v <= 1.0f - local_u) side = 0;
      else if (local_u >= local_v && local_u >= 1.0f - local_v) side = 1;
      else if (local_v >= local_u && local_v >= 1.0f - local_u) side = 2;
      return triangleIndex(x, z, side);
    };
    auto liquid_metrics = nlohmann::json::object();
    auto const* river_bed_feature = findFeature(layout, "river_bed");
    auto const* central_river_feature
      = static_cast<ProceduralLiquidLayoutFeature const*>(nullptr);
    auto central_river_index = procedural_liquid_no_feature;
    auto central_river_count = std::size_t{0};
    if (liquid_layout)
      for (std::size_t index = 0; index < liquid_layout->features.size(); ++index)
      {
        auto const& feature = liquid_layout->features[index];
        if (feature.name == "central_river")
        {
          central_river_feature = &feature;
          central_river_index = index;
          ++central_river_count;
        }
      }
    auto points_aligned = central_river_feature && river_bed_feature
      && central_river_feature->points.size() == river_bed_feature->points.size();
    if (points_aligned)
      for (std::size_t index = 0;
           index < central_river_feature->points.size(); ++index)
      {
        auto const& water = central_river_feature->points[index];
        auto const& bed = river_bed_feature->points[index];
        points_aligned = points_aligned
          && std::hypot(water.u - bed.u, water.v - bed.v) <= .000001f;
      }
    auto const river_aligned = liquid_layout && liquid_layout->replace_existing
      && liquid_layout->features.size() == 1 && central_river_count == 1
      && river_bed_feature && points_aligned
      && central_river_feature->shape == ProceduralLayoutShape::Corridor
      && river_bed_feature->shape == ProceduralLayoutShape::Corridor
      && std::abs(central_river_feature->half_width_ratio
                  - river_bed_feature->half_width_ratio) <= .000001f
      && central_river_feature->half_width_ratio
           + central_river_feature->transition_width_ratio
           <= river_bed_feature->half_width_ratio
              + river_bed_feature->transition_width_ratio + .000001f;
    if (!river_aligned)
      addIssue(report, "liquid.river_alignment",
               "replace_existing=true et une unique central_river alignée sur river_bed sont obligatoires");

    auto liquid_cells = std::vector<std::uint8_t>(cells * cells);
    auto active_liquid_cells = std::size_t{0};
    auto minimum_water_column = std::numeric_limits<float>::infinity();
    auto maximum_water_column = -std::numeric_limits<float>::infinity();
    auto minimum_liquid_depth = std::numeric_limits<float>::infinity();
    auto maximum_liquid_depth = -std::numeric_limits<float>::infinity();
    auto water_column_wadeable = river_aligned;
    static constexpr auto liquid_corner_offsets
      = std::array<std::array<std::size_t, 2>, 4>{
          std::array<std::size_t, 2>{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    if (liquid_layout && liquid_layout->replace_existing)
      for (std::size_t cell_z = 0; cell_z < cells; ++cell_z)
        for (std::size_t cell_x = 0; cell_x < cells; ++cell_x)
        {
          auto const center_u = (static_cast<float>(cell_x) + .5f)
            / static_cast<float>(cells);
          auto const center_v = (static_cast<float>(cell_z) + .5f)
            / static_cast<float>(cells);
          auto const center = sampleProceduralLiquidLayout(
            *liquid_layout, center_u, center_v, world_size, world_size);
          if (!center.has_liquid) continue;

          auto cell_has_visible_column = false;
          auto cell_wadeable = center.feature_index == central_river_index
            && center.depth > 0.0f && center.depth <= 1.0f;
          auto cell_minimum_column = std::numeric_limits<float>::infinity();
          auto cell_maximum_column = -std::numeric_limits<float>::infinity();
          auto cell_minimum_depth = center.depth;
          auto cell_maximum_depth = center.depth;
          auto const center_column = center.height
            - center_heights[centerIndex(cell_x, cell_z)];
          if (center_column > 0.0f)
          {
            cell_has_visible_column = true;
            cell_minimum_column = center_column;
            cell_maximum_column = center_column;
            cell_wadeable = cell_wadeable && center_column <= 1.01f;
          }
          auto active = false;
          for (auto const& offset : liquid_corner_offsets)
          {
            auto const corner_x = cell_x + offset[0];
            auto const corner_z = cell_z + offset[1];
            auto const corner = sampleProceduralLiquidFeature(
              *liquid_layout, center.feature_index,
              static_cast<float>(corner_x) / static_cast<float>(cells),
              static_cast<float>(corner_z) / static_cast<float>(cells),
              world_size, world_size);
            auto const column = corner.height
              - corner_heights[cornerIndex(corner_x, corner_z)];
            if (column >= 0.0f)
            {
              active = true;
              cell_has_visible_column = cell_has_visible_column || column > 0.0f;
              cell_minimum_column = std::min(cell_minimum_column, column);
              cell_maximum_column = std::max(cell_maximum_column, column);
              cell_wadeable = cell_wadeable && column <= 1.01f;
            }
            cell_minimum_depth = std::min(cell_minimum_depth, corner.depth);
            cell_maximum_depth = std::max(cell_maximum_depth, corner.depth);
            cell_wadeable = cell_wadeable
              && corner.depth >= 0.0f && corner.depth <= 1.0f;
          }
          // ChunkWater::applyCellUpdates crops a cell only when all four
          // liquid vertices are strictly below terrain.
          if (!active) continue;
          liquid_cells[centerIndex(cell_x, cell_z)] = 1;
          ++active_liquid_cells;
          minimum_water_column = std::min(
            minimum_water_column, cell_minimum_column);
          maximum_water_column = std::max(
            maximum_water_column, cell_maximum_column);
          minimum_liquid_depth = std::min(
            minimum_liquid_depth, cell_minimum_depth);
          maximum_liquid_depth = std::max(
            maximum_liquid_depth, cell_maximum_depth);
          water_column_wadeable = water_column_wadeable
            && cell_has_visible_column && cell_wadeable;
        }

    auto endpoint_cells_active = false;
    auto river_connected = false;
    auto connected_liquid_cells = std::size_t{0};
    if (river_bed_feature && river_bed_feature->points.size() >= 2)
    {
      auto const cellAt = [cells, &centerIndex](ProceduralLayoutPoint const& point)
      {
        auto const x = static_cast<std::size_t>(
          std::clamp(point.u, 0.0f, .999999f) * static_cast<float>(cells));
        auto const z = static_cast<std::size_t>(
          std::clamp(point.v, 0.0f, .999999f) * static_cast<float>(cells));
        return centerIndex(x, z);
      };
      auto const first = cellAt(river_bed_feature->points.front());
      auto const last = cellAt(river_bed_feature->points.back());
      endpoint_cells_active = liquid_cells[first] && liquid_cells[last];
      if (endpoint_cells_active)
      {
        auto reached = std::vector<std::uint8_t>(cells * cells);
        auto queue = std::deque<std::size_t>{first};
        reached[first] = 1;
        while (!queue.empty())
        {
          auto const current = queue.front();
          queue.pop_front();
          ++connected_liquid_cells;
          auto const x = current % cells;
          auto const z = current / cells;
          auto visit = [&](std::size_t neighbor)
          {
            if (liquid_cells[neighbor] && !reached[neighbor])
            {
              reached[neighbor] = 1;
              queue.push_back(neighbor);
            }
          };
          if (x > 0) visit(current - 1);
          if (x + 1 < cells) visit(current + 1);
          if (z > 0) visit(current - cells);
          if (z + 1 < cells) visit(current + cells);
        }
        river_connected = reached[last]
          && connected_liquid_cells == active_liquid_cells;
      }
    }
    if (!active_liquid_cells)
      addIssue(report, "liquid.river_alignment", "couverture MH2O vide");
    if (!river_connected)
      addIssue(report, "liquid.river_continuity",
               "les cellules MH2O doivent former une surface continue entre les deux extrémités du lit: extrémités="
                 + std::to_string(endpoint_cells_active) + ", composante="
                 + std::to_string(connected_liquid_cells) + "/"
                 + std::to_string(active_liquid_cells));
    if (!water_column_wadeable || !active_liquid_cells)
      addIssue(report, "liquid.wadeable",
               "chaque cellule MH2O active doit avoir une profondeur valide et une colonne d'eau dans ]0,1.01]");

    auto const liquidCellOccupied = [&](float u, float v)
    {
      auto const x = static_cast<std::size_t>(
        std::clamp(u, 0.0f, .999999f) * static_cast<float>(cells));
      auto const z = static_cast<std::size_t>(
        std::clamp(v, 0.0f, .999999f) * static_cast<float>(cells));
      return liquid_cells[centerIndex(x, z)] != 0;
    };
    liquid_metrics = {
      {"active_mh2o_cells", active_liquid_cells},
      {"connected_mh2o_cells", connected_liquid_cells},
      {"endpoint_cells_active", endpoint_cells_active},
      {"river_connected", river_connected},
      {"river_aligned", river_aligned},
      {"wadeable", water_column_wadeable},
      {"minimum_water_column", std::isfinite(minimum_water_column)
        ? nlohmann::json(minimum_water_column) : nlohmann::json(nullptr)},
      {"maximum_water_column", std::isfinite(maximum_water_column)
        ? nlohmann::json(maximum_water_column) : nlohmann::json(nullptr)},
      {"minimum_liquid_depth", std::isfinite(minimum_liquid_depth)
        ? nlohmann::json(minimum_liquid_depth) : nlohmann::json(nullptr)},
      {"maximum_liquid_depth", std::isfinite(maximum_liquid_depth)
        ? nlohmann::json(maximum_liquid_depth) : nlohmann::json(nullptr)}};

    std::vector<ProceduralLayoutFeature const*> boundary_features;
    std::vector<ProceduralLayoutFeature const*> wall_features;
    std::vector<ProceduralLayoutFeature const*> jungle_texture_exclusions;
    for (auto const& feature : layout.features)
    {
      if (feature.name.ends_with("_lane") || feature.name == "river_bed")
        boundary_features.push_back(&feature);
      if (feature.name.starts_with("jungle_") && feature.name.ends_with("_wall_mass"))
        wall_features.push_back(&feature);
      if (feature.name.ends_with("_camp_floor")
          || feature.name.starts_with("objective_")
          || feature.name.ends_with("_inner_court")
          || feature.name.ends_with("_base_apron"))
        jungle_texture_exclusions.push_back(&feature);
    }
    // The authored routes are only a skeleton used to flatten the terrain.
    // jungle_domain is filled after the camp manifest is parsed from the
    // physical components containing those camps.
    std::vector<std::uint8_t> jungle_domain(triangle_count);
    std::vector<std::uint8_t> lane_or_river(triangle_count);
    for (std::size_t index = 0; index < triangle_count; ++index)
    {
      auto const [u, v] = triangleCenter(index);
      for (auto const* feature : boundary_features)
        if (nearFeature(*feature, u, v, -.5f * feature->transition_width_ratio))
        {
          lane_or_river[index] = 1;
          break;
        }
    }

    auto const inQuadrant = [](std::string_view quadrant, float u, float v)
    {
      if (quadrant == "north")
        return v < u && u + v < 1.0f;
      if (quadrant == "east")
        return v < u && u + v > 1.0f;
      if (quadrant == "south")
        return v > u && u + v > 1.0f;
      return v > u && u + v < 1.0f;
    };

    auto flood = [&](std::vector<std::size_t> const& seeds, auto&& allowed)
    {
      std::vector<std::uint8_t> reached(triangle_count);
      std::deque<std::size_t> queue;
      for (auto const seed : seeds)
        if (seed < triangle_count && allowed(seed) && !reached[seed])
        {
          reached[seed] = 1;
          queue.push_back(seed);
        }
      while (!queue.empty())
      {
        auto const index = queue.front();
        queue.pop_front();
        forNeighbors(index, [&](std::size_t neighbor)
        {
          if (!reached[neighbor] && allowed(neighbor))
          {
            reached[neighbor] = 1;
            queue.push_back(neighbor);
          }
        });
      }
      return reached;
    };

    std::vector<std::size_t> boundary_seeds;
    if (auto const* left_base = findFeature(layout, "team_left_inner_court"))
    {
      auto const [u, v] = centroid(*left_base);
      boundary_seeds.push_back(triangleAt(u, v));
    }
    else
    {
      auto const fallback = std::find(walkable.begin(), walkable.end(), 1);
      if (fallback != walkable.end())
        boundary_seeds.push_back(
          static_cast<std::size_t>(std::distance(walkable.begin(), fallback)));
    }
    auto const globally_reached = flood(boundary_seeds,
      [&](std::size_t index) { return walkable[index] != 0; });

    auto const* semantics = blueprint.contains("moba_semantics")
      && blueprint.at("moba_semantics").is_object()
      ? &blueprint.at("moba_semantics") : nullptr;
    if (!semantics)
      addIssue(report, "manifest.missing", "manifeste moba_semantics absent");
    static auto const valid_quadrants = std::set<std::string>{
      "north", "east", "south", "west"};
    static auto const valid_kinds = std::set<std::string>{
      "hub", "medium", "spur"};
    static auto const valid_route_roles = std::set<std::string>{
      "clear", "spur", "branch", "door", "wall_cut", "separator"};
    static auto const required_boundaries = std::set<std::string>{
      "top_lane", "middle_lane", "bottom_lane", "river_bed"};

    for (auto const& boundary_name : required_boundaries)
    {
      auto const* boundary = findFeature(layout, boundary_name);
      if (!boundary || boundary->points.size() < 2) continue;
      auto const& first = boundary->points.front();
      auto const& last = boundary->points.back();
      auto const first_index = triangleAt(first.u, first.v);
      auto const last_index = triangleAt(last.u, last.v);
      auto const reached = flood({first_index}, [&](std::size_t index)
      {
        auto const [u, v] = triangleCenter(index);
        return walkable[index] && nearFeature(*boundary, u, v, .002f);
      });
      if (!walkable[first_index] || !walkable[last_index] || !reached[last_index])
        addIssue(report, "navigation.boundary_continuity",
                 "frontière praticable coupée entre ses deux extrémités",
                 boundary_name);
    }

    std::vector<Camp> camps;
    std::set<std::string> camp_names;
    std::set<std::string> camp_features;
    if (!semantics || !semantics->contains("camps")
        || !semantics->at("camps").is_array())
      addIssue(report, "manifest.camps", "moba_semantics.camps doit être un tableau");
    else
      for (auto const& value : semantics->at("camps"))
      {
        if (!value.is_object()
            || !value.contains("name") || !value.at("name").is_string()
            || !value.contains("quadrant") || !value.at("quadrant").is_string()
            || !value.contains("kind") || !value.at("kind").is_string()
            || !value.contains("feature") || !value.at("feature").is_string()
            || !value.contains("u") || !value.at("u").is_number()
            || !value.contains("v") || !value.at("v").is_number()
            || !value.contains("radius") || !value.at("radius").is_number())
        {
          addIssue(report, "manifest.camp", "entrée de camp incomplète ou mal typée");
          continue;
        }
        Camp camp{value.at("name").get<std::string>(),
                  value.at("quadrant").get<std::string>(),
                  value.at("kind").get<std::string>(),
                  value.at("feature").get<std::string>(),
                  value.at("u").get<float>(), value.at("v").get<float>(),
                  value.at("radius").get<float>()};
        if (!camp_names.insert(camp.name).second)
          addIssue(report, "manifest.duplicate", "camp dupliqué", camp.name);
        if (!camp_features.insert(camp.feature).second)
          addIssue(report, "manifest.duplicate", "feature de camp dupliquée", camp.feature);
        if (!valid_quadrants.contains(camp.quadrant))
          addIssue(report, "manifest.quadrant", "quadrant de camp invalide", camp.name);
        if (!valid_kinds.contains(camp.kind))
          addIssue(report, "manifest.kind", "type de camp invalide", camp.name);
        auto const* camp_feature = findFeature(layout, camp.feature);
        if (!camp_feature || camp_feature->shape != ProceduralLayoutShape::Area)
          addIssue(report, "manifest.feature", "feature de clairière absente", camp.name);
        else
        {
          auto const [feature_u, feature_v] = centroid(*camp_feature);
          auto feature_radius = 0.0f;
          for (auto const& point : camp_feature->points)
            feature_radius = std::max(feature_radius,
              std::hypot(point.u - feature_u, point.v - feature_v));
          if (std::hypot(feature_u - camp.u, feature_v - camp.v) > .00001f
              || std::abs(feature_radius - camp.radius) > .00001f)
            addIssue(report, "manifest.geometry",
              "centre ou rayon différent de la feature", camp.name);
        }
        camps.push_back(std::move(camp));
      }
    if (camps.size() != 12)
      addIssue(report, "camp.count", "12 camps attendus, "
        + std::to_string(camps.size()) + " trouvés");

    // Keep only the four negative-space components that contain jungle
    // camps. This excludes bases and the exterior while preserving genuinely
    // disconnected camp components for the connectivity checks below.
    for (auto const quadrant : std::array<std::string_view, 4>{
           "north", "east", "south", "west"})
    {
      auto seeds = std::vector<std::size_t>{};
      for (auto const& camp : camps)
        if (camp.quadrant == quadrant)
          seeds.push_back(triangleAt(camp.u, camp.v));
      auto const reached = flood(seeds, [&](std::size_t index)
      {
        auto const [u, v] = triangleCenter(index);
        return walkable[index] && !lane_or_river[index]
          && inQuadrant(quadrant, u, v);
      });
      for (std::size_t index = 0; index < triangle_count; ++index)
        jungle_domain[index] |= reached[index];
    }

    std::vector<Route> routes;
    std::set<std::string> route_names;
    if (!semantics || !semantics->contains("routes")
        || !semantics->at("routes").is_array())
      addIssue(report, "manifest.routes", "moba_semantics.routes doit être un tableau");
    else
      for (auto const& value : semantics->at("routes"))
      {
        if (!value.is_object()
            || !value.contains("name") || !value.at("name").is_string()
            || !value.contains("quadrant") || !value.at("quadrant").is_string()
            || !value.contains("role") || !value.at("role").is_string()
            || !value.contains("doors") || !value.at("doors").is_array())
        {
          addIssue(report, "manifest.route", "entrée de route incomplète ou mal typée");
          continue;
        }
        Route route{value.at("name").get<std::string>(),
                    value.at("quadrant").get<std::string>(),
                    value.at("role").get<std::string>(), {}};
        for (auto const& door : value.at("doors"))
        {
          if (!door.is_string()
              || !required_boundaries.contains(door.get<std::string>()))
            addIssue(report, "manifest.boundary", "frontière de route invalide",
                     route.name);
          else
            route.doors.push_back(door.get<std::string>());
        }
        route.feature = findFeature(layout, route.name);
        if (!route_names.insert(route.name).second)
          addIssue(report, "manifest.duplicate", "route dupliquée", route.name);
        if (!valid_quadrants.contains(route.quadrant))
          addIssue(report, "manifest.quadrant", "quadrant de route invalide", route.name);
        if (!valid_route_roles.contains(route.role))
          addIssue(report, "manifest.role", "rôle de route invalide", route.name);
        if (!route.feature || route.feature->shape != ProceduralLayoutShape::Corridor)
          addIssue(report, "manifest.feature", "feature de route absente", route.name);
        routes.push_back(std::move(route));
      }
    for (auto const& feature : layout.features)
      if (((feature.name.starts_with("jungle_") && feature.name.ends_with("_path"))
           || feature.name.ends_with("_wall_cut"))
          && !route_names.contains(feature.name))
        addIssue(report, "manifest.unregistered", "route non déclarée", feature.name);

    auto manifest_boundaries = std::set<std::string>{};
    auto boundary_manifest_valid = semantics && semantics->contains("boundaries")
      && semantics->at("boundaries").is_array()
      && semantics->at("boundaries").size() == required_boundaries.size();
    if (semantics && semantics->contains("boundaries")
        && semantics->at("boundaries").is_array())
      for (auto const& boundary : semantics->at("boundaries"))
      {
        if (!boundary.is_string())
        {
          boundary_manifest_valid = false;
          continue;
        }
        boundary_manifest_valid = manifest_boundaries.insert(
          boundary.get<std::string>()).second && boundary_manifest_valid;
      }
    boundary_manifest_valid = boundary_manifest_valid
      && manifest_boundaries == required_boundaries;
    for (auto const& boundary : required_boundaries)
    {
      auto const* feature = findFeature(layout, boundary);
      boundary_manifest_valid = boundary_manifest_valid && feature
        && feature->shape == ProceduralLayoutShape::Corridor;
    }
    if (!boundary_manifest_valid)
      addIssue(report, "manifest.boundaries",
               "les quatre frontières top/mid/bottom/river doivent être uniques et liées à des corridors");

    auto const parsePointsOfInterest = [&](std::string_view key,
                                           std::set<std::string> const& expected,
                                           std::string_view issue_code)
    {
      auto const key_string = std::string{key};
      auto result = std::vector<PointOfInterest>{};
      auto names = std::set<std::string>{};
      auto valid = semantics && semantics->contains(key_string)
        && semantics->at(key_string).is_array()
        && semantics->at(key_string).size() == expected.size();
      if (semantics && semantics->contains(key_string)
          && semantics->at(key_string).is_array())
      {
        for (auto const& value : semantics->at(key_string))
        {
          if (!value.is_object() || value.size() != 3
              || !value.contains("name") || !value.at("name").is_string()
              || !value.contains("u") || !value.at("u").is_number()
              || !value.contains("v") || !value.at("v").is_number())
          {
            valid = false;
            continue;
          }
          auto const u = value.at("u").get<double>();
          auto const v = value.at("v").get<double>();
          auto const name = value.at("name").get<std::string>();
          auto const coordinates_valid = std::isfinite(u) && std::isfinite(v)
            && u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0;
          auto const unique_expected_name = expected.contains(name)
            && names.insert(name).second;
          auto const* feature = findFeature(layout, name);
          auto linked = coordinates_valid && unique_expected_name && feature
            && feature->shape == ProceduralLayoutShape::Area;
          if (linked)
          {
            auto const [feature_u, feature_v] = centroid(*feature);
            linked = std::hypot(feature_u - static_cast<float>(u),
                                feature_v - static_cast<float>(v)) <= .002f
              && distanceToProceduralShape(feature->points, feature->shape,
                   static_cast<float>(u), static_cast<float>(v), 1.0f, 1.0f).distance
                   <= feature->half_width_ratio;
          }
          valid = valid && linked;
          if (coordinates_valid && unique_expected_name)
            result.push_back({name, static_cast<float>(u), static_cast<float>(v),
                              feature});
        }
      }
      valid = valid && names == expected && result.size() == expected.size();
      if (!valid)
        addIssue(report, std::string{issue_code}, std::string{key}
          + " doit contenir exactement les POI attendus, typés et liés à leurs areas");
      return result;
    };
    auto const objectives = parsePointsOfInterest(
      "objectives", {"objective_north", "objective_south"},
      "manifest.objectives");
    auto const bases = parsePointsOfInterest(
      "bases", {"team_left_inner_court", "team_right_inner_court"},
      "manifest.bases");

    static auto const expected_centers = std::map<std::string, std::pair<float, float>>{
      {"north_raptors", {.5245f, .3496f}}, {"north_red", {.4755f, .2637f}},
      {"north_krugs", {.4344f, .1621f}}, {"east_wolves", {.7573f, .4297f}},
      {"east_blue", {.7613f, .5430f}}, {"east_gromp", {.8493f, .5703f}},
      {"south_raptors", {.4755f, .6504f}}, {"south_red", {.5245f, .7363f}},
      {"south_krugs", {.5656f, .8379f}}, {"west_wolves", {.2427f, .5703f}},
      {"west_blue", {.2387f, .4570f}}, {"west_gromp", {.1507f, .4297f}}};

    auto props_metrics = nlohmann::json::object();
    if (procedural_props)
    {
      auto const isLight = [](std::string const& name)
      {
        return name.find("_glow") != std::string::npos
          || name.ends_with("_flame");
      };
      auto const prop_by_name = [&]()
      {
        auto result = std::map<std::string, ProceduralProp const*>{};
        for (auto const& prop : procedural_props->props)
          result.emplace(prop.name, &prop);
        return result;
      }();
      auto const coincides = [](ProceduralProp const& first,
                                ProceduralProp const& second)
      {
        return std::hypot(first.u - second.u, first.v - second.v) <= .000001f;
      };
      auto requireProp = [&](std::string const& name,
                             float u, float v, float tolerance)
      {
        auto const found = prop_by_name.find(name);
        if (found == prop_by_name.end())
        {
          addIssue(report, "props.missing", "prop obligatoire absent", name);
          return;
        }
        if (std::hypot(found->second->u - u, found->second->v - v) > tolerance)
          addIssue(report, "props.anchor", "coordonnées différentes de l'ancrage sémantique", name);
      };

      auto expected_names = std::set<std::string>{
        "river_glow_west", "river_glow_east"};
      for (auto const* name : {"river_glow_west", "river_glow_east"})
        if (!prop_by_name.contains(name))
          addIssue(report, "props.missing", "lueur de rivière absente", name);
      for (auto const* team : {"team_left", "team_right"})
        for (auto const* flank : {"top_a", "top_b", "middle_a",
                                  "middle_b", "bottom_a", "bottom_b"})
          for (auto const* role : {"brazier", "flame"})
          {
            auto const name = std::string{team} + "_entrance_" + flank
              + "_" + role;
            expected_names.insert(name);
            if (!prop_by_name.contains(name))
              addIssue(report, "props.missing", "marqueur d'entrée absent", name);
          }
      for (auto const& base : bases)
      {
        auto const suffix = std::string{"_inner_court"};
        auto const prefix = base.name.ends_with(suffix)
          ? base.name.substr(0, base.name.size() - suffix.size()) : base.name;
        for (auto const* role : {"_landmark", "_glow"})
        {
          auto const name = prefix + role;
          expected_names.insert(name);
          requireProp(name, base.u, base.v, .00001f);
        }
      }
      for (auto const& objective : objectives)
        for (auto const* role : {"_landmark", "_glow"})
        {
          auto const name = objective.name + role;
          expected_names.insert(name);
          requireProp(name, objective.u, objective.v, .00001f);
        }
      for (auto const& camp : camps)
        for (auto const* role : {"_brazier", "_flame"})
        {
          auto const name = camp.name + role;
          expected_names.insert(name);
          auto const found = prop_by_name.find(name);
          if (found == prop_by_name.end())
            addIssue(report, "props.missing", "marqueur de camp absent", name);
          else if (std::hypot(found->second->u - camp.u,
                              found->second->v - camp.v) > camp.radius)
            addIssue(report, "props.anchor", "marqueur hors de sa clairière", name);
        }

      auto non_light = std::vector<ProceduralProp const*>{};
      auto anchored_count = std::size_t{0};
      auto light_count = std::size_t{0};
      auto lane_counts = std::map<std::string, std::size_t>{
        {"top", 0}, {"middle", 0}, {"bottom", 0}};
      for (auto const& prop : procedural_props->props)
      {
        auto anchored = expected_names.contains(prop.name);
        auto const light = isLight(prop.name);
        light_count += light;
        if (prop.name.ends_with("_glow")
            && !prop.name.starts_with("river_glow_")
            && !prop.name.starts_with("lamp_glow_"))
        {
          auto const support_name = prop.name.substr(
            0, prop.name.size() - std::string_view{"_glow"}.size())
            + "_landmark";
          auto const support = prop_by_name.find(support_name);
          if (support == prop_by_name.end() || !coincides(prop, *support->second))
            addIssue(report, "props.light_pair",
                     "lueur sans landmark au même emplacement", prop.name);
        }
        if (prop.name == "river_glow_west" || prop.name == "river_glow_east")
        {
          auto const* river = findFeature(layout, "river_bed");
          anchored = river && nearFeature(*river, prop.u, prop.v, 0.0f);
        }
        else if (prop.name.starts_with("lamp_"))
        {
          auto const glow = prop.name.starts_with("lamp_glow_");
          auto const prefix_size = glow ? std::string_view{"lamp_glow_"}.size()
                                        : std::string_view{"lamp_"}.size();
          auto const suffix = std::string_view{prop.name}.substr(prefix_size);
          auto const separator = suffix.find('_');
          auto const lane = separator == std::string_view::npos
            ? std::string{} : std::string{suffix.substr(0, separator)};
          auto const ordinal = separator == std::string_view::npos
            ? std::string_view{} : suffix.substr(separator + 1);
          auto const ordinal_valid = !ordinal.empty()
            && std::all_of(ordinal.begin(), ordinal.end(), [](unsigned char character)
               { return std::isdigit(character) != 0; });
          auto const lane_found = lane_counts.find(lane);
          auto const* lane_feature = lane_found == lane_counts.end()
            ? nullptr : findFeature(layout, lane + "_lane");
          anchored = ordinal_valid && lane_feature
            && nearFeature(*lane_feature, prop.u, prop.v, .005f);
          if (anchored && !glow) ++lane_found->second;
          if (glow)
          {
            auto const support = prop_by_name.find(
              "lamp_" + std::string{suffix});
            if (support == prop_by_name.end() || !coincides(prop, *support->second))
              addIssue(report, "props.light_pair",
                       "lueur sans lampe au même emplacement", prop.name);
          }
        }
        else if (prop.name.starts_with("team_left_entrance_")
                 || prop.name.starts_with("team_right_entrance_"))
        {
          auto const prefix = prop.name.starts_with("team_left_")
            ? std::string_view{"team_left_entrance_"}
            : std::string_view{"team_right_entrance_"};
          auto const suffix = std::string_view{prop.name}.substr(prefix.size());
          auto const separator = suffix.find('_');
          auto const lane = separator == std::string_view::npos
            ? std::string{} : std::string{suffix.substr(0, separator)};
          auto anchored_lane = lane;
          if (prefix == std::string_view{"team_right_entrance_"})
          {
            if (anchored_lane == "top") anchored_lane = "bottom";
            else if (anchored_lane == "bottom") anchored_lane = "top";
          }
          auto const valid_role = suffix.ends_with("_brazier")
            || suffix.ends_with("_flame");
          auto const* lane_feature = lane_counts.contains(anchored_lane)
            ? findFeature(layout, anchored_lane + "_lane") : nullptr;
          anchored = expected_names.contains(prop.name)
            && valid_role && lane_feature
            && nearFeature(*lane_feature, prop.u, prop.v, .035f);
          if (prop.name.ends_with("_flame"))
          {
            auto const support_name = prop.name.substr(
              0, prop.name.size() - std::string_view{"_flame"}.size())
              + "_brazier";
            auto const support = prop_by_name.find(support_name);
            if (support == prop_by_name.end() || !coincides(prop, *support->second))
              addIssue(report, "props.light_pair",
                       "flamme sans brasero au même emplacement", prop.name);
          }
        }
        else if (prop.name.ends_with("_flame"))
        {
          auto const support_name = prop.name.substr(
            0, prop.name.size() - std::string_view{"_flame"}.size())
            + "_brazier";
          auto const support = prop_by_name.find(support_name);
          if (support == prop_by_name.end() || !coincides(prop, *support->second))
            addIssue(report, "props.light_pair",
                     "flamme sans brasero au même emplacement", prop.name);
        }
        if (!anchored)
          addIssue(report, expected_names.contains(prop.name)
            ? "props.anchor" : "props.name_contract",
            "nom ou coordonnées sans ancrage POI/camp/lane valide", prop.name);
        else
          ++anchored_count;

        if (light) continue;
        if (liquidCellOccupied(prop.u, prop.v))
          addIssue(report, "props.liquid", "prop solide placé dans une cellule MH2O", prop.name);
        if (!raw_walkable[triangleAt(prop.u, prop.v)])
          addIssue(report, "props.unwalkable", "prop solide placé sur un relief impraticable", prop.name);
        for (auto const* previous : non_light)
          if (std::hypot(previous->u - prop.u, previous->v - prop.v)
              * world_size < .5f)
            addIssue(report, "props.spatial_duplicate",
                     "deux props solides occupent le même emplacement: "
                       + previous->name, prop.name);
        non_light.push_back(&prop);
      }
      for (auto const& [lane, count] : lane_counts)
        if (!count)
          addIssue(report, "props.missing", "chaîne de lampes absente", lane + "_lane");
      for (auto const* prop : non_light)
      {
        auto const counterpart = std::find_if(
          non_light.begin(), non_light.end(), [&](auto const* candidate)
          {
            return candidate->path == prop->path
              && std::abs(candidate->scale - prop->scale) <= .0001f
              && std::hypot(candidate->u - (1.0f - prop->u),
                            candidate->v - (1.0f - prop->v)) <= .001f;
          });
        if (counterpart == non_light.end())
          addIssue(report, "symmetry.props",
                   "prop solide sans homologue à rotation 180 degrés",
                   prop->name);
      }
      props_metrics = {{"count", procedural_props->props.size()},
                       {"anchored", anchored_count},
                       {"lights", light_count},
                       {"solid", non_light.size()},
                       {"lane_lamps", lane_counts}};
    }
    auto const routeMouths = [&](Camp const& camp)
    {
      auto angles = std::vector<float>{};
      for (auto const& route : routes)
      {
        if (!route.feature || route.quadrant != camp.quadrant) continue;
        auto const radius = camp.radius + route.feature->half_width_ratio
          + route.feature->transition_width_ratio;
        if (distanceToProceduralShape(route.feature->points, route.feature->shape,
              camp.u, camp.v, 1.0f, 1.0f).distance > radius - .003f)
          continue;
        auto const& points = route.feature->points;
        for (std::size_t segment = 1; segment < points.size(); ++segment)
        {
          auto const ax = points[segment - 1].u - camp.u;
          auto const ay = points[segment - 1].v - camp.v;
          auto const dx = points[segment].u - points[segment - 1].u;
          auto const dy = points[segment].v - points[segment - 1].v;
          auto const a = dx * dx + dy * dy;
          if (a <= .0000000001f) continue;
          auto const b = 2.0f * (ax * dx + ay * dy);
          auto const c = ax * ax + ay * ay - radius * radius;
          auto const discriminant = b * b - 4.0f * a * c;
          if (discriminant < 0.0f) continue;
          auto const root = std::sqrt(discriminant);
          for (auto const t : {(-b - root) / (2.0f * a),
                               (-b + root) / (2.0f * a)})
            if (t >= -.00001f && t <= 1.00001f)
            {
              auto const crossing_u = points[segment - 1].u
                + dx * std::clamp(t, 0.0f, 1.0f);
              auto const crossing_v = points[segment - 1].v
                + dy * std::clamp(t, 0.0f, 1.0f);
              angles.push_back(std::atan2(crossing_v - camp.v,
                                          crossing_u - camp.u));
            }
        }
      }
      auto clusters = std::vector<std::vector<float>>{};
      for (auto const angle : angles) clusters.push_back({angle});
      auto const circularDiameter = [](std::vector<float> values)
      {
        if (values.size() < 2) return 0.0f;
        for (auto& angle : values)
          if (angle < 0.0f) angle += 6.283185307179586f;
        std::sort(values.begin(), values.end());
        auto largest_gap = values.front() + 6.283185307179586f - values.back();
        for (std::size_t i = 1; i < values.size(); ++i)
          largest_gap = std::max(largest_gap, values[i] - values[i - 1]);
        return 6.283185307179586f - largest_gap;
      };
      constexpr auto mouth_cluster_angle = 20.0f / radians_to_degrees;
      auto merged = true;
      while (merged)
      {
        merged = false;
        for (std::size_t first = 0; first < clusters.size() && !merged; ++first)
          for (std::size_t second = first + 1; second < clusters.size(); ++second)
          {
            auto combined = clusters[first];
            combined.insert(combined.end(), clusters[second].begin(),
                            clusters[second].end());
            if (circularDiameter(combined) > mouth_cluster_angle) continue;
            clusters[first] = std::move(combined);
            clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(second));
            merged = true;
            break;
          }
      }
      return clusters.size();
    };
    auto camp_metrics = nlohmann::json::object();
    for (auto const& camp : camps)
    {
      auto const expected = expected_centers.find(camp.name);
      if (expected == expected_centers.end())
        addIssue(report, "camp.name", "camp inconnu " + camp.name);
      else
      {
        auto const error = std::hypot(camp.u - expected->second.first,
                                      camp.v - expected->second.second);
        if (error > .025f)
          addIssue(report, "camp.position", camp.name + " est décalé de "
            + std::to_string(error));
      }
      auto const diameter = camp.radius * 2.0f;
      auto minimum = .037f;
      auto maximum = .045f;
      if (camp.kind == "hub") { minimum = .031f; maximum = .036f; }
      if (camp.name.ends_with("gromp")) { minimum = .026f; maximum = .029f; }
      if (camp.name.ends_with("krugs")) { minimum = .034f; maximum = .036f; }
      if (diameter < minimum || diameter > maximum)
        addIssue(report, "camp.diameter", camp.name + " a un diamètre de "
          + std::to_string(diameter));

      constexpr auto samples = std::size_t{64};
      std::array<bool, samples> open{};
      auto const ring = camp.radius + .025f;
      for (std::size_t sample = 0; sample < samples; ++sample)
      {
        auto const angle = static_cast<float>(sample) * 6.283185307179586f
          / static_cast<float>(samples);
        auto const index = triangleAt(camp.u + std::cos(angle) * ring,
                                      camp.v + std::sin(angle) * ring);
        open[sample] = walkable[index] && jungle_domain[index];
      }
      // A one- or two-sample blocked sliver is below the physical separator
      // width at this radius and must not split one doorway into two mouths.
      // Read every run from the original ring so wider walls stay walls.
      auto physical_open = open;
      for (std::size_t sample = 0; sample < samples; ++sample)
      {
        if (open[sample] || !open[(sample + samples - 1) % samples]) continue;
        auto gap = std::size_t{0};
        while (gap < samples && !open[(sample + gap) % samples]) ++gap;
        if (gap > 2 || gap == samples) continue;
        for (std::size_t offset = 0; offset < gap; ++offset)
          physical_open[(sample + offset) % samples] = true;
      }
      open = physical_open;
      auto physical_mouths = std::size_t{0};
      auto run = std::size_t{0};
      auto runs = std::vector<std::size_t>{};
      for (std::size_t sample = 0; sample < samples; ++sample)
      {
        if (open[sample]) ++run;
        else if (run) { runs.push_back(run); run = 0; }
      }
      if (run) runs.push_back(run);
      if (runs.size() > 1 && open.front() && open.back())
      {
        runs.front() += runs.back();
        runs.pop_back();
      }
      for (auto const length : runs) physical_mouths += length >= 3;
      auto minimum_mouths = std::size_t{1};
      auto maximum_mouths = std::size_t{3};
      if (camp.kind == "hub") { minimum_mouths = 1; maximum_mouths = 4; }
      if (camp.kind == "spur") { minimum_mouths = 1; maximum_mouths = 4; }
      if (physical_mouths < minimum_mouths || physical_mouths > maximum_mouths)
      {
        auto mouth_mask = std::string{};
        mouth_mask.reserve(samples);
        for (auto const sample_open : open)
          mouth_mask.push_back(sample_open ? '1' : '0');
        addIssue(report, "camp.mouth_count", camp.name + " possède "
          + std::to_string(physical_mouths) + " bouches physiques; anneau="
          + mouth_mask, camp.name);
      }
      auto const route_mouths = routeMouths(camp);
      auto expected_route_minimum = std::size_t{1};
      auto expected_route_maximum = std::size_t{3};
      if (camp.kind == "hub")
      {
        expected_route_minimum = 2;
        expected_route_maximum = 4;
      }
      if (camp.kind == "spur") expected_route_minimum = expected_route_maximum = 1;
      if (route_mouths < expected_route_minimum
          || route_mouths > expected_route_maximum)
        addIssue(report, "camp.route_mouth_count", camp.name + " possède "
          + std::to_string(route_mouths) + " bouches déclarées", camp.name);
      auto const index = triangleAt(camp.u, camp.v);
      if (!walkable[index])
        addIssue(report, "camp.unwalkable", camp.name + " n'est pas praticable");
      if (!globally_reached[index])
        addIssue(report, "poi.unreachable", camp.name + " est inaccessible");
      auto minimum_height = std::numeric_limits<float>::max();
      auto maximum_height = std::numeric_limits<float>::lowest();
      auto flat_samples = std::size_t{0};
      auto total_samples = std::size_t{0};
      for (auto const radius_ratio : {.20f, .45f, .70f})
        for (std::size_t sample = 0; sample < 16; ++sample)
        {
          auto const angle = static_cast<float>(sample) * 6.283185307179586f / 16.0f;
          auto const u = camp.u + std::cos(angle) * camp.radius * radius_ratio;
          auto const v = camp.v + std::sin(angle) * camp.radius * radius_ratio;
          auto const sample_height = sampleHeight(u, v);
          minimum_height = std::min(minimum_height, sample_height);
          maximum_height = std::max(maximum_height, sample_height);
          ++total_samples;
          flat_samples += slopes[triangleAt(u, v)] <= 8.0f;
        }
      if (maximum_height - minimum_height > 1.5f
          || flat_samples * 20 < total_samples * 19)
        addIssue(report, "camp.flatness", "clairière trop accidentée: amplitude="
          + std::to_string(maximum_height - minimum_height) + ", pente_ok="
          + std::to_string(flat_samples) + "/" + std::to_string(total_samples),
          camp.name);
      camp_metrics[camp.name] = {{"diameter_ratio", diameter},
                                 {"physical_mouths", physical_mouths},
                                 {"route_mouths", route_mouths},
                                 {"height_range", maximum_height - minimum_height},
                                 {"reachable", globally_reached[index] != 0}};
    }

    auto const boundaryContacts = [](ProceduralLayoutFeature const& route,
                                     ProceduralLayoutFeature const& boundary)
    {
      auto contacts = std::vector<std::pair<float, float>>{};
      auto in_contact = false;
      auto sum_u = 0.0f;
      auto sum_v = 0.0f;
      auto count = std::size_t{0};
      auto const finishContact = [&]
      {
        if (count)
          contacts.emplace_back(sum_u / static_cast<float>(count),
                                sum_v / static_cast<float>(count));
        in_contact = false;
        sum_u = 0.0f;
        sum_v = 0.0f;
        count = 0;
      };
      for (std::size_t segment = 1; segment < route.points.size(); ++segment)
      {
        auto const& first = route.points[segment - 1];
        auto const& second = route.points[segment];
        auto const length = std::hypot(second.u - first.u, second.v - first.v);
        auto const steps = std::max(1, static_cast<int>(std::ceil(length / .0015f)));
        for (int step = segment == 1 ? 0 : 1; step <= steps; ++step)
        {
          auto const t = static_cast<float>(step) / static_cast<float>(steps);
          auto const u = first.u + (second.u - first.u) * t;
          auto const v = first.v + (second.v - first.v) * t;
          auto const distance = distanceToProceduralShape(
            boundary.points, boundary.shape, u, v, 1.0f, 1.0f).distance;
          auto const touching = distance
            <= route.half_width_ratio + boundary.half_width_ratio + .001f;
          if (touching)
          {
            in_contact = true;
            sum_u += u;
            sum_v += v;
            ++count;
          }
          else if (in_contact)
            finishContact();
        }
      }
      if (in_contact) finishContact();
      return contacts;
    };

    struct BoundaryContact
    {
      float u;
      float v;
      ProceduralLayoutFeature const* route;
    };
    auto contacts_by_boundary = std::map<std::string, std::vector<BoundaryContact>>{};
    for (auto const& route : routes)
    {
      if (!route.feature) continue;
      auto declared = std::set<std::string>{route.doors.begin(), route.doors.end()};
      for (auto const& boundary_name : declared)
      {
        auto const* boundary = findFeature(layout, boundary_name);
        if (!boundary) continue;
        for (auto const& [u, v] : boundaryContacts(*route.feature, *boundary))
          contacts_by_boundary[boundary_name].push_back({u, v, route.feature});
      }
    }
    auto undeclared_contacts_by_boundary
      = std::map<std::string, std::vector<BoundaryContact>>{};
    for (auto const& feature : layout.features)
    {
      auto const route_like = feature.name.starts_with("jungle_")
        && (feature.name.ends_with("_path")
            || feature.name.ends_with("_wall_cut"));
      auto const registered = std::any_of(routes.begin(), routes.end(),
        [&](auto const& route) { return route.name == feature.name; });
      if (!route_like || registered) continue;
      for (auto const* boundary_name : {
             "top_lane", "bottom_lane", "middle_lane", "river_bed"})
      {
        auto const* boundary = findFeature(layout, boundary_name);
        if (!boundary) continue;
        for (auto const& [u, v] : boundaryContacts(feature, *boundary))
          undeclared_contacts_by_boundary[boundary_name].push_back(
            {u, v, &feature});
      }
    }

    // Measure the six useful jungle banks independently. A ray is open only
    // when no physical wall is found in the first .018 beyond the lane/river
    // edge. Scoring stays around declared doors, excluding bases, pits and
    // lane/river crossings, so non-jungle sides cannot create false breaches.
    struct BankSpec
    {
      char const* boundary;
      int side;
      char const* label;
      float maximum_opening;
    };
    static constexpr auto bank_specs = std::array{
      // Limits are the supplied reference profile rounded up by roughly one
      // preview cell. A universal .065 ceiling rejected its intended paired
      // doors on the outer and middle lanes.
      BankSpec{"top_lane", 0, "jungle", .145f},
      BankSpec{"bottom_lane", 0, "jungle", .155f},
      BankSpec{"middle_lane", -1, "left", .108f},
      BankSpec{"middle_lane", 1, "right", .300f},
      BankSpec{"river_bed", -1, "left", .055f},
      BankSpec{"river_bed", 1, "right", .055f}};
    constexpr auto bank_sample_step = .002f;
    constexpr auto bank_wall_ray = .018f;
    constexpr auto bank_contact_window = .075f;
    constexpr auto minimum_separator = .015f;
    auto boundary_opening_metrics = nlohmann::json::object();
    for (auto const& spec : bank_specs)
    {
      // The authored silhouette is normalized and must keep the same topology
      // on every supported footprint. Only absorb one raster cell; scaling the
      // whole limit by 4 / side let compact maps accept merged jungle breaches.
      auto const maximum_opening = spec.maximum_opening
        + 1.0f / static_cast<float>(cells);
      auto const* boundary = findFeature(layout, spec.boundary);
      if (!boundary || boundary->points.size() < 2) continue;
      auto const contacts = contacts_by_boundary.find(spec.boundary);
      auto const* boundary_contacts = contacts == contacts_by_boundary.end()
        ? nullptr : &contacts->second;
      auto const undeclared_contacts
        = undeclared_contacts_by_boundary.find(spec.boundary);
      auto const* physical_undeclared_contacts
        = undeclared_contacts == undeclared_contacts_by_boundary.end()
        ? nullptr : &undeclared_contacts->second;
      auto total_length = 0.0f;
      for (std::size_t segment = 1; segment < boundary->points.size(); ++segment)
        total_length += std::hypot(
          boundary->points[segment].u - boundary->points[segment - 1].u,
          boundary->points[segment].v - boundary->points[segment - 1].v);
      auto const edge = boundary->half_width_ratio
          * (1.0f + boundary->width_variation_ratio)
        + .5f * boundary->transition_width_ratio;

      struct Sample
      {
        float arc;
        bool eligible;
        bool declared;
        bool undeclared;
        bool open;
      };
      std::vector<Sample> samples;
      auto preceding_length = 0.0f;
      for (std::size_t segment = 1; segment < boundary->points.size(); ++segment)
      {
        auto const& first = boundary->points[segment - 1];
        auto const& second = boundary->points[segment];
        auto const du = second.u - first.u;
        auto const dv = second.v - first.v;
        auto const length = std::hypot(du, dv);
        if (length <= 0.0f) continue;
        auto const steps = std::max(1,
          static_cast<int>(std::ceil(length / bank_sample_step)));
        auto const normal_u = -dv / length;
        auto const normal_v = du / length;
        for (int step = segment == 1 ? 0 : 1; step <= steps; ++step)
        {
          auto const t = static_cast<float>(step) / static_cast<float>(steps);
          auto const u = first.u + du * t;
          auto const v = first.v + dv * t;
          auto const arc = preceding_length + length * t;
          auto side = spec.side;
          if (side == 0)
          {
            auto const plus = std::hypot(u + normal_u * edge - .5f,
                                         v + normal_v * edge - .5f);
            auto const minus = std::hypot(u - normal_u * edge - .5f,
                                          v - normal_v * edge - .5f);
            side = plus < minus ? 1 : -1;
          }
          auto const outer_u = u + normal_u * static_cast<float>(side) * edge;
          auto const outer_v = v + normal_v * static_cast<float>(side) * edge;
          auto const opposite_u = u - normal_u * static_cast<float>(side) * edge;
          auto const opposite_v = v - normal_v * static_cast<float>(side) * edge;
          auto eligible = arc >= .03f && arc + .03f <= total_length;
          eligible = eligible && outer_u > 0.0f && outer_u < 1.0f
            && outer_v > 0.0f && outer_v < 1.0f;
          auto declared = false;
          if (eligible && boundary_contacts)
          {
            for (auto const& contact : *boundary_contacts)
            {
              if (std::hypot(u - contact.u, v - contact.v)
                    > bank_contact_window)
                continue;
              auto const outside_distance = distanceToProceduralShape(
                contact.route->points, contact.route->shape,
                outer_u, outer_v, 1.0f, 1.0f).distance;
              auto const opposite_distance = distanceToProceduralShape(
                contact.route->points, contact.route->shape,
                opposite_u, opposite_v, 1.0f, 1.0f).distance;
              if (spec.side == 0 || outside_distance <= opposite_distance + .001f)
              {
                declared = true;
                break;
              }
            }
          }
          auto undeclared = false;
          if (eligible && physical_undeclared_contacts)
          {
            for (auto const& contact : *physical_undeclared_contacts)
            {
              auto const contact_window = contact.route->half_width_ratio
                  * (1.0f + contact.route->width_variation_ratio)
                + .5f * contact.route->transition_width_ratio + .003f;
              if (std::hypot(u - contact.u, v - contact.v) > contact_window)
                continue;
              auto const outside_distance = distanceToProceduralShape(
                contact.route->points, contact.route->shape,
                outer_u, outer_v, 1.0f, 1.0f).distance;
              auto const opposite_distance = distanceToProceduralShape(
                contact.route->points, contact.route->shape,
                opposite_u, opposite_v, 1.0f, 1.0f).distance;
              if (spec.side == 0 || outside_distance <= opposite_distance + .001f)
              {
                undeclared = true;
                break;
              }
            }
          }
          for (auto const* collection : {&objectives, &bases})
            for (auto const& point : *collection)
              if (eligible && point.feature
                  && nearFeature(*point.feature, outer_u, outer_v, bank_wall_ray))
                eligible = false;
          for (auto const* other : boundary_features)
            if (eligible && other != boundary
                && nearFeature(*other, u, v,
                               -.5f * other->transition_width_ratio))
              eligible = false;

          auto open = eligible;
          for (int ray = 0; ray <= 6 && open; ++ray)
          {
            auto const distance = edge + bank_wall_ray
              * static_cast<float>(ray) / 6.0f;
            auto const index = triangleAt(
              u + normal_u * static_cast<float>(side) * distance,
              v + normal_v * static_cast<float>(side) * distance);
            open = raw_walkable[index] != 0;
          }
          samples.push_back({arc, eligible, declared, undeclared, open});
        }
        preceding_length += length;
      }

      struct Run
      {
        int state;
        float start;
        float end;
        float width;
      };
      std::vector<Run> runs;
      for (auto const& sample : samples)
      {
        auto const state = sample.eligible && sample.declared
          ? (sample.open ? 1 : 0) : -1;
        if (runs.empty() || runs.back().state != state)
          runs.push_back({state, sample.arc, sample.arc, bank_sample_step});
        else
        {
          runs.back().end = sample.arc;
          runs.back().width = sample.arc - runs.back().start + bank_sample_step;
        }
      }
      auto widest_opening = 0.0f;
      auto narrowest_separator = std::numeric_limits<float>::infinity();
      auto opening_count = std::size_t{0};
      auto const subject = std::string{spec.boundary} + ":" + spec.label;
      std::vector<Run> undeclared_runs;
      for (auto const& sample : samples)
      {
        auto const state = sample.eligible && sample.open && sample.undeclared ? 1 : 0;
        if (undeclared_runs.empty() || undeclared_runs.back().state != state)
          undeclared_runs.push_back(
            {state, sample.arc, sample.arc, bank_sample_step});
        else
        {
          undeclared_runs.back().end = sample.arc;
          undeclared_runs.back().width
            = sample.arc - undeclared_runs.back().start + bank_sample_step;
        }
      }
      auto const minimum_undeclared_opening = std::max(
        .010f, 2.1f / static_cast<float>(cells));
      for (auto const& run : undeclared_runs)
        if (run.state == 1 && run.width > minimum_undeclared_opening)
          addIssue(report, "boundary.undeclared_opening", "ouverture ["
            + std::to_string(run.start) + "," + std::to_string(run.end)
            + "]=" + std::to_string(run.width), subject);
      for (std::size_t run = 0; run < runs.size(); ++run)
      {
        if (runs[run].state == 1)
        {
          ++opening_count;
          widest_opening = std::max(widest_opening, runs[run].width);
          if (runs[run].width > maximum_opening)
            addIssue(report, "boundary.opening_width", "ouverture ["
              + std::to_string(runs[run].start) + ","
              + std::to_string(runs[run].end) + "]="
              + std::to_string(runs[run].width) + " > "
              + std::to_string(maximum_opening), subject);
        }
        if (runs[run].state != 0 || run == 0 || run + 1 == runs.size()
            || runs[run - 1].state != 1 || runs[run + 1].state != 1)
          continue;
        auto const merged_width = runs[run - 1].width + runs[run].width
          + runs[run + 1].width;
        // A separator narrower than two terrain cells is rasterization noise,
        // not a stable physical obstacle. Still score both adjacent openings
        // as one so a real merged breach cannot escape the width check.
        if (runs[run].width <= 2.1f / static_cast<float>(cells))
        {
          widest_opening = std::max(widest_opening, merged_width);
          if (merged_width > maximum_opening)
            addIssue(report, "boundary.opening_width", "brèche fusionnée autour de ["
              + std::to_string(runs[run].start) + ","
              + std::to_string(runs[run].end) + "]="
              + std::to_string(merged_width) + " > "
              + std::to_string(maximum_opening), subject);
          continue;
        }
        narrowest_separator = std::min(narrowest_separator, runs[run].width);
        if (runs[run].width >= minimum_separator) continue;
        addIssue(report, "boundary.separator_width", "séparateur ["
          + std::to_string(runs[run].start) + ","
          + std::to_string(runs[run].end) + "]="
          + std::to_string(runs[run].width) + " < "
          + std::to_string(minimum_separator), subject);
        if (merged_width > maximum_opening)
          addIssue(report, "boundary.opening_width", "brèche fusionnée autour de ["
            + std::to_string(runs[run].start) + ","
            + std::to_string(runs[run].end) + "]="
            + std::to_string(merged_width) + " > "
            + std::to_string(maximum_opening), subject);
      }
      boundary_opening_metrics[subject] = {
        {"openings", opening_count},
        {"maximum_opening_ratio", widest_opening},
        {"minimum_separator_ratio", std::isfinite(narrowest_separator)
          ? nlohmann::json(narrowest_separator) : nlohmann::json(nullptr)},
        {"ordinary_maximum_ratio", maximum_opening},
        {"separator_minimum_ratio", minimum_separator},
        {"wall_ray_ratio", bank_wall_ray}};
    }

    auto quadrant_metrics = nlohmann::json::object();
    for (auto const& quadrant : std::array<std::string, 4>{
           "north", "east", "south", "west"})
    {
      std::vector<Camp const*> local;
      for (auto const& camp : camps)
        if (camp.quadrant == quadrant) local.push_back(&camp);
      auto const hub = std::find_if(local.begin(), local.end(),
        [](auto const* camp) { return camp->kind == "hub"; });
      auto const medium = std::find_if(local.begin(), local.end(),
        [](auto const* camp) { return camp->kind == "medium"; });
      auto const spur = std::find_if(local.begin(), local.end(),
        [](auto const* camp) { return camp->kind == "spur"; });
      auto angle_degrees = 0.0f;
      if (local.size() != 3 || hub == local.end()
          || medium == local.end() || spur == local.end())
        addIssue(report, "quadrant.camps", quadrant + " ne contient pas hub/medium/spur");
      else
      {
        auto const expected_hub = expected_centers.find((*hub)->name);
        auto const expected_medium = expected_centers.find((*medium)->name);
        auto const expected_spur = expected_centers.find((*spur)->name);
        if (expected_hub == expected_centers.end()
            || expected_medium == expected_centers.end()
            || expected_spur == expected_centers.end())
        {
          addIssue(report, "quadrant.reference", "camp sans référence", quadrant);
          continue;
        }
        angle_degrees = angleDegrees(
          {(*hub)->u, (*hub)->v},
          {(*medium)->u, (*medium)->v},
          {(*spur)->u, (*spur)->v});
        auto const expected_angle = angleDegrees(
          expected_hub->second, expected_medium->second, expected_spur->second);
        if (std::abs(angle_degrees - expected_angle) > 8.0f)
          addIssue(report, "quadrant.camp_angle", quadrant + " forme un angle de "
            + std::to_string(angle_degrees) + " degrés au lieu de "
            + std::to_string(expected_angle));
        for (auto const* camp : {*medium, *spur})
        {
          auto const distance = std::hypot(camp->u - (*hub)->u,
                                           camp->v - (*hub)->v);
          auto const minimum = camp->kind == "medium" ? .09f : .09f;
          auto const maximum = camp->kind == "medium" ? .12f : .13f;
          if (distance < minimum || distance > maximum)
            addIssue(report, "quadrant.camp_distance", quadrant
              + " a une distance hub-camp de " + std::to_string(distance));
        }
        auto const reached = flood({triangleAt((*hub)->u, (*hub)->v)},
          [&](std::size_t index)
          {
            auto const [u, v] = triangleCenter(index);
            return walkable[index] && jungle_domain[index]
              && !lane_or_river[index] && inQuadrant(quadrant, u, v);
          });
        for (auto const* camp : local)
          if (!reached[triangleAt(camp->u, camp->v)])
            addIssue(report, "quadrant.full_clear", quadrant
              + " ne relie pas ses camps hors lane/rivière");
      }

      std::vector<Door> doors;
      for (auto const& route : routes)
      {
        if (route.quadrant != quadrant || !route.feature) continue;
        auto const expected = std::multiset<std::string>(
          route.doors.begin(), route.doors.end());
        for (auto const& boundary_name : required_boundaries)
        {
          auto const* boundary = findFeature(layout, boundary_name);
          if (!boundary) continue;
          auto const contacts = boundaryContacts(*route.feature, *boundary);
          auto const expected_count = expected.count(boundary_name);
          if (contacts.size() != expected_count)
            addIssue(report, "route.boundary_contacts", "contacts avec "
              + boundary_name + ": " + std::to_string(contacts.size())
              + " au lieu de " + std::to_string(expected_count), route.name);
          for (auto const& contact : contacts)
          {
            auto const duplicate = std::any_of(doors.begin(), doors.end(),
              [&](auto const& door)
              {
                return door.boundary == boundary_name
                  && std::hypot(door.u - contact.first,
                                door.v - contact.second) < .035f;
              });
            if (!duplicate)
            {
              doors.push_back(
                {contact.first, contact.second, boundary_name, route.name});
            }
          }
        }
      }
      auto const boundaryCount = [&](std::string_view boundary)
      {
        return static_cast<std::size_t>(std::count_if(doors.begin(), doors.end(),
          [&](auto const& door) { return door.boundary == boundary; }));
      };
      auto const outer_boundary = quadrant == "north" || quadrant == "west"
        ? "top_lane" : "bottom_lane";
      auto const outer_doors = boundaryCount(outer_boundary);
      auto const mid_doors = boundaryCount("middle_lane");
      auto const river_doors = boundaryCount("river_bed");
      if (doors.size() < 5 || doors.size() > 6
          || outer_doors != 2 || mid_doors != 2
          || river_doors < 1 || river_doors > 2)
        addIssue(report, "quadrant.door_count", quadrant + " possède "
          + std::to_string(doors.size()) + " portes (outer="
          + std::to_string(outer_doors) + ", mid=" + std::to_string(mid_doors)
          + ", river=" + std::to_string(river_doors) + ")");
      auto const quadrantWalkable = [&](std::size_t index)
      {
        if (!walkable[index] || !jungle_domain[index]
            || lane_or_river[index]) return false;
        auto const [u, v] = triangleCenter(index);
        return inQuadrant(quadrant, u, v);
      };
      auto const interiorSeed = [&](Door const& door)
        -> std::optional<std::size_t>
      {
        auto const maximum_anchor_distance = .05f
          + 1.0f / static_cast<float>(cells);
        auto nearest_distance = maximum_anchor_distance;
        auto nearest = std::optional<std::size_t>{};
        for (std::size_t index = 0; index < triangle_count; ++index)
        {
          if (!quadrantWalkable(index)) continue;
          auto const [u, v] = triangleCenter(index);
          auto const distance = std::hypot(u - door.u, v - door.v);
          if (distance >= nearest_distance) continue;
          nearest_distance = distance;
          nearest = index;
        }
        return nearest;
      };
      auto const boundarySeed = [&](Door const& door,
                                    ProceduralLayoutFeature const& boundary)
        -> std::optional<std::size_t>
      {
        for (int ring = 0; ring <= 8; ++ring)
        {
          auto const radius = static_cast<float>(ring) * .003f;
          for (int sample = 0; sample < 24; ++sample)
          {
            auto const angle = static_cast<float>(sample)
              * 6.283185307179586f / 24.0f;
            auto const index = triangleAt(door.u + std::cos(angle) * radius,
                                          door.v + std::sin(angle) * radius);
            auto const [u, v] = triangleCenter(index);
            if (walkable[index]
                && nearFeature(boundary, u, v,
                               -.5f * boundary.transition_width_ratio))
              return index;
          }
        }
        return std::nullopt;
      };
      auto const nearestInterior = [&](Door const& door)
      {
        auto nearest_distance = std::numeric_limits<float>::infinity();
        auto nearest_u = 0.0f;
        auto nearest_v = 0.0f;
        for (std::size_t index = 0; index < triangle_count; ++index)
        {
          if (!quadrantWalkable(index)) continue;
          auto const [u, v] = triangleCenter(index);
          auto const distance = std::hypot(u - door.u, v - door.v);
          if (distance >= nearest_distance) continue;
          nearest_distance = distance;
          nearest_u = u;
          nearest_v = v;
        }
        return std::string{"distance="} + std::to_string(nearest_distance)
          + " vers (" + std::to_string(nearest_u) + ","
          + std::to_string(nearest_v) + ")";
      };
      auto const loopExists = [&](std::string const& boundary_name)
        -> std::pair<bool, std::string>
      {
        auto pair = std::vector<Door const*>{};
        for (auto const& door : doors)
          if (door.boundary == boundary_name) pair.push_back(&door);
        auto const doorDescription = [](Door const& door)
        {
          return door.route + "@(" + std::to_string(door.u) + ","
            + std::to_string(door.v) + ")";
        };
        if (pair.size() != 2)
          return {false, std::to_string(pair.size()) + " portes"};
        auto const pair_description = doorDescription(*pair[0]) + " <-> "
          + doorDescription(*pair[1]);
        if (std::hypot(pair[0]->u - pair[1]->u,
                       pair[0]->v - pair[1]->v) < .05f)
          return {false, "portes confondues: " + pair_description};
        auto const first_inside = interiorSeed(*pair[0]);
        auto const second_inside = interiorSeed(*pair[1]);
        auto const* boundary = findFeature(layout, boundary_name);
        if (!first_inside || !second_inside || !boundary)
          return {false, "ancrage jungle absent: " + pair_description
            + "; premier " + nearestInterior(*pair[0]) + "; second "
            + nearestInterior(*pair[1])};
        if (*first_inside == *second_inside)
          return {false, "même ancrage jungle: " + pair_description};
        auto const jungle_reached = flood({*first_inside}, quadrantWalkable);
        if (!jungle_reached[*second_inside])
          return {false, "composantes jungle séparées: " + pair_description};
        for (auto const* camp : local)
          if (!jungle_reached[triangleAt(camp->u, camp->v)])
            return {false, "camp " + camp->name
              + " hors composante: " + pair_description};
        auto const first_boundary = boundarySeed(*pair[0], *boundary);
        auto const second_boundary = boundarySeed(*pair[1], *boundary);
        if (!first_boundary || !second_boundary)
          return {false, "ancrage frontière absent: " + pair_description};
        if (*first_boundary == *second_boundary)
          return {false, "même ancrage frontière: " + pair_description};
        for (auto const [door, inside, outside] : std::array{
               std::tuple{pair[0], *first_inside, *first_boundary},
               std::tuple{pair[1], *second_inside, *second_boundary}})
        {
          auto const crossing = flood({inside}, [&](std::size_t index)
          {
            auto const [u, v] = triangleCenter(index);
            return walkable[index]
              && std::hypot(u - door->u, v - door->v)
                <= .05f + 1.0f / static_cast<float>(cells);
          });
          if (!crossing[outside])
            return {false, "porte non franchissable localement: "
              + doorDescription(*door)};
        }
        auto const boundary_reached = flood({*first_boundary},
          [&](std::size_t index)
          {
            auto const [u, v] = triangleCenter(index);
            return walkable[index] && nearFeature(*boundary, u, v,
              -.5f * boundary->transition_width_ratio);
          });
        if (!boundary_reached[*second_boundary])
          return {false, "frontière physiquement coupée: " + pair_description};
        return {true, pair_description};
      };
      auto const outer_loop = loopExists(outer_boundary);
      auto const mid_loop = loopExists("middle_lane");
      if (!outer_loop.first || !mid_loop.first)
        addIssue(report, "quadrant.loop", "boucles physiques outer="
          + std::to_string(outer_loop.first) + " [" + outer_loop.second
          + "], mid=" + std::to_string(mid_loop.first) + " ["
          + mid_loop.second + "]",
          quadrant);
      quadrant_metrics[quadrant] = {{"camp_angle_degrees", angle_degrees},
        {"doors", doors.size()}, {"outer_doors", outer_doors},
        {"mid_doors", mid_doors}, {"river_doors", river_doors},
        {"outer_loop", outer_loop.first}, {"mid_loop", mid_loop.first},
        {"outer_loop_detail", outer_loop.second},
        {"mid_loop_detail", mid_loop.second}};
    }

    auto objective_metrics = nlohmann::json::object();
    auto const north_objective = std::find_if(
      objectives.begin(), objectives.end(), [](auto const& objective)
      { return objective.name == "objective_north"; });
    auto const south_objective = std::find_if(
      objectives.begin(), objectives.end(), [](auto const& objective)
      { return objective.name == "objective_south"; });
    if (north_objective != objectives.end()
        && south_objective != objectives.end()
        && std::hypot((1.0f - north_objective->u) - south_objective->u,
                      (1.0f - north_objective->v) - south_objective->v) > .00001f)
      addIssue(report, "manifest.objectives",
               "les deux objectifs ne sont pas symétriques à 180 degrés");
    auto const* river_feature = findFeature(layout, "river_bed");
    for (auto const& objective : objectives)
    {
      if (!objective.feature || !river_feature) continue;
      auto const center = triangleAt(objective.u, objective.v);
      auto const center_reachable = walkable[center] && globally_reached[center];
      auto pit_radius = 0.0f;
      for (auto const& point : objective.feature->points)
        pit_radius = std::max(pit_radius,
          std::hypot(point.u - objective.u, point.v - objective.v));
      pit_radius += objective.feature->half_width_ratio
        + .5f * objective.feature->transition_width_ratio;

      constexpr auto mouth_samples = std::size_t{128};
      auto open = std::array<std::uint8_t, mouth_samples>{};
      auto river_open_samples = std::size_t{0};
      auto open_samples = std::size_t{0};
      for (std::size_t sample = 0; sample < mouth_samples; ++sample)
      {
        auto const angle = static_cast<float>(sample)
          * 6.283185307179586f / static_cast<float>(mouth_samples);
        auto const u = objective.u + std::cos(angle) * pit_radius;
        auto const v = objective.v + std::sin(angle) * pit_radius;
        auto const index = triangleAt(u, v);
        open[sample] = walkable[index];
        open_samples += open[sample] != 0;
        river_open_samples += open[sample]
          && nearFeature(*river_feature, u, v, 0.0f);
      }
      auto mouth_clusters = std::size_t{0};
      if (open_samples == mouth_samples)
      {
        mouth_clusters = 1;
      }
      else if (open_samples)
      {
        auto closed = std::find(open.begin(), open.end(), std::uint8_t{0});
        auto previous_open = false;
        for (std::size_t offset = 1; offset <= mouth_samples; ++offset)
        {
          auto const index = (static_cast<std::size_t>(closed - open.begin()) + offset)
            % mouth_samples;
          auto const current_open = open[index] != 0;
          mouth_clusters += current_open && !previous_open;
          previous_open = current_open;
        }
      }

      auto const pit_reached = center_reachable ? flood({center},
        [&](std::size_t index)
        {
          auto const [u, v] = triangleCenter(index);
          return walkable[index]
            && (nearFeature(*objective.feature, u, v, 0.0f)
                || nearFeature(*river_feature, u, v, 0.0f));
        }) : std::vector<std::uint8_t>(triangle_count);
      auto river_reachable = false;
      for (std::size_t index = 0; index < triangle_count && !river_reachable; ++index)
      {
        if (!pit_reached[index]) continue;
        auto const [u, v] = triangleCenter(index);
        river_reachable = nearFeature(*river_feature, u, v,
          -.5f * river_feature->transition_width_ratio);
      }
      auto const single_river_mouth = center_reachable && river_reachable
        && mouth_clusters == 1 && open_samples > 0
        && open_samples * 2 <= mouth_samples
        && river_open_samples * 4 >= open_samples * 3;
      objective_metrics[objective.name] = {
        {"reachable", center_reachable}, {"river_reachable", river_reachable},
        {"mouth_clusters", mouth_clusters}, {"open_samples", open_samples},
        {"river_open_samples", river_open_samples}};
      if (!single_river_mouth)
      {
        auto mouth_mask = std::string{};
        mouth_mask.reserve(mouth_samples);
        for (auto const sample_open : open)
          mouth_mask.push_back(sample_open ? '1' : '0');
        addIssue(report, "objective.river_mouth", "praticable="
          + std::to_string(center_reachable) + ", rivière="
          + std::to_string(river_reachable) + ", ouvertures="
          + std::to_string(mouth_clusters) + ", arc="
          + std::to_string(open_samples) + "/"
          + std::to_string(mouth_samples) + ", anneau=" + mouth_mask,
          objective.name);
      }
    }

    auto const distance_resolution = cells * 2;
    auto const distancePixel = [&](float u, float v)
    {
      auto const x = std::min(distance_resolution - 1,
        static_cast<std::size_t>(std::clamp(u, 0.0f, .999999f)
          * static_cast<float>(distance_resolution)));
      auto const y = std::min(distance_resolution - 1,
        static_cast<std::size_t>(std::clamp(v, 0.0f, .999999f)
          * static_cast<float>(distance_resolution)));
      return y * distance_resolution + x;
    };
    auto blocked_sources = std::vector<std::uint8_t>(
      distance_resolution * distance_resolution);
    auto walkable_sources = std::vector<std::uint8_t>(
      distance_resolution * distance_resolution);
    auto feature_core_samples = std::vector<std::size_t>(layout.features.size());
    auto strong_texture_samples = std::vector<std::size_t>(
      layout.texture_paths.size());
    auto maximum_texture_alpha = std::vector<std::uint8_t>(
      layout.texture_paths.size());
    auto jungle_texture_samples = std::size_t{0};
    auto jungle_green_samples = std::size_t{0};
    auto jungle_non_green_by_feature = std::map<std::string, std::size_t>{};
    auto jungle_non_green_by_semantic = std::map<std::string, std::size_t>{};
    for (std::size_t y = 0; y < distance_resolution; ++y)
      for (std::size_t x = 0; x < distance_resolution; ++x)
      {
        auto const u = (static_cast<float>(x) + .5f)
          / static_cast<float>(distance_resolution);
        auto const v = (static_cast<float>(y) + .5f)
          / static_cast<float>(distance_resolution);
        auto const triangle = triangleAt(u, v);
        auto const is_walkable = raw_walkable[triangle] != 0;
        blocked_sources[y * distance_resolution + x] = !is_walkable;
        walkable_sources[y * distance_resolution + x] = is_walkable;
        auto const sample = sampleProceduralLayout(
          layout, u, v, base_height, slopes[triangle], world_size, world_size);
        for (std::size_t feature = 0; feature < layout.features.size(); ++feature)
          feature_core_samples[feature] += sample.feature_masks[feature] >= .999f;
        for (std::size_t layer = 0; layer < layout.texture_paths.size(); ++layer)
        {
          strong_texture_samples[layer] += sample.quantized_weights[layer] >= 64;
          maximum_texture_alpha[layer] = std::max(
            maximum_texture_alpha[layer], sample.quantized_weights[layer]);
        }
        auto near_camp = false;
        for (auto const& camp : camps)
          near_camp = near_camp
            || std::hypot(u - camp.u, v - camp.v) <= camp.radius + .012f;
        auto const near_excluded_area = std::any_of(
          jungle_texture_exclusions.begin(), jungle_texture_exclusions.end(),
          [&](auto const* feature)
          {
            return nearFeature(*feature, u, v, 0.0f);
          });
        if (walkable[triangle] && jungle_domain[triangle]
            && !lane_or_river[triangle] && !near_camp
            && !near_excluded_area)
        {
          ++jungle_texture_samples;
          auto const green = sample.quantized_weights[0] >= 160
            && sample.quantized_weights[1] <= 80;
          jungle_green_samples += green;
          if (!green)
          {
            auto dominant = std::string{"arena_ground"};
            auto dominant_mask = 0.0f;
            for (std::size_t feature = 0;
                 feature < layout.features.size(); ++feature)
              if (sample.feature_masks[feature] > dominant_mask)
              {
                dominant_mask = sample.feature_masks[feature];
                dominant = layout.features[feature].name;
              }
            ++jungle_non_green_by_feature[dominant];
            auto semantic = std::string{"other"};
            if (dominant.ends_with("_lane")) semantic = "lane";
            else if (dominant == "river_bed") semantic = "river";
            else if (dominant.starts_with("objective_")) semantic = "objective";
            else if (dominant.ends_with("_inner_court")
                     || dominant.ends_with("_base_apron")) semantic = "base";
            ++jungle_non_green_by_semantic[semantic];
          }
        }
      }
    auto feature_metrics = nlohmann::json::object();
    for (std::size_t feature = 0; feature < layout.features.size(); ++feature)
    {
      feature_metrics[layout.features[feature].name]
        = {{"effective_core_samples", feature_core_samples[feature]}};
      if (feature_core_samples[feature] < 4)
        addIssue(report, "feature.core_missing", "moins de quatre échantillons de cœur",
                 layout.features[feature].name);
    }
    auto texture_metrics = nlohmann::json::array();
    for (std::size_t layer = 0; layer < layout.texture_paths.size(); ++layer)
    {
      texture_metrics.push_back({{"layer", layer},
        {"path", layout.texture_paths[layer]},
        {"strong_samples", strong_texture_samples[layer]},
        {"maximum_alpha", maximum_texture_alpha[layer]}});
      if (strong_texture_samples[layer] < 64 || maximum_texture_alpha[layer] < 64)
        addIssue(report, "texture.coverage", "texture sans couverture forte",
                 layout.texture_paths[layer]);
    }
    if (!jungle_texture_samples
        || jungle_green_samples * 10 < jungle_texture_samples * 9)
    {
      auto breakdown = std::ostringstream{};
      auto first = true;
      for (auto const& [semantic, count] : jungle_non_green_by_semantic)
      {
        if (!first) breakdown << ", ";
        breakdown << semantic << '=' << count;
        first = false;
      }
      auto dominant_features = std::vector<std::pair<std::string, std::size_t>>(
        jungle_non_green_by_feature.begin(), jungle_non_green_by_feature.end());
      std::sort(dominant_features.begin(), dominant_features.end(),
        [](auto const& left, auto const& right)
        {
          return left.second != right.second
            ? left.second > right.second : left.first < right.first;
        });
      auto features = std::ostringstream{};
      for (std::size_t index = 0;
           index < std::min(std::size_t{8}, dominant_features.size()); ++index)
      {
        if (index) features << ", ";
        features << dominant_features[index].first << '='
                 << dominant_features[index].second;
      }
      addIssue(report, "texture.jungle_green", "sol vert sur "
        + std::to_string(jungle_green_samples) + "/"
        + std::to_string(jungle_texture_samples) + " échantillons de jungle; "
        + breakdown.str() + "; dominantes: " + features.str());
    }

    struct ScatterPreviewPoint
    {
      float u = 0.0f;
      float v = 0.0f;
      bool walls = false;
      bool accepted = false;
    };
    auto scatter_preview = std::vector<ScatterPreviewPoint>{};
    auto scatter_metrics = nlohmann::json::object();
    auto preceding_instances = std::vector<std::pair<float, float>>{};
    if (procedural_props)
      for (auto const& prop : procedural_props->props)
        preceding_instances.emplace_back(prop.u, prop.v);
    auto const props_occupancy_count = preceding_instances.size();
    auto const auditScatter = [&](ProceduralScatter const& scatter, bool walls)
    {
      struct RegionCounts
      {
        std::size_t generated = 0;
        std::size_t viable = 0;
        std::size_t accepted = 0;
        std::size_t liquid_rejections = 0;
        std::size_t spacing_rejections = 0;
        std::size_t obstructions = 0;
      };
      auto counts = std::vector<RegionCounts>(scatter.regions.size());
      auto wall_positions = std::vector<
        std::vector<std::optional<std::pair<float, float>>>>{};
      wall_positions.reserve(scatter.regions.size());
      for (auto const& region : scatter.regions)
        wall_positions.emplace_back(walls ? region.density_per_tile : 0);
      auto occupied = std::vector<std::pair<std::pair<float, float>, float>>{};
      if (!walls)
        for (auto const& position : preceding_instances)
          occupied.push_back({{position.first * world_size,
                               position.second * world_size}, 0.0f});
      auto region_metrics = nlohmann::json::object();
      auto total_viable = std::size_t{0};
      auto total_accepted = std::size_t{0};
      auto total_liquid_rejections = std::size_t{0};
      auto total_obstructions = std::size_t{0};
      auto obstruction_details = std::vector<std::string>{};
      for (std::size_t tile_z = 0; tile_z < footprint_side_tiles; ++tile_z)
        for (std::size_t tile_x = 0; tile_x < footprint_side_tiles; ++tile_x)
        {
          auto const u_min = static_cast<float>(tile_x)
            / static_cast<float>(footprint_side_tiles);
          auto const u_max = static_cast<float>(tile_x + 1)
            / static_cast<float>(footprint_side_tiles);
          auto const v_min = static_cast<float>(tile_z)
            / static_cast<float>(footprint_side_tiles);
          auto const v_max = static_cast<float>(tile_z + 1)
            / static_cast<float>(footprint_side_tiles);
          for (std::size_t region_index = 0;
               region_index < scatter.regions.size(); ++region_index)
          {
            auto const& region = scatter.regions[region_index];
            if (!proceduralScatterRegionIntersects(
                  region, u_min, u_max, v_min, v_max))
              continue;
            auto& region_counts = counts[region_index];
            for (std::size_t candidate_index = 0;
                 candidate_index < region.density_per_tile; ++candidate_index)
            {
              auto const candidate = proceduralScatterCandidate(
                scatter, region_index, min_tile_x + tile_x,
                min_tile_z + tile_z, candidate_index,
                u_min, u_max, v_min, v_max);
              if (!candidate.active) continue;
              auto const preview_candidate = scatter_preview.size();
              scatter_preview.push_back(
                {candidate.u, candidate.v, walls, false});
              ++region_counts.generated;
              if (!walls && !proceduralScatterContains(
                    region.points, candidate.u, candidate.v))
                continue;
              if (proceduralScatterExcluded(
                    scatter, candidate.u, candidate.v, world_size, world_size))
                continue;
              auto const height = sampleHeight(candidate.u, candidate.v);
              auto const slope = slopes[triangleAt(candidate.u, candidate.v)];
              if (height < region.min_height || height > region.max_height
                  || slope < region.min_slope_degrees
                  || slope > region.max_slope_degrees)
                continue;
              if (liquidCellOccupied(candidate.u, candidate.v))
              {
                ++region_counts.liquid_rejections;
                continue;
              }
              ++region_counts.viable;
              if (!walls)
              {
                auto const& asset = scatter.assets[candidate.asset_index];
                auto const spacing = region.min_spacing_ratio
                  * asset.spacing_multiplier * std::max(candidate.scale, .5f)
                  * world_size;
                auto const too_close = std::any_of(
                  occupied.begin(), occupied.end(), [&](auto const& position)
                  {
                    auto const dx = position.first.first - candidate.u * world_size;
                    auto const dz = position.first.second - candidate.v * world_size;
                    auto const required = std::max(spacing, position.second);
                    return dx * dx + dz * dz < required * required;
                  });
                if (too_close)
                {
                  ++region_counts.spacing_rejections;
                  continue;
                }
                occupied.push_back({{candidate.u * world_size,
                                     candidate.v * world_size}, spacing * .45f});
              }
              ++region_counts.accepted;
              scatter_preview[preview_candidate].accepted = true;
              if (walls)
              {
                wall_positions[region_index][candidate_index]
                  = std::pair{candidate.u, candidate.v};
                preceding_instances.emplace_back(candidate.u, candidate.v);
              }
              if (!walls)
              {
                auto const triangle = triangleAt(candidate.u, candidate.v);
                auto obstructions = std::vector<std::string>{};
                if (lane_or_river[triangle])
                  for (auto const* boundary : boundary_features)
                    if (nearFeature(*boundary, candidate.u, candidate.v,
                                    -.5f * boundary->transition_width_ratio))
                      obstructions.push_back(boundary->name);
                for (auto const& route : routes)
                  if (route.feature && nearFeature(
                        *route.feature, candidate.u, candidate.v, .003f))
                    obstructions.push_back(route.name);
                for (auto const& camp : camps)
                  if (std::hypot(candidate.u - camp.u,
                                 candidate.v - camp.v) <= camp.radius + .010f)
                    obstructions.push_back(camp.name);
                for (auto const& objective : objectives)
                  if (objective.feature && nearFeature(*objective.feature,
                        candidate.u, candidate.v, .010f))
                    obstructions.push_back(objective.name);
                for (auto const& base : bases)
                  if (base.feature && nearFeature(*base.feature,
                        candidate.u, candidate.v, .010f))
                    obstructions.push_back(base.name);
                if (!obstructions.empty())
                {
                  ++region_counts.obstructions;
                  obstruction_details.push_back(region.name + "@("
                    + std::to_string(candidate.u) + ","
                    + std::to_string(candidate.v) + ")->"
                    + joined(obstructions));
                }
              }
            }
          }
        }
      for (std::size_t region_index = 0;
           region_index < scatter.regions.size(); ++region_index)
      {
        auto const& region = scatter.regions[region_index];
        auto const& region_counts = counts[region_index];
        total_viable += region_counts.viable;
        total_accepted += region_counts.accepted;
        total_liquid_rejections += region_counts.liquid_rejections;
        total_obstructions += region_counts.obstructions;
        auto maximum_corner_distance = 0.0f;
        auto maximum_gap = 0.0f;
        if (walls)
        {
          for (auto const& corner : region.points)
          {
            auto nearest = std::numeric_limits<float>::infinity();
            for (auto const& position : wall_positions[region_index])
              if (position)
                nearest = std::min(nearest, std::hypot(
                  position->first - corner.u, position->second - corner.v)
                  * world_size);
            maximum_corner_distance = std::max(maximum_corner_distance, nearest);
          }
          for (std::size_t index = 1;
               index < wall_positions[region_index].size(); ++index)
            if (wall_positions[region_index][index - 1]
                && wall_positions[region_index][index])
              maximum_gap = std::max(maximum_gap, std::hypot(
                wall_positions[region_index][index - 1]->first
                  - wall_positions[region_index][index]->first,
                wall_positions[region_index][index - 1]->second
                  - wall_positions[region_index][index]->second) * world_size);
          if (region.name.find("_chain") == std::string::npos
              && wall_positions[region_index].size() > 1
              && wall_positions[region_index].front()
              && wall_positions[region_index].back())
            maximum_gap = std::max(maximum_gap, std::hypot(
              wall_positions[region_index].front()->first
                - wall_positions[region_index].back()->first,
              wall_positions[region_index].front()->second
                - wall_positions[region_index].back()->second) * world_size);
        }
        region_metrics[region.name] = {
          {"generated", region_counts.generated},
          {"viable_before_spacing", region_counts.viable},
          {"accepted_after_spacing", region_counts.accepted},
          {"liquid_rejections", region_counts.liquid_rejections},
          {"spacing_rejections", region_counts.spacing_rejections},
          {"playable_obstructions", region_counts.obstructions},
          {"maximum_corner_distance", walls
            ? nlohmann::json(maximum_corner_distance) : nlohmann::json(nullptr)},
          {"maximum_gap", walls
            ? nlohmann::json(maximum_gap) : nlohmann::json(nullptr)}};
        if (walls && (!proceduralScatterIsWallRegion(region)
                      || region_counts.generated != region.density_per_tile
                      || region_counts.accepted != region_counts.generated
                      || maximum_corner_distance > 16.0f
                      || maximum_gap > 32.0f))
          addIssue(report, "scatter.wall_chain", "chaîne discontinue: "
            + std::to_string(region_counts.accepted) + "/"
            + std::to_string(region_counts.generated) + ", coins="
            + std::to_string(maximum_corner_distance) + ", gap="
            + std::to_string(maximum_gap), region.name);
        if (!walls && !region_counts.accepted)
          addIssue(report, region.role == "canopy"
            ? "scatter.canopy" : "scatter.region_empty",
            "aucun candidat accepté après espacement", region.name);
      }
      if (!total_accepted)
        addIssue(report, walls ? "scatter.walls_empty" : "scatter.vegetation_empty",
                 "aucun candidat accepté");
      if (total_obstructions)
        addIssue(report, "scatter.playable_obstruction",
                 std::to_string(total_obstructions)
                   + " candidats sur le sol jouable: "
                   + joined(obstruction_details));
      return nlohmann::json{{"viable_before_spacing", total_viable},
                            {"accepted_after_spacing", total_accepted},
                            {"liquid_rejections", total_liquid_rejections},
                            {"playable_obstructions", total_obstructions},
                            {"obstruction_details", obstruction_details},
                            {"regions", std::move(region_metrics)}};
    };
    if (wall_scatter)
    {
      if (wall_scatter->assets.empty()
          || !wall_scatter->exclusions.empty()
          || std::any_of(wall_scatter->assets.begin(), wall_scatter->assets.end(),
               [](auto const& asset) { return asset.role != "wall"; }))
        addIssue(report, "scatter.wall_assets",
                 "batch de murs non isolé ou découpé par des exclusions");
      scatter_metrics["walls"] = auditScatter(*wall_scatter, true);
    }
    if (vegetation_scatter)
    {
      auto asset_roles = std::set<std::string>{};
      auto region_roles = std::set<std::string>{};
      for (auto const& asset : vegetation_scatter->assets)
        asset_roles.insert(asset.role);
      for (auto const& region : vegetation_scatter->regions)
        region_roles.insert(region.role);
      static auto const required_vegetation_roles = std::set<std::string>{
        "understory", "rock"};
      if (std::any_of(vegetation_scatter->assets.begin(),
                      vegetation_scatter->assets.end(),
            [](auto const& asset) { return asset.role == "wall"; })
          || !std::includes(asset_roles.begin(), asset_roles.end(),
                            required_vegetation_roles.begin(),
                            required_vegetation_roles.end())
          || !std::includes(region_roles.begin(), region_roles.end(),
                            required_vegetation_roles.begin(),
                            required_vegetation_roles.end()))
        addIssue(report, "scatter.vegetation_assets",
          "la végétation doit isoler understory et rock sans wall");
      auto const corridorExcluded = [&](ProceduralLayoutFeature const& feature)
      {
        for (std::size_t segment = 1; segment < feature.points.size(); ++segment)
        {
          auto const& first = feature.points[segment - 1];
          auto const& second = feature.points[segment];
          auto const du = second.u - first.u;
          auto const dv = second.v - first.v;
          auto const length = std::hypot(du, dv);
          if (length <= 0.0f) return false;
          auto const perpendicular_u = -dv / length;
          auto const perpendicular_v = du / length;
          auto const protected_radius = feature.half_width_ratio
              * (1.0f + feature.width_variation_ratio)
            + .5f * feature.transition_width_ratio + .003f;
          auto const steps = std::max(1, static_cast<int>(std::ceil(length / .005f)));
          for (int step = 0; step <= steps; ++step)
          {
            auto const t = static_cast<float>(step) / static_cast<float>(steps);
            auto const center_u = first.u + du * t;
            auto const center_v = first.v + dv * t;
            for (auto const offset : {-protected_radius, 0.0f, protected_radius})
              if (!proceduralScatterExcluded(*vegetation_scatter,
                    center_u + perpendicular_u * offset,
                    center_v + perpendicular_v * offset,
                    world_size, world_size))
                return false;
          }
        }
        return true;
      };
      for (auto const& route : routes)
      {
        if (!route.feature) continue;
        if (!corridorExcluded(*route.feature))
          addIssue(report, "scatter.route_exclusion", "route non protégée", route.name);
      }
      for (auto const& boundary : required_boundaries)
      {
        auto const* feature = findFeature(layout, boundary);
        if (feature && !corridorExcluded(*feature))
          addIssue(report, "scatter.route_exclusion",
                   "frontière non protégée", boundary);
      }
      for (auto const& camp : camps)
      {
        auto covered = proceduralScatterExcluded(
          *vegetation_scatter, camp.u, camp.v, world_size, world_size);
        for (auto const radius_ratio : {.5f, 1.0f})
          for (int sample = 0; sample < 48; ++sample)
          {
            auto const angle = static_cast<float>(sample)
              * 6.283185307179586f / 48.0f;
            auto const radius = camp.radius * radius_ratio
              + (radius_ratio == 1.0f ? .010f : 0.0f);
            covered = covered && proceduralScatterExcluded(*vegetation_scatter,
              camp.u + std::cos(angle) * radius,
              camp.v + std::sin(angle) * radius,
              world_size, world_size);
          }
        if (!covered)
          addIssue(report, "scatter.camp_exclusion", "clairière non protégée", camp.name);
      }
      for (auto const* collection : {&objectives, &bases})
        for (auto const& point : *collection)
        {
          if (!point.feature) continue;
          auto covered = proceduralScatterExcluded(
            *vegetation_scatter, point.u, point.v, world_size, world_size);
          for (auto const& corner : point.feature->points)
            covered = covered && proceduralScatterExcluded(*vegetation_scatter,
              corner.u, corner.v, world_size, world_size);
          if (!covered)
            addIssue(report, "scatter.route_exclusion",
                     "POI non protégé", point.name);
      }
      scatter_metrics["vegetation"] = auditScatter(*vegetation_scatter, false);
    }
    scatter_metrics["initial_occupancy"] = {
      {"props", props_occupancy_count},
      {"accepted_walls", preceding_instances.size() - props_occupancy_count}};
    scatter_metrics["oracle_limits"] = {
      {"preexisting_world_objects_included", false},
      {"mh2o_cells_included", true},
      {"absolute_tile_origin_included", true}};
    scatter_metrics["tile_origin"] = {
      {"min_tile_x", min_tile_x}, {"min_tile_z", min_tile_z}};
    auto const distance_to_blocked = squaredEuclideanDistance(
      blocked_sources, distance_resolution, distance_resolution);
    auto const distance_to_walkable = squaredEuclideanDistance(
      walkable_sources, distance_resolution, distance_resolution);
    std::vector<float> route_widths;
    auto corridor_width_metrics = nlohmann::json::object();
    auto narrow_routes = std::vector<std::string>{};
    for (auto const& feature : layout.features)
    {
      auto const path = feature.name.starts_with("jungle_")
        && feature.name.ends_with("_path");
      auto const cut = feature.name.ends_with("_wall_cut");
      if (!path && !cut) continue;
      auto const manifest_route = std::find_if(
        routes.begin(), routes.end(), [&](auto const& route)
        {
          return route.name == feature.name;
        });
      auto const declared_width = feature.half_width_ratio * 2.0f;
      auto const spur_throat = manifest_route != routes.end()
        && manifest_route->role == "spur";
      auto const compact_door = manifest_route != routes.end()
        && manifest_route->role == "door" && declared_width < .020f;
      auto const path_width_valid = spur_throat
        ? declared_width >= .010f && declared_width <= .014f
        : (compact_door
            ? declared_width >= .016f && declared_width <= .020f
            : declared_width >= .024f && declared_width <= .035f);
      if (path && !path_width_valid)
        addIssue(report, "corridor.declared_width", feature.name + " vaut "
          + std::to_string(declared_width));
      if (cut && (declared_width < .014f || declared_width > .022f))
        addIssue(report, "wall_cut.declared_width", feature.name + " vaut "
          + std::to_string(declared_width));
      if (!path) continue;
      auto feature_widths = std::vector<float>{};
      for (std::size_t part = 1; part < feature.points.size(); ++part)
        for (int step = 1; step < 12; ++step)
        {
          auto const t = static_cast<float>(step) / 12.0f;
          auto const u = feature.points[part - 1].u
            + (feature.points[part].u - feature.points[part - 1].u) * t;
          auto const v = feature.points[part - 1].v
            + (feature.points[part].v - feature.points[part - 1].v) * t;
          auto near_camp = false;
          for (auto const& camp : camps)
            near_camp = near_camp
              || std::hypot(u - camp.u, v - camp.v) < camp.radius + .025f;
          if (near_camp) continue;
          auto overlapping_routes = std::size_t{0};
          for (auto const& route : routes)
            if (route.feature && nearFeature(*route.feature, u, v, .002f))
              ++overlapping_routes;
          if (overlapping_routes > 1) continue;
          auto const distance = std::sqrt(distance_to_blocked[distancePixel(u, v)]);
          auto const physical_width = 2.0f * distance
            / static_cast<float>(distance_resolution);
          if (compact_door)
            route_widths.push_back(physical_width);
          feature_widths.push_back(physical_width);
        }
      auto const route_minimum = percentile(feature_widths, 0.0f);
      auto const route_p10 = percentile(feature_widths, .10f);
      auto const route_median = percentile(feature_widths, .50f);
      corridor_width_metrics[feature.name] = {
        {"minimum", route_minimum}, {"p10", route_p10},
        {"median", route_median}, {"compact_door", compact_door}};
      if (compact_door && route_p10 < .008f)
        narrow_routes.push_back(feature.name + "="
          + std::to_string(route_p10));
    }
    auto const width_p10 = percentile(route_widths, .10f);
    auto const width_median = percentile(route_widths, .50f);
    auto const width_p90 = percentile(route_widths, .90f);
    if (!narrow_routes.empty() || width_p10 < .008f
        || width_median < .010f || width_median > .060f)
      addIssue(report, "corridor.physical_width", "p10=" + std::to_string(width_p10)
        + ", médiane=" + std::to_string(width_median)
        + ", p90=" + std::to_string(width_p90)
        + ", routes étroites=" + joined(narrow_routes));

    std::vector<float> wall_thickness;
    std::map<std::string, std::vector<float>> wall_thickness_by_feature;
    for (std::size_t index = 0; index < triangle_count; ++index)
    {
      if (raw_walkable[index]) continue;
      auto const [u, v] = triangleCenter(index);
      auto const thickness = 2.0f
        * std::sqrt(distance_to_walkable[distancePixel(u, v)])
        / static_cast<float>(distance_resolution);
      auto in_wall = false;
      for (auto const* wall : wall_features)
        if (nearFeature(*wall, u, v, 0.0f))
        {
          in_wall = true;
          wall_thickness_by_feature[wall->name].push_back(thickness);
        }
      if (in_wall) wall_thickness.push_back(thickness);
    }
    auto const wall_median = percentile(wall_thickness, .50f);
    auto const wall_p95 = percentile(wall_thickness, .95f);
    auto const wall_maximum = percentile(wall_thickness, 1.0f);
    auto wall_feature_metrics = nlohmann::json::object();
    auto worst_wall = std::string{};
    auto worst_wall_p95 = 0.0f;
    auto thick_walls = std::vector<std::string>{};
    for (auto const& [name, values] : wall_thickness_by_feature)
    {
      auto const feature_p95 = percentile(values, .95f);
      auto const feature_maximum = percentile(values, 1.0f);
      wall_feature_metrics[name] = {{"p95", feature_p95},
                                    {"maximum", feature_maximum}};
      if (feature_p95 > worst_wall_p95)
      {
        worst_wall = name;
        worst_wall_p95 = feature_p95;
      }
      if (feature_p95 > .070f)
        thick_walls.push_back(name + "=" + std::to_string(feature_p95));
    }
    constexpr auto wall_exception_maximum = .095f;
    if (wall_median > .035f || wall_p95 > .070f
        || wall_maximum > wall_exception_maximum)
      addIssue(report, "wall.too_thick", "médiane=" + std::to_string(wall_median)
        + ", p95=" + std::to_string(wall_p95)
        + ", maximum=" + std::to_string(wall_maximum)
        + ", pire=" + worst_wall + " (" + std::to_string(worst_wall_p95)
        + "), masses=" + joined(thick_walls));
    if (wall_median < .015f)
      addIssue(report, "wall.too_thin", "médiane=" + std::to_string(wall_median)
        + " (cible SR 0.015–0.035)");

    auto geometry_symmetry_error = 0.0f;
    auto checkFeaturePair = [&](std::string const& first_name,
                                std::string const& second_name)
    {
      auto const* first = findFeature(layout, first_name);
      auto const* second = findFeature(layout, second_name);
      if (!first || !second)
      {
        addIssue(report, "symmetry.geometry", "paire absente " + first_name
          + " / " + second_name);
        geometry_symmetry_error = std::numeric_limits<float>::infinity();
        return;
      }
      auto const error = mirroredFeatureError(*first, *second);
      geometry_symmetry_error = std::max(
        geometry_symmetry_error, mirrorErrorMagnitude(error));
      if (!mirrorErrorAcceptable(error))
        addIssue(report, "symmetry.geometry",
          mirrorErrorDescription(error), first_name + " / " + second_name);
    };
    auto const oppositeQuadrant = [](std::string_view quadrant)
    {
      if (quadrant == "north") return std::string{"south"};
      if (quadrant == "east") return std::string{"west"};
      if (quadrant == "south") return std::string{"north"};
      return std::string{"east"};
    };
    auto const mirroredBoundary = [](std::string const& boundary)
    {
      if (boundary == "top_lane") return std::string{"bottom_lane"};
      if (boundary == "bottom_lane") return std::string{"top_lane"};
      return boundary;
    };
    for (auto const& route : routes)
    {
      if (route.quadrant != "north" && route.quadrant != "east") continue;
      auto counterparts = std::vector<Route const*>{};
      for (auto const& candidate : routes)
        if (candidate.quadrant == oppositeQuadrant(route.quadrant)
            && candidate.role == route.role)
          counterparts.push_back(&candidate);
      if (counterparts.size() != 1)
      {
        addIssue(report, "symmetry.pair", "route miroir absente ou ambiguë", route.name);
        geometry_symmetry_error = std::numeric_limits<float>::infinity();
        continue;
      }
      // The supplied SR reference is topologically balanced but not a literal
      // 180-degree tracing. Pair route roles and boundary contacts while
      // allowing each side to follow its own reference-safe negative space.
      auto mirrored_doors = std::multiset<std::string>{};
      for (auto const& boundary : route.doors)
        mirrored_doors.insert(mirroredBoundary(boundary));
      if (mirrored_doors != std::multiset<std::string>(
            counterparts.front()->doors.begin(), counterparts.front()->doors.end()))
        addIssue(report, "symmetry.boundaries", "portes miroir différentes", route.name);
    }
    // Summoner's Rift deliberately uses asymmetric wall silhouettes. Their
    // parity is guarded by the checked-in reference mask; competitive balance
    // is enforced below through entrances, camp reachability and the bounded
    // aggregate rotation mismatch instead of fictitious one-to-one wall pairs.
    checkFeaturePair("objective_north", "objective_south");
    checkFeaturePair("team_left_base_apron", "team_right_base_apron");
    checkFeaturePair("team_left_inner_court", "team_right_inner_court");
    checkFeaturePair("top_lane", "bottom_lane");
    checkFeaturePair("middle_lane", "middle_lane");
    checkFeaturePair("river_bed", "river_bed");
    for (auto const& camp : camps)
    {
      auto counterpart_quadrant = std::string{};
      if (camp.quadrant == "north") counterpart_quadrant = "south";
      else if (camp.quadrant == "east") counterpart_quadrant = "west";
      else continue;
      auto counterparts = std::vector<Camp const*>{};
      for (auto const& candidate : camps)
        if (candidate.quadrant == counterpart_quadrant
            && candidate.kind == camp.kind)
          counterparts.push_back(&candidate);
      if (counterparts.size() != 1)
      {
        addIssue(report, "symmetry.pair", "camp miroir absent ou ambigu", camp.name);
        geometry_symmetry_error = std::numeric_limits<float>::infinity();
        continue;
      }
      auto const* counterpart = counterparts.front();
      auto const error = std::hypot((1.0f - camp.u) - counterpart->u,
                                    (1.0f - camp.v) - counterpart->v);
      geometry_symmetry_error = std::max(geometry_symmetry_error, error);
      if (error > .00001f || std::abs(camp.radius - counterpart->radius) > .000001f)
        addIssue(report, "symmetry.geometry", "centre ou rayon différent",
                 camp.name + " / " + counterpart->name);
      checkFeaturePair(camp.feature, counterpart->feature);
    }

    auto symmetry_samples = std::size_t{0};
    auto symmetry_mismatches = std::size_t{0};
    for (std::size_t index = 0; index < triangle_count; ++index)
    {
      auto const cell = triangleCell(index);
      auto const mirror_side = (static_cast<int>(cell[2]) + 2) % 4;
      auto const mirror = triangleIndex(
        cells - 1 - cell[0], cells - 1 - cell[1], mirror_side);
      ++symmetry_samples;
      symmetry_mismatches += walkable[index] != walkable[mirror]
        || jungle_domain[index] != jungle_domain[mirror];
    }
    auto const symmetry_mismatch = symmetry_samples
      ? static_cast<float>(symmetry_mismatches) / static_cast<float>(symmetry_samples)
      : 1.0f;
    constexpr auto maximum_rotation_mismatch = .18f;
    if (symmetry_mismatch > maximum_rotation_mismatch)
      addIssue(report, "symmetry.rotation_mask",
               "désaccord walkability/jungle après rotation 180° : "
                 + std::to_string(symmetry_mismatch));

    auto enclosed_components = std::size_t{0};
    auto enclosed_triangles = std::size_t{0};
    auto enclosed_details = std::vector<std::string>{};
    auto component_seen = std::vector<std::uint8_t>(triangle_count);
    for (std::size_t start = 0; start < triangle_count; ++start)
    {
      if (!walkable[start] || globally_reached[start] || component_seen[start]) continue;
      auto component_size = std::size_t{0};
      auto center_u = 0.0f;
      auto center_v = 0.0f;
      auto queue = std::deque<std::size_t>{start};
      component_seen[start] = 1;
      while (!queue.empty())
      {
        auto const index = queue.front();
        queue.pop_front();
        ++component_size;
        auto const [u, v] = triangleCenter(index);
        center_u += u;
        center_v += v;
        forNeighbors(index, [&](std::size_t neighbor)
        {
          if (walkable[neighbor] && !globally_reached[neighbor]
              && !component_seen[neighbor])
          {
            component_seen[neighbor] = 1;
            queue.push_back(neighbor);
          }
        });
      }
      if (component_size < 16) continue;
      ++enclosed_components;
      enclosed_triangles += component_size;
      enclosed_details.push_back("(" + std::to_string(center_u / component_size)
        + "," + std::to_string(center_v / component_size) + ":"
        + std::to_string(component_size) + ")");
    }
    if (enclosed_components)
      addIssue(report, "navigation.enclosed_pocket", std::to_string(enclosed_components)
        + " poches, " + std::to_string(enclosed_triangles) + " triangles: "
        + joined(enclosed_details));

    auto unreachable_pois = std::vector<std::string>{};
    for (auto const& feature : layout.features)
    {
      if (!feature.name.starts_with("objective_")
          && !feature.name.ends_with("_inner_court"))
        continue;
      auto const [u, v] = centroid(feature);
      auto const index = triangleAt(u, v);
      if (!walkable[index] || !globally_reached[index])
        unreachable_pois.push_back(feature.name);
    }
    if (!unreachable_pois.empty())
      addIssue(report, "poi.unreachable", joined(unreachable_pois));

    report.metrics = {
      {"nav_cells_per_side", cells},
      {"nav_triangles", triangle_count},
      {"base_height", base_height},
      {"marginal_slope_triangles", marginal_triangles},
      {"feature_cores", std::move(feature_metrics)},
      {"textures", std::move(texture_metrics)},
      {"liquid", std::move(liquid_metrics)},
      {"props", std::move(props_metrics)},
      {"scatter", std::move(scatter_metrics)},
      {"jungle_green_coverage", jungle_texture_samples
        ? static_cast<double>(jungle_green_samples)
          / static_cast<double>(jungle_texture_samples) : 0.0},
      {"jungle_non_green_by_feature", jungle_non_green_by_feature},
      {"jungle_non_green_by_semantic", jungle_non_green_by_semantic},
      {"corridor_width_ratio", {{"p10", width_p10}, {"median", width_median},
                                  {"p90", width_p90}}},
      {"corridor_width_by_route", std::move(corridor_width_metrics)},
      {"boundary_openings", std::move(boundary_opening_metrics)},
      {"wall_thickness_ratio", {{"median", wall_median}, {"p95", wall_p95},
                                  {"maximum", wall_maximum},
                                  {"typical_minimum", .015},
                                  {"typical_maximum", .035},
                                  {"exception_maximum", wall_exception_maximum},
                                  {"features", std::move(wall_feature_metrics)}}},
      {"geometry_symmetry_error", geometry_symmetry_error},
      {"rotation_mismatch_ratio", symmetry_mismatch},
      {"maximum_rotation_mismatch_ratio", maximum_rotation_mismatch},
      {"enclosed_components", enclosed_components},
      {"enclosed_triangles", enclosed_triangles},
      {"camps", std::move(camp_metrics)},
      {"objectives", std::move(objective_metrics)},
      {"quadrants", std::move(quadrant_metrics)},
      {"unreachable_pois", unreachable_pois.size()}
    };

    report.preview_rgba.resize(
      report.preview_width * report.preview_height * std::size_t{4});
    for (std::size_t y = 0; y < preview_resolution; ++y)
      for (std::size_t x = 0; x < preview_resolution; ++x)
      {
        auto const u = (static_cast<float>(x) + .5f)
          / static_cast<float>(preview_resolution);
        auto const v = (static_cast<float>(y) + .5f)
          / static_cast<float>(preview_resolution);
        auto const nav_index = triangleAt(u, v);
        auto const sample = sampleProceduralLayout(
          layout, u, v, base_height, slopes[nav_index], world_size, world_size);
        auto dominant = std::string{"arena_ground"};
        auto dominant_mask = 0.0f;
        for (std::size_t feature = 0; feature < layout.features.size(); ++feature)
          if (sample.feature_masks[feature] >= dominant_mask)
          {
            dominant_mask = sample.feature_masks[feature];
            dominant = layout.features[feature].name;
          }
        auto const semantic = semanticForName(dominant, raw_walkable[nav_index]);
        auto const color = semanticColor(semantic);
        auto const height = sampleHeight(u, v);
        auto const shade = std::clamp(.72f + (height - base_height) / 45.0f,
                                      .45f, 1.0f);
        auto const writePixel = [&](std::size_t panel,
                                    std::array<std::uint8_t, 3> rgb)
        {
          auto const pixel = (y * report.preview_width
            + panel * preview_resolution + x) * 4;
          report.preview_rgba[pixel] = rgb[0];
          report.preview_rgba[pixel + 1] = rgb[1];
          report.preview_rgba[pixel + 2] = rgb[2];
          report.preview_rgba[pixel + 3] = 255;
        };
        writePixel(0, {static_cast<std::uint8_t>(color[0] * shade),
                       static_cast<std::uint8_t>(color[1] * shade),
                       static_cast<std::uint8_t>(color[2] * shade)});
        writePixel(1, color);
      }

    auto const blendPixel = [&](std::size_t panel, int x, int y,
                                std::array<std::uint8_t, 3> color,
                                std::uint8_t alpha)
    {
      if (x < 0 || y < 0
          || x >= static_cast<int>(preview_resolution)
          || y >= static_cast<int>(preview_resolution))
        return;
      auto const pixel = (static_cast<std::size_t>(y) * report.preview_width
        + panel * preview_resolution + static_cast<std::size_t>(x)) * 4;
      for (std::size_t channel = 0; channel < 3; ++channel)
        report.preview_rgba[pixel + channel] = static_cast<std::uint8_t>(
          (static_cast<unsigned>(report.preview_rgba[pixel + channel])
             * (255U - alpha)
           + static_cast<unsigned>(color[channel]) * alpha + 127U) / 255U);
    };
    auto const previewPoint = [preview_resolution](float u, float v)
    {
      auto const maximum = static_cast<float>(preview_resolution - 1);
      return std::pair{
        static_cast<int>(std::lround(std::clamp(u, 0.0f, 1.0f) * maximum)),
        static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * maximum))};
    };
    auto const drawMarker = [&](std::size_t panel, float u, float v, int radius,
                                std::array<std::uint8_t, 3> color,
                                std::uint8_t alpha)
    {
      auto const [x, y] = previewPoint(u, v);
      for (int offset = -radius; offset <= radius; ++offset)
      {
        blendPixel(panel, x + offset, y, color, alpha);
        blendPixel(panel, x, y + offset, color, alpha);
      }
    };
    auto const drawLine = [&](std::size_t panel,
                              ProceduralLayoutPoint const& first,
                              ProceduralLayoutPoint const& second,
                              std::array<std::uint8_t, 3> color)
    {
      auto [x0, y0] = previewPoint(first.u, first.v);
      auto const [x1, y1] = previewPoint(second.u, second.v);
      auto const dx = std::abs(x1 - x0);
      auto const sx = x0 < x1 ? 1 : -1;
      auto const dy = -std::abs(y1 - y0);
      auto const sy = y0 < y1 ? 1 : -1;
      auto error = dx + dy;
      while (true)
      {
        blendPixel(panel, x0, y0, color, 150);
        if (x0 == x1 && y0 == y1) break;
        auto const twice = error * 2;
        if (twice >= dy)
        {
          error += dy;
          x0 += sx;
        }
        if (twice <= dx)
        {
          error += dx;
          y0 += sy;
        }
      }
    };
    auto const drawRegions = [&](ProceduralScatter const& scatter, bool walls)
    {
      auto const color = walls ? std::array<std::uint8_t, 3>{245, 115, 35}
                               : std::array<std::uint8_t, 3>{95, 205, 70};
      for (auto const& region : scatter.regions)
      {
        for (std::size_t index = 1; index < region.points.size(); ++index)
          drawLine(1, region.points[index - 1], region.points[index], color);
        if (!walls && region.points.size() > 2)
          drawLine(1, region.points.back(), region.points.front(), color);
      }
    };

    for (std::size_t y = 0; y < preview_resolution; ++y)
      for (std::size_t x = 0; x < preview_resolution; ++x)
      {
        auto const u = (static_cast<float>(x) + .5f)
          / static_cast<float>(preview_resolution);
        auto const v = (static_cast<float>(y) + .5f)
          / static_cast<float>(preview_resolution);
        if (!liquidCellOccupied(u, v)) continue;
        blendPixel(0, static_cast<int>(x), static_cast<int>(y),
                   {20, 195, 245}, 155);
        blendPixel(1, static_cast<int>(x), static_cast<int>(y),
                   {20, 195, 245}, 95);
      }
    if (wall_scatter) drawRegions(*wall_scatter, true);
    if (vegetation_scatter) drawRegions(*vegetation_scatter, false);
    for (auto const accepted : {false, true})
      for (auto const& candidate : scatter_preview)
      {
        if (candidate.accepted != accepted) continue;
        auto const color = candidate.walls
          ? std::array<std::uint8_t, 3>{255, 155, 35}
          : std::array<std::uint8_t, 3>{150, 255, 75};
        drawMarker(1, candidate.u, candidate.v, accepted ? 1 : 0,
                   color, accepted ? 230 : 45);
      }
    if (procedural_props)
      for (auto const& prop : procedural_props->props)
      {
        auto const light = prop.name.find("_glow") != std::string::npos
          || prop.name.ends_with("_flame");
        drawMarker(1, prop.u, prop.v, light ? 1 : 2,
                   light ? std::array<std::uint8_t, 3>{255, 235, 120}
                         : std::array<std::uint8_t, 3>{255, 70, 220},
                   255);
      }
    report.metrics["preview_overlays"] = {
      {"liquid_cells", active_liquid_cells},
      {"scatter_candidates", scatter_preview.size()},
      {"props", procedural_props ? procedural_props->props.size() : 0}};
    return report;
  }

  bool MobaArenaAuditReport::hasIssue(std::string_view code) const noexcept
  {
    return std::any_of(issues.begin(), issues.end(), [&](auto const& issue)
    {
      return issue.code == code;
    });
  }

  nlohmann::json mobaArenaAuditSummary(MobaArenaAuditReport const& report)
  {
    auto issues = nlohmann::json::array();
    for (auto const& issue : report.issues)
      issues.push_back({{"code", issue.code}, {"subject", issue.subject},
                        {"message", issue.message}});
    return {{"ok", report.ok()}, {"metrics", report.metrics},
            {"issues", std::move(issues)},
            {"preview", {{"width", report.preview_width},
                         {"height", report.preview_height}}}};
  }
}
