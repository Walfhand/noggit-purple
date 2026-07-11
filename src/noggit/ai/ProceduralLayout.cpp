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
    constexpr std::size_t max_texture_paths = 4;
    constexpr std::size_t max_points_per_feature = 16;
    constexpr std::size_t max_total_segments = 128;

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

    struct FeatureDistance
    {
      float distance = std::numeric_limits<float>::max();
      float height = 0.0f;
    };

    FeatureDistance distanceToFeature(
      ProceduralLayoutFeature const& feature,
      float sample_x,
      float sample_z,
      float scale_x,
      float scale_z)
    {
      auto scaledPoint = [&](ProceduralLayoutPoint const& point)
      {
        return std::array<float, 2>{point.u * scale_x, point.v * scale_z};
      };

      if (feature.points.size() == 1)
      {
        auto const point = scaledPoint(feature.points.front());
        return {
          std::hypot(sample_x - point[0], sample_z - point[1]),
          feature.points.front().height
        };
      }

      FeatureDistance result;
      for (std::size_t index = 1; index < feature.points.size(); ++index)
      {
        auto const& first = feature.points[index - 1];
        auto const& second = feature.points[index];
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

      return result;
    }

    float featureMask(ProceduralLayoutFeature const& feature, float distance)
    {
      if (distance <= feature.half_width_ratio)
      {
        return 1.0f;
      }
      if (feature.transition_width_ratio == 0.0f)
      {
        return 0.0f;
      }

      return 1.0f - smooth(
        feature.half_width_ratio,
        feature.half_width_ratio + feature.transition_width_ratio,
        distance);
    }

    std::array<std::uint8_t, 4> quantizeWeights(
      std::array<float, 4> weights,
      std::size_t layer_count)
    {
      std::array<std::uint8_t, 4> result{};
      std::array<float, 4> remainders{};
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

  ProceduralLayoutParseResult parseProceduralLayout(nlohmann::json const& arguments)
  {
    static auto const root_fields = std::set<std::string>{
      "texture_paths", "steep_texture_layer", "slope_start_degrees",
      "slope_full_degrees", "features"
    };
    static auto const feature_fields = std::set<std::string>{
      "name", "points", "half_width_ratio", "transition_width_ratio",
      "texture_layer", "priority"
    };
    static auto const point_fields = std::set<std::string>{"u", "v", "height"};

    auto fail = [](std::string error)
    {
      return ProceduralLayoutParseResult{std::nullopt, std::move(error)};
    };

    if (!hasExactFields(arguments, root_fields))
    {
      return fail("Le layout exige exactement texture_paths, steep_texture_layer, "
                  "slope_start_degrees, slope_full_degrees et features.");
    }

    auto const& texture_paths = arguments.at("texture_paths");
    if (!texture_paths.is_array()
        || texture_paths.size() < 2 || texture_paths.size() > max_texture_paths)
    {
      return fail("texture_paths doit contenir entre deux et quatre chemins.");
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

    auto const& features = arguments.at("features");
    if (!features.is_array() || features.empty()
        || features.size() > procedural_layout_max_features)
    {
      return fail("features doit contenir entre une et 32 formes.");
    }

    std::set<std::string> unique_names;
    std::size_t total_segments = 0;
    std::array<bool, max_texture_paths> used_layers{};
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
                    "transition_width_ratio, texture_layer et priority.");
      }

      auto const& name_value = feature_value.at("name");
      auto const& points = feature_value.at("points");
      if (!name_value.is_string() || !points.is_array()
          || points.empty() || points.size() > max_points_per_feature)
      {
        return fail("Chaque feature doit avoir un nom et entre un et 16 points.");
      }

      ProceduralLayoutFeature feature;
      feature.name = name_value.get<std::string>();
      if (feature.name.empty() || feature.name.size() > 64
          || !isPrintableAscii(feature.name)
          || !unique_names.insert(feature.name).second)
      {
        return fail("Les noms de feature doivent être ASCII imprimables, uniques et limités à 64 caractères.");
      }

      total_segments += points.size() - 1;
      if (total_segments > max_total_segments)
      {
        return fail("Le layout ne peut pas dépasser 128 segments au total.");
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
            || point.height < -500.0f || point.height > 5000.0f)
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

      if (!readFiniteFloat(feature_value.at("half_width_ratio"), feature.half_width_ratio)
          || !readFiniteFloat(feature_value.at("transition_width_ratio"), feature.transition_width_ratio)
          || feature.half_width_ratio < 0.005f || feature.half_width_ratio > 0.25f
          || feature.transition_width_ratio < 0.001f
          || feature.transition_width_ratio > 0.25f)
      {
        return fail("half_width_ratio doit être dans [0.005,0.25] et transition_width_ratio dans [0.001,0.25].");
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
      used_layers[feature.texture_layer] = true;
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

    auto const layer_count = std::min(layout.texture_paths.size(), max_texture_paths);
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
    // ponytail: O(samples * segments) is bounded by 128 segments; add a
    // spatial index only if profiling shows large layouts need it.
    for (std::size_t index = 0; index < feature_count; ++index)
    {
      auto const& feature = layout.features[index];
      if (feature.points.empty())
      {
        continue;
      }
      auto const nearest = distanceToFeature(
        feature, sample_x, sample_z, scale_x, scale_z);
      auto const mask = featureMask(feature, nearest.distance);
      sample.feature_masks[index] = mask;
      if (mask <= 0.0f)
      {
        continue;
      }

      // ponytail: transitions are one-shot; preserve source heights only if
      // repeatable layout reapplication becomes a real editing workflow.
      sample.height += (nearest.height - sample.height) * mask;
      for (std::size_t layer = 0; layer < layer_count; ++layer)
      {
        sample.semantic_weights[layer] *= 1.0f - mask;
      }
      if (feature.texture_layer < layer_count)
      {
        sample.semantic_weights[feature.texture_layer] += mask;
      }
      else
      {
        sample.semantic_weights[0] += mask;
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
}
