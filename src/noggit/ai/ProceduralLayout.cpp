// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/ProceduralLayout.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
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
    constexpr std::size_t max_points_per_feature = 16;
    constexpr std::size_t max_total_segments = 516;
    constexpr float minimum_layout_height = -500.0f;
    constexpr float maximum_layout_height = 5000.0f;

    bool hasExactFields(nlohmann::json const& object, std::set<std::string> const& fields)
    {
      if (!object.is_object() || object.size() != fields.size())
      {
        return false;
      }

      for (auto const& [name, value] : object.items())
      {
        static_cast<void>(value);
        if (!fields.contains(name))
        {
          return false;
        }
      }
      return true;
    }

    bool readInteger(nlohmann::json const& value, std::int64_t& result)
    {
      if (!value.is_number_integer())
      {
        return false;
      }
      if (value.is_number_unsigned())
      {
        auto const number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
          return false;
        }
        result = static_cast<std::int64_t>(number);
        return true;
      }

      result = value.get<std::int64_t>();
      return true;
    }

    bool readFiniteFloat(nlohmann::json const& value, float& result)
    {
      if (!value.is_number())
      {
        return false;
      }

      auto const number = value.get<double>();
      if (!std::isfinite(number)
          || number < -static_cast<double>(std::numeric_limits<float>::max())
          || number > static_cast<double>(std::numeric_limits<float>::max()))
      {
        return false;
      }

      result = static_cast<float>(number);
      return true;
    }

    bool isPrintableAscii(std::string const& value)
    {
      return std::all_of(value.begin(), value.end(), [](unsigned char character)
      {
        return character >= 32 && character <= 126;
      });
    }

    float smooth(float start, float end, float value)
    {
      if (end <= start)
      {
        return value >= end ? 1.0f : 0.0f;
      }

      auto const ratio = std::clamp((value - start) / (end - start), 0.0f, 1.0f);
      return ratio * ratio * (3.0f - 2.0f * ratio);
    }

    float cross(
      ProceduralLayoutPoint const& a,
      ProceduralLayoutPoint const& b,
      ProceduralLayoutPoint const& c)
    {
      return (b.u - a.u) * (c.v - a.v) - (b.v - a.v) * (c.u - a.u);
    }

    bool pointOnSegment(
      ProceduralLayoutPoint const& point,
      ProceduralLayoutPoint const& a,
      ProceduralLayoutPoint const& b)
    {
      constexpr float epsilon = 1.0e-6f;
      return std::abs(cross(a, b, point)) <= epsilon
        && point.u >= std::min(a.u, b.u) - epsilon
        && point.u <= std::max(a.u, b.u) + epsilon
        && point.v >= std::min(a.v, b.v) - epsilon
        && point.v <= std::max(a.v, b.v) + epsilon;
    }

    bool segmentsIntersect(
      ProceduralLayoutPoint const& a,
      ProceduralLayoutPoint const& b,
      ProceduralLayoutPoint const& c,
      ProceduralLayoutPoint const& d)
    {
      auto const ab_c = cross(a, b, c);
      auto const ab_d = cross(a, b, d);
      auto const cd_a = cross(c, d, a);
      auto const cd_b = cross(c, d, b);
      constexpr float epsilon = 1.0e-6f;
      auto opposite = [=](float first, float second)
      {
        return (first > epsilon && second < -epsilon)
          || (first < -epsilon && second > epsilon);
      };
      if (opposite(ab_c, ab_d) && opposite(cd_a, cd_b))
      {
        return true;
      }
      return (std::abs(ab_c) <= epsilon && pointOnSegment(c, a, b))
        || (std::abs(ab_d) <= epsilon && pointOnSegment(d, a, b))
        || (std::abs(cd_a) <= epsilon && pointOnSegment(a, c, d))
        || (std::abs(cd_b) <= epsilon && pointOnSegment(b, c, d));
    }

    bool isSimpleAreaImpl(std::vector<ProceduralLayoutPoint> const& points)
    {
      if (points.size() < 3)
      {
        return false;
      }

      constexpr float epsilon = 1.0e-6f;
      auto signed_area = 0.0f;
      for (std::size_t index = 0; index < points.size(); ++index)
      {
        auto const& current = points[index];
        auto const& next = points[(index + 1) % points.size()];
        signed_area += current.u * next.v - next.u * current.v;
        if (std::abs(current.height - points.front().height) > epsilon)
        {
          return false;
        }
        for (std::size_t other = index + 1; other < points.size(); ++other)
        {
          if (current.u == points[other].u && current.v == points[other].v)
          {
            return false;
          }
        }
      }
      if (std::abs(signed_area) <= epsilon)
      {
        return false;
      }

      for (std::size_t first = 0; first < points.size(); ++first)
      {
        auto const first_next = (first + 1) % points.size();
        for (std::size_t second = first + 1; second < points.size(); ++second)
        {
          auto const second_next = (second + 1) % points.size();
          if (first == second || first_next == second || second_next == first)
          {
            continue;
          }
          if (segmentsIntersect(
                points[first], points[first_next],
                points[second], points[second_next]))
          {
            return false;
          }
        }
      }
      return true;
    }

    std::uint32_t stableHash(std::string const& value)
    {
      auto hash = std::uint32_t{2166136261U};
      for (auto const character : value)
      {
        hash ^= static_cast<unsigned char>(character);
        hash *= 16777619U;
      }
      return hash;
    }

    float gridNoise(int x, int z, std::uint32_t seed)
    {
      auto value = static_cast<std::uint32_t>(x) * 0x9e3779b9U
        ^ static_cast<std::uint32_t>(z) * 0x85ebca6bU ^ seed;
      value ^= value >> 16U;
      value *= 0x7feb352dU;
      value ^= value >> 15U;
      value *= 0x846ca68bU;
      value ^= value >> 16U;
      return static_cast<float>(value & 0x00ffffffU) / 8388607.5f - 1.0f;
    }

    float valueNoise(float x, float z, std::uint32_t seed)
    {
      auto const x0 = static_cast<int>(std::floor(x));
      auto const z0 = static_cast<int>(std::floor(z));
      auto const tx = smooth(0.0f, 1.0f, x - static_cast<float>(x0));
      auto const tz = smooth(0.0f, 1.0f, z - static_cast<float>(z0));
      auto const top = gridNoise(x0, z0, seed)
        + (gridNoise(x0 + 1, z0, seed) - gridNoise(x0, z0, seed)) * tx;
      auto const bottom = gridNoise(x0, z0 + 1, seed)
        + (gridNoise(x0 + 1, z0 + 1, seed) - gridNoise(x0, z0 + 1, seed)) * tx;
      return top + (bottom - top) * tz;
    }

    float edgeNoiseImpl(float x, float z, std::string const& name)
    {
      auto const seed = stableHash(name);
      return 0.65f * valueNoise(x * 6.0f, z * 6.0f, seed)
        + 0.35f * valueNoise(x * 15.0f, z * 15.0f, seed ^ 0xa511e9b3U);
    }

    float terrainRoughness(float x, float z, std::string const& name)
    {
      auto const seed = stableHash(name);
      return 0.72f * valueNoise(x * 8.0f, z * 8.0f, seed ^ 0x51ed270bU)
        + 0.28f * valueNoise(x * 27.0f, z * 27.0f, seed ^ 0x9e3779b9U);
    }

    ProceduralShapeDistance distanceToShapeImpl(
      std::vector<ProceduralLayoutPoint> const& points,
      ProceduralLayoutShape shape,
      float sample_x,
      float sample_z,
      float scale_x,
      float scale_z)
    {
      auto scaledPoint = [&](ProceduralLayoutPoint const& point)
      {
        return std::array<float, 2>{point.u * scale_x, point.v * scale_z};
      };

      if (points.empty())
      {
        return {std::numeric_limits<float>::max(), 0.0f};
      }

      if (shape == ProceduralLayoutShape::Corridor && points.size() == 1)
      {
        auto const point = scaledPoint(points.front());
        return {
          std::hypot(sample_x - point[0], sample_z - point[1]),
          points.front().height
        };
      }

      ProceduralShapeDistance result{std::numeric_limits<float>::max(), 0.0f};
      auto const segment_count = shape == ProceduralLayoutShape::Area
        ? points.size() : points.size() - 1;
      for (std::size_t index = 0; index < segment_count; ++index)
      {
        auto const& first = points[index];
        auto const& second = points[(index + 1) % points.size()];
        auto const a = scaledPoint(first);
        auto const b = scaledPoint(second);
        auto const dx = b[0] - a[0];
        auto const dz = b[1] - a[1];
        auto const length_squared = dx * dx + dz * dz;
        auto const projection = length_squared > 0.0f
          ? std::clamp(((sample_x - a[0]) * dx + (sample_z - a[1]) * dz)
                         / length_squared,
                       0.0f, 1.0f)
          : 0.0f;
        auto const nearest_x = a[0] + dx * projection;
        auto const nearest_z = a[1] + dz * projection;
        auto const distance = std::hypot(sample_x - nearest_x, sample_z - nearest_z);
        if (distance < result.distance)
        {
          result.distance = distance;
          result.height = first.height + (second.height - first.height) * projection;
        }
      }

      if (shape == ProceduralLayoutShape::Area)
      {
        auto inside = false;
        for (std::size_t current = 0, previous = points.size() - 1;
             current < points.size(); previous = current++)
        {
          auto const a = scaledPoint(points[current]);
          auto const b = scaledPoint(points[previous]);
          if ((a[1] > sample_z) != (b[1] > sample_z)
              && sample_x < (b[0] - a[0]) * (sample_z - a[1])
                   / (b[1] - a[1]) + a[0])
          {
            inside = !inside;
          }
        }
        result.distance = inside ? -result.distance : result.distance;
        result.height = points.front().height;
      }

      return result;
    }

    float shapeMaskImpl(
      float half_width,
      float transition_width,
      float distance)
    {
      if (distance <= half_width)
      {
        return 1.0f;
      }
      if (transition_width == 0.0f)
      {
        return 0.0f;
      }

      return 1.0f - smooth(
        half_width,
        half_width + transition_width,
        distance);
    }

    std::array<std::uint8_t, procedural_layout_max_texture_paths> quantizeWeights(
      std::array<float, procedural_layout_max_texture_paths> weights,
      std::size_t layer_count)
    {
      std::array<std::uint8_t, procedural_layout_max_texture_paths> result{};
      std::array<float, procedural_layout_max_texture_paths> remainders{};
      auto total = 0.0f;
      for (std::size_t layer = 0; layer < layer_count; ++layer)
      {
        weights[layer] = std::clamp(weights[layer], 0.0f, 1.0f);
        total += weights[layer];
      }
      if (total <= 0.0f)
      {
        weights[0] = 1.0f;
        total = 1.0f;
      }

      auto assigned = 0;
      for (std::size_t layer = 0; layer < layer_count; ++layer)
      {
        auto const scaled = weights[layer] / total * 255.0f;
        auto const integral = static_cast<int>(std::floor(scaled));
        result[layer] = static_cast<std::uint8_t>(integral);
        remainders[layer] = scaled - static_cast<float>(integral);
        assigned += integral;
      }

      while (assigned < 255)
      {
        auto best_layer = std::size_t{0};
        for (std::size_t layer = 1; layer < layer_count; ++layer)
        {
          if (remainders[layer] > remainders[best_layer])
          {
            best_layer = layer;
          }
        }
        ++result[best_layer];
        remainders[best_layer] = -1.0f;
        ++assigned;
      }

      return result;
    }
  }

  bool isSimpleProceduralArea(std::vector<ProceduralLayoutPoint> const& points)
  {
    return isSimpleAreaImpl(points);
  }

  ProceduralShapeDistance distanceToProceduralShape(
    std::vector<ProceduralLayoutPoint> const& points,
    ProceduralLayoutShape shape,
    float sample_x,
    float sample_z,
    float scale_x,
    float scale_z)
  {
    return distanceToShapeImpl(
      points, shape, sample_x, sample_z, scale_x, scale_z);
  }

  float proceduralShapeMask(
    float half_width_ratio,
    float transition_width_ratio,
    float distance)
  {
    return shapeMaskImpl(
      half_width_ratio, transition_width_ratio, distance);
  }

  float proceduralEdgeNoise(float x, float z, std::string const& name)
  {
    return edgeNoiseImpl(x, z, name);
  }

  ProceduralLayoutParseResult parseProceduralLayout(nlohmann::json const& arguments)
  {
    static auto const root_fields = std::set<std::string>{
      "texture_paths", "steep_texture_layer", "slope_start_degrees",
      "slope_full_degrees", "edge_noise_ratio", "max_slope_degrees",
      "smoothing_strength", "features"
    };
    static auto const feature_fields = std::set<std::string>{
      "name", "points", "half_width_ratio", "transition_width_ratio",
      "texture_layer", "priority", "shape", "height_mode",
      "roughness_amplitude", "texture_strength", "width_variation_ratio"
    };
    static auto const point_fields = std::set<std::string>{"u", "v", "height"};

    auto fail = [](std::string error)
    {
      return ProceduralLayoutParseResult{std::nullopt, std::move(error)};
    };

    if (!hasExactFields(arguments, root_fields))
    {
      return fail("Le layout exige exactement texture_paths, steep_texture_layer, "
                  "slope_start_degrees, slope_full_degrees, edge_noise_ratio, "
                  "max_slope_degrees, smoothing_strength et features.");
    }

    auto const& texture_paths = arguments.at("texture_paths");
    if (!texture_paths.is_array()
        || texture_paths.size() < 2
        || texture_paths.size() > procedural_layout_max_texture_paths)
    {
      return fail("texture_paths doit contenir entre deux et 16 chemins.");
    }

    ProceduralLayout layout;
    std::set<std::string> unique_paths;
    for (auto const& path_value : texture_paths)
    {
      if (!path_value.is_string())
      {
        return fail("Chaque entrée de texture_paths doit être une chaîne.");
      }
      auto path = path_value.get<std::string>();
      if (path.empty() || path.size() > 260
          || !unique_paths.insert(path).second)
      {
        return fail("Les chemins de texture doivent être non vides, uniques et limités à 260 caractères.");
      }
      layout.texture_paths.emplace_back(std::move(path));
    }

    auto const& steep_layer = arguments.at("steep_texture_layer");
    auto const& slope_start = arguments.at("slope_start_degrees");
    auto const& slope_full = arguments.at("slope_full_degrees");
    auto const all_steep_null = steep_layer.is_null()
      && slope_start.is_null() && slope_full.is_null();
    auto const any_steep_null = steep_layer.is_null()
      || slope_start.is_null() || slope_full.is_null();
    if (!all_steep_null)
    {
      if (any_steep_null || !steep_layer.is_number_integer())
      {
        return fail("Les trois paramètres de pente doivent être définis ensemble ou tous être null.");
      }

      std::int64_t layer = 0;
      float start = 0.0f;
      float full = 0.0f;
      if (!readInteger(steep_layer, layer)
          || layer < 0 || static_cast<std::size_t>(layer) >= layout.texture_paths.size()
          || !readFiniteFloat(slope_start, start)
          || !readFiniteFloat(slope_full, full)
          || start < 0.0f || full > 90.0f || start >= full)
      {
        return fail("La couche de pente est invalide ou ses angles doivent vérifier 0 <= début < fin <= 90.");
      }
      layout.steep = ProceduralLayoutSteep{
        static_cast<std::size_t>(layer), start, full
      };
    }

    if (!readFiniteFloat(arguments.at("edge_noise_ratio"), layout.edge_noise_ratio)
        || layout.edge_noise_ratio < 0.0f || layout.edge_noise_ratio > 0.05f
        || !readFiniteFloat(arguments.at("smoothing_strength"), layout.smoothing_strength)
        || layout.smoothing_strength < 0.0f || layout.smoothing_strength > 1.0f)
    {
      return fail("edge_noise_ratio doit être dans [0,0.05] et smoothing_strength dans [0,1].");
    }
    auto const& max_slope = arguments.at("max_slope_degrees");
    if (!max_slope.is_null())
    {
      float degrees = 0.0f;
      if (!readFiniteFloat(max_slope, degrees) || degrees < 5.0f || degrees > 89.0f)
      {
        return fail("max_slope_degrees doit être null ou compris dans [5,89].");
      }
      layout.max_slope_degrees = degrees;
    }

    auto const& features = arguments.at("features");
    if (!features.is_array() || features.empty()
        || features.size() > procedural_layout_max_features)
    {
      return fail("features doit contenir entre une et 64 formes.");
    }

    std::set<std::string> unique_names;
    std::size_t total_segments = 0;
    std::array<bool, procedural_layout_max_texture_paths> used_layers{};
    used_layers[0] = true;
    if (layout.steep)
    {
      used_layers[layout.steep->texture_layer] = true;
    }
    for (auto const& feature_value : features)
    {
      if (!hasExactFields(feature_value, feature_fields))
      {
        return fail("Chaque feature exige exactement name, points, half_width_ratio, "
                    "transition_width_ratio, texture_layer, priority, shape, height_mode "
                    "roughness_amplitude, texture_strength et width_variation_ratio.");
      }

      auto const& name_value = feature_value.at("name");
      auto const& points = feature_value.at("points");
      if (!name_value.is_string() || !points.is_array()
          || points.empty() || points.size() > max_points_per_feature)
      {
        return fail("Chaque feature doit avoir un nom et entre un et 16 points.");
      }

      ProceduralLayoutFeature feature;
      auto const& shape = feature_value.at("shape");
      auto const& height_mode = feature_value.at("height_mode");
      if (!shape.is_string() || !height_mode.is_string())
      {
        return fail("shape et height_mode doivent être des chaînes.");
      }
      auto const shape_name = shape.get<std::string>();
      auto const height_mode_name = height_mode.get<std::string>();
      if (shape_name == "corridor")
      {
        feature.shape = ProceduralLayoutShape::Corridor;
      }
      else if (shape_name == "area")
      {
        feature.shape = ProceduralLayoutShape::Area;
      }
      else
      {
        return fail("shape doit valoir corridor ou area.");
      }
      if (height_mode_name == "absolute")
      {
        feature.height_mode = ProceduralLayoutHeightMode::Absolute;
      }
      else if (height_mode_name == "offset")
      {
        feature.height_mode = ProceduralLayoutHeightMode::Offset;
      }
      else
      {
        return fail("height_mode doit valoir absolute ou offset.");
      }
      if (feature.shape == ProceduralLayoutShape::Area && points.size() < 3)
      {
        return fail("Une area exige entre trois et 16 points.");
      }

      feature.name = name_value.get<std::string>();
      if (feature.name.empty() || feature.name.size() > 64
          || !isPrintableAscii(feature.name)
          || !unique_names.insert(feature.name).second)
      {
        return fail("Les noms de feature doivent être ASCII imprimables, uniques et limités à 64 caractères.");
      }

      total_segments += feature.shape == ProceduralLayoutShape::Area
        ? points.size() : points.size() - 1;
      if (total_segments > max_total_segments)
      {
        return fail("Le layout ne peut pas dépasser 516 segments au total.");
      }
      for (auto const& point_value : points)
      {
        if (!hasExactFields(point_value, point_fields))
        {
          return fail("Chaque point exige exactement u, v et height.");
        }

        ProceduralLayoutPoint point{};
        if (!readFiniteFloat(point_value.at("u"), point.u)
            || !readFiniteFloat(point_value.at("v"), point.v)
            || !readFiniteFloat(point_value.at("height"), point.height)
            || point.u < 0.0f || point.u > 1.0f
            || point.v < 0.0f || point.v > 1.0f
            || point.height < minimum_layout_height
            || point.height > maximum_layout_height)
        {
          return fail("u et v doivent être dans [0,1] et height dans [-500,5000].");
        }
        if (!feature.points.empty()
            && feature.points.back().u == point.u
            && feature.points.back().v == point.v)
        {
          return fail("Deux points consécutifs d'une feature ne peuvent pas avoir les mêmes coordonnées.");
        }
        feature.points.emplace_back(point);
      }

      if (feature.shape == ProceduralLayoutShape::Area
          && !isSimpleProceduralArea(feature.points))
      {
        return fail("Une area doit être un polygone simple, non dégénéré, avec la même hauteur à chaque point.");
      }

      if (!readFiniteFloat(feature_value.at("half_width_ratio"), feature.half_width_ratio)
          || !readFiniteFloat(feature_value.at("transition_width_ratio"), feature.transition_width_ratio)
          || feature.half_width_ratio
               < (feature.shape == ProceduralLayoutShape::Area ? 0.0f : 0.00125f)
          || feature.half_width_ratio > 0.25f
          || feature.transition_width_ratio < 0.00025f
          || feature.transition_width_ratio > 0.25f)
      {
        return fail("half_width_ratio doit être dans [0,0.25] pour area ou [0.00125,0.25] pour corridor, et transition_width_ratio dans [0.00025,0.25].");
      }

      auto const& texture_layer = feature_value.at("texture_layer");
      auto const& priority = feature_value.at("priority");
      std::int64_t layer = 0;
      std::int64_t parsed_priority = 0;
      if (!readInteger(texture_layer, layer) || !readInteger(priority, parsed_priority))
      {
        return fail("texture_layer et priority doivent être des entiers.");
      }
      if (layer < 0 || static_cast<std::size_t>(layer) >= layout.texture_paths.size()
          || parsed_priority < std::numeric_limits<int>::min()
          || parsed_priority > std::numeric_limits<int>::max()
          || parsed_priority < 0 || parsed_priority > 100)
      {
        return fail("texture_layer est hors palette ou priority n'est pas dans [0,100].");
      }
      feature.texture_layer = static_cast<std::size_t>(layer);
      feature.priority = static_cast<int>(parsed_priority);
      if (!readFiniteFloat(
            feature_value.at("roughness_amplitude"), feature.roughness_amplitude)
          || feature.roughness_amplitude < 0.0f
          || feature.roughness_amplitude > 100.0f)
      {
        return fail("roughness_amplitude doit être dans [0,100].");
      }
      if (!readFiniteFloat(
            feature_value.at("texture_strength"), feature.texture_strength)
          || feature.texture_strength < 0.05f || feature.texture_strength > 1.0f)
      {
        return fail("texture_strength doit être dans [0.05,1].");
      }
      if (!readFiniteFloat(
            feature_value.at("width_variation_ratio"), feature.width_variation_ratio)
          || feature.width_variation_ratio < 0.0f
          || feature.width_variation_ratio > 0.75f)
      {
        return fail("width_variation_ratio doit être dans [0,0.75].");
      }
      used_layers[feature.texture_layer] = true;
      feature.min_u = feature.max_u = feature.points.front().u;
      feature.min_v = feature.max_v = feature.points.front().v;
      feature.min_height = feature.max_height = feature.points.front().height;
      for (auto const& bound_point : feature.points)
      {
        feature.min_u = std::min(feature.min_u, bound_point.u);
        feature.max_u = std::max(feature.max_u, bound_point.u);
        feature.min_v = std::min(feature.min_v, bound_point.v);
        feature.max_v = std::max(feature.max_v, bound_point.v);
        feature.min_height = std::min(feature.min_height, bound_point.height);
        feature.max_height = std::max(feature.max_height, bound_point.height);
      }
      layout.features.emplace_back(std::move(feature));
    }

    for (std::size_t layer = 0; layer < layout.texture_paths.size(); ++layer)
    {
      if (!used_layers[layer])
      {
        return fail("Chaque texture de la palette doit être utilisée par la base, une feature ou la pente.");
      }
    }

    std::stable_sort(layout.features.begin(), layout.features.end(),
      [](ProceduralLayoutFeature const& left, ProceduralLayoutFeature const& right)
      {
        return left.priority < right.priority;
      });

    return {std::move(layout), {}};
  }

  ProceduralLayoutSample sampleProceduralLayout(
    ProceduralLayout const& layout,
    float u,
    float v,
    float original_height,
    float slope_degrees,
    float map_width,
    float map_height)
  {
    ProceduralLayoutSample sample{
      original_height,
      {1.0f, 0.0f, 0.0f, 0.0f},
      {255, 0, 0, 0},
      {}
    };
    if (layout.texture_paths.empty())
    {
      return sample;
    }

    auto const layer_count = std::min(
      layout.texture_paths.size(), procedural_layout_max_texture_paths);
    auto const valid_dimensions = std::isfinite(map_width) && std::isfinite(map_height)
      && map_width > 0.0f && map_height > 0.0f;
    auto const minimum_dimension = valid_dimensions
      ? std::min(map_width, map_height) : 1.0f;
    auto const scale_x = valid_dimensions ? map_width / minimum_dimension : 1.0f;
    auto const scale_z = valid_dimensions ? map_height / minimum_dimension : 1.0f;
    auto const normalized_u = std::isfinite(u) ? std::clamp(u, 0.0f, 1.0f) : 0.0f;
    auto const normalized_v = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    auto const sample_x = normalized_u * scale_x;
    auto const sample_z = normalized_v * scale_z;

    auto const feature_count = std::min(
      layout.features.size(), procedural_layout_max_features);
    constexpr float degrees_to_radians_gradient = 0.017453292519943295f;
    auto const slope_widening_gradient = layout.max_slope_degrees
        && std::isfinite(*layout.max_slope_degrees)
        && *layout.max_slope_degrees >= 5.0f
        && *layout.max_slope_degrees <= 89.0f
      ? std::optional<float>(std::tan(
          *layout.max_slope_degrees * degrees_to_radians_gradient))
      : std::nullopt;
    // Bounding-box cull with a conservative influence reach (widened
    // transitions and edge noise included) keeps large layouts affordable:
    // the per-sample cost is dominated by the few features actually nearby.
    for (std::size_t index = 0; index < feature_count; ++index)
    {
      auto const& feature = layout.features[index];
      if (feature.points.empty())
      {
        continue;
      }
      {
        auto const height_delta_bound = feature.roughness_amplitude
          + (feature.height_mode == ProceduralLayoutHeightMode::Offset
             ? std::max(std::abs(feature.min_height), std::abs(feature.max_height))
             : std::max(std::abs(feature.min_height - sample.height),
                        std::abs(feature.max_height - sample.height)));
        auto transition_bound = feature.transition_width_ratio;
        if (slope_widening_gradient)
        {
          transition_bound = std::max(transition_bound,
            1.5f * height_delta_bound
              / (*slope_widening_gradient * minimum_dimension));
        }
        auto const reach = feature.half_width_ratio
            * (1.0f + feature.width_variation_ratio)
          + transition_bound + layout.edge_noise_ratio;
        auto const outside_x = std::max({feature.min_u * scale_x - sample_x,
                                         sample_x - feature.max_u * scale_x,
                                         0.0f});
        auto const outside_z = std::max({feature.min_v * scale_z - sample_z,
                                         sample_z - feature.max_v * scale_z,
                                         0.0f});
        if (outside_x * outside_x + outside_z * outside_z > reach * reach)
        {
          continue;
        }
      }
      auto const nearest = distanceToProceduralShape(
        feature.points, feature.shape, sample_x, sample_z, scale_x, scale_z);
      auto distance = nearest.distance;
      auto half_width = feature.half_width_ratio;
      if (feature.shape == ProceduralLayoutShape::Corridor
          && feature.width_variation_ratio > 0.0f)
      {
        half_width *= 1.0f + feature.width_variation_ratio
          * proceduralEdgeNoise(sample_x, sample_z, feature.name + ":width");
      }
      auto const roughness = feature.roughness_amplitude
        * terrainRoughness(sample_x, sample_z, feature.name);
      auto const target_height = feature.height_mode == ProceduralLayoutHeightMode::Offset
        ? std::clamp(sample.height + nearest.height + roughness,
                     minimum_layout_height, maximum_layout_height)
        : std::clamp(nearest.height + roughness,
                     minimum_layout_height, maximum_layout_height);
      auto transition_width = feature.transition_width_ratio;
      // ponytail: analytic widening bounds the generated transition on a flat
      // source; use a global vertex graph only if arbitrary source slopes need
      // a hard guarantee.
      if (layout.max_slope_degrees
          && std::isfinite(*layout.max_slope_degrees)
          && *layout.max_slope_degrees >= 5.0f
          && *layout.max_slope_degrees <= 89.0f)
      {
        constexpr float degrees_to_radians = 0.017453292519943295f;
        auto const maximum_gradient = std::tan(
          *layout.max_slope_degrees * degrees_to_radians);
        auto const minimum_transition = 1.5f
          * std::abs(target_height - sample.height)
          / (maximum_gradient * minimum_dimension);
        transition_width = std::max(transition_width, minimum_transition);
      }
      if (layout.edge_noise_ratio > 0.0f)
      {
        auto const amplitude = std::min(
          layout.edge_noise_ratio, half_width * 0.75f);
        if (distance <= half_width + transition_width + amplitude)
        {
          distance += amplitude
            * proceduralEdgeNoise(sample_x, sample_z, feature.name);
        }
      }
      auto const mask = proceduralShapeMask(
        half_width, transition_width, distance);
      sample.feature_masks[index] = mask;
      if (mask <= 0.0f)
      {
        continue;
      }

      // ponytail: transitions are one-shot; preserve source heights only if
      // repeatable layout reapplication becomes a real editing workflow.
      sample.height += (target_height - sample.height) * mask;
      auto texture_mask = mask;
      if (feature.texture_strength < 1.0f)
      {
        auto const variation = 0.85f + 0.15f
          * terrainRoughness(sample_x, sample_z, feature.name + ":texture");
        texture_mask *= feature.texture_strength * variation;
      }
      for (std::size_t layer = 0; layer < layer_count; ++layer)
      {
        sample.semantic_weights[layer] *= 1.0f - texture_mask;
      }
      if (feature.texture_layer < layer_count)
      {
        sample.semantic_weights[feature.texture_layer] += texture_mask;
      }
      else
      {
        sample.semantic_weights[0] += texture_mask;
      }
    }

    auto remaining_influence = 1.0f;
    for (auto index = feature_count; index-- > 0;)
    {
      auto const geometric_mask = sample.feature_masks[index];
      sample.feature_masks[index] = geometric_mask * remaining_influence;
      remaining_influence *= 1.0f - geometric_mask;
    }

    if (layout.steep && layout.steep->texture_layer < layer_count)
    {
      auto const steep_weight = smooth(
        layout.steep->slope_start_degrees,
        layout.steep->slope_full_degrees,
        slope_degrees);
      for (std::size_t layer = 0; layer < layer_count; ++layer)
      {
        sample.semantic_weights[layer] *= 1.0f - steep_weight;
      }
      sample.semantic_weights[layout.steep->texture_layer] += steep_weight;
    }

    sample.quantized_weights = quantizeWeights(
      sample.semantic_weights,
      layer_count);
    return sample;
  }

  float sampleSmoothedProceduralLayoutHeight(
    ProceduralLayout const& layout,
    float u,
    float v,
    float original_height,
    float map_width,
    float map_height,
    float sample_spacing_world)
  {
    auto const center_sample = sampleProceduralLayout(
      layout, u, v, original_height, 0.0f, map_width, map_height);
    auto const center = center_sample.height;
    if (layout.smoothing_strength <= 0.0f
        || !std::isfinite(sample_spacing_world) || sample_spacing_world <= 0.0f
        || !std::isfinite(map_width) || map_width <= 0.0f
        || !std::isfinite(map_height) || map_height <= 0.0f)
    {
      return center;
    }
    if (std::any_of(
          center_sample.feature_masks.begin(), center_sample.feature_masks.end(),
          [](float mask) { return mask >= 0.999f; }))
    {
      return center;
    }
    if (std::none_of(
          center_sample.feature_masks.begin(), center_sample.feature_masks.end(),
          [](float mask) { return mask > 0.0f; }))
    {
      return center;
    }

    auto const du = sample_spacing_world / map_width;
    auto const dv = sample_spacing_world / map_height;
    // ponytail: filter only the analytic displacement to stay streaming and
    // seam-safe; a true source-terrain blur would require a global snapshot.
    auto filtered_delta = 0.5f * (center - original_height);
    for (auto const& offset : std::array<std::array<float, 2>, 4>{
           std::array<float, 2>{-du, -dv}, {du, -dv}, {-du, dv}, {du, dv}})
    {
      auto const neighbor = sampleProceduralLayout(
        layout,
        std::clamp(u + offset[0], 0.0f, 1.0f),
        std::clamp(v + offset[1], 0.0f, 1.0f),
        original_height, 0.0f, map_width, map_height).height;
      filtered_delta += 0.125f * (neighbor - original_height);
    }

    auto const center_delta = center - original_height;
    return original_height + center_delta
      + (filtered_delta - center_delta)
        * std::clamp(layout.smoothing_strength, 0.0f, 1.0f);
  }
}
