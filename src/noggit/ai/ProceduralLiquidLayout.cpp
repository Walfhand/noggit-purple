// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/ProceduralLiquidLayout.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace Noggit::Ai
{
  namespace
  {
    constexpr std::size_t max_points_per_feature = 16;
    constexpr std::size_t max_total_segments = 128;

    bool hasExactFields(
      nlohmann::json const& object,
      std::set<std::string> const& fields)
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

    bool readInteger(nlohmann::json const& value, std::int64_t& result)
    {
      if (!value.is_number_integer())
      {
        return false;
      }
      if (value.is_number_unsigned())
      {
        auto const number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()))
        {
          return false;
        }
        result = static_cast<std::int64_t>(number);
        return true;
      }
      result = value.get<std::int64_t>();
      return true;
    }

    bool isPrintableAscii(std::string const& value)
    {
      return std::all_of(value.begin(), value.end(), [](unsigned char character)
      {
        return character >= 32 && character <= 126;
      });
    }
  }

  ProceduralLiquidLayoutParseResult parseProceduralLiquidLayout(
    nlohmann::json const& arguments)
  {
    static auto const root_fields = std::set<std::string>{
      "replace_existing", "edge_noise_ratio", "features"
    };
    static auto const feature_fields = std::set<std::string>{
      "name", "shape", "points", "half_width_ratio",
      "transition_width_ratio", "liquid_type_id", "depth", "priority"
    };
    static auto const point_fields = std::set<std::string>{"u", "v", "height"};

    auto fail = [](std::string error)
    {
      return ProceduralLiquidLayoutParseResult{
        std::nullopt, std::move(error)};
    };

    if (!hasExactFields(arguments, root_fields))
    {
      return fail("Le layout liquide exige exactement replace_existing, "
                  "edge_noise_ratio et features.");
    }

    ProceduralLiquidLayout layout;
    if (!arguments.at("replace_existing").is_boolean())
    {
      return fail("replace_existing doit être un booléen.");
    }
    layout.replace_existing = arguments.at("replace_existing").get<bool>();
    if (!readFiniteFloat(arguments.at("edge_noise_ratio"), layout.edge_noise_ratio)
        || layout.edge_noise_ratio < 0.0f || layout.edge_noise_ratio > 0.05f)
    {
      return fail("edge_noise_ratio doit être dans [0,0.05].");
    }

    auto const& features = arguments.at("features");
    if (!features.is_array() || features.empty()
        || features.size() > procedural_layout_max_features)
    {
      return fail("features doit contenir entre une et 64 formes.");
    }

    std::set<std::string> unique_names;
    std::set<std::uint16_t> unique_liquid_types;
    std::size_t total_segments = 0;
    for (auto const& feature_value : features)
    {
      if (!hasExactFields(feature_value, feature_fields))
      {
        return fail("Chaque feature liquide exige exactement name, shape, points, "
                    "half_width_ratio, transition_width_ratio, liquid_type_id, "
                    "depth et priority.");
      }

      auto const& name_value = feature_value.at("name");
      auto const& shape_value = feature_value.at("shape");
      auto const& points = feature_value.at("points");
      if (!name_value.is_string() || !shape_value.is_string()
          || !points.is_array() || points.empty()
          || points.size() > max_points_per_feature)
      {
        return fail("Chaque feature liquide doit avoir un nom, une forme et entre un et 16 points.");
      }

      ProceduralLiquidLayoutFeature feature;
      feature.name = name_value.get<std::string>();
      if (feature.name.empty() || feature.name.size() > 64
          || !isPrintableAscii(feature.name)
          || !unique_names.insert(feature.name).second)
      {
        return fail("Les noms de feature doivent être ASCII imprimables, uniques et limités à 64 caractères.");
      }

      auto const shape_name = shape_value.get<std::string>();
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
      if (feature.shape == ProceduralLayoutShape::Area && points.size() < 3)
      {
        return fail("Une area liquide exige entre trois et 16 points.");
      }

      total_segments += feature.shape == ProceduralLayoutShape::Area
        ? points.size() : points.size() - 1;
      if (total_segments > max_total_segments)
      {
        return fail("Le layout liquide ne peut pas dépasser 128 segments au total.");
      }

      for (auto const& point_value : points)
      {
        if (!hasExactFields(point_value, point_fields))
        {
          return fail("Chaque point liquide exige exactement u, v et height.");
        }
        ProceduralLayoutPoint point;
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
          return fail("Deux points consécutifs d'une feature liquide ne peuvent pas être identiques.");
        }
        feature.points.push_back(point);
      }

      if (feature.shape == ProceduralLayoutShape::Area
          && !isSimpleProceduralArea(feature.points))
      {
        return fail("Une area liquide doit être un polygone simple, non dégénéré, avec la même hauteur à chaque point.");
      }

      if (!readFiniteFloat(
            feature_value.at("half_width_ratio"), feature.half_width_ratio)
          || !readFiniteFloat(
            feature_value.at("transition_width_ratio"),
            feature.transition_width_ratio)
          || feature.half_width_ratio < 0.00125f
          || feature.half_width_ratio > 0.25f
          || feature.transition_width_ratio < 0.00025f
          || feature.transition_width_ratio > 0.25f)
      {
        return fail("half_width_ratio doit être dans [0.00125,0.25] et transition_width_ratio dans [0.00025,0.25].");
      }

      std::int64_t liquid_type_id = 0;
      std::int64_t priority = 0;
      if (!readInteger(feature_value.at("liquid_type_id"), liquid_type_id)
          || liquid_type_id < 1 || liquid_type_id > 65535
          || !readFiniteFloat(feature_value.at("depth"), feature.depth)
          || feature.depth < 0.01f || feature.depth > 1.0f
          || !readInteger(feature_value.at("priority"), priority)
          || priority < 0 || priority > 100)
      {
        return fail("liquid_type_id doit être dans [1,65535], depth dans [0.01,1] et priority dans [0,100].");
      }
      feature.liquid_type_id = static_cast<std::uint16_t>(liquid_type_id);
      unique_liquid_types.insert(feature.liquid_type_id);
      if (unique_liquid_types.size() > procedural_liquid_max_distinct_types)
      {
        return fail("Un layout liquide ne peut pas utiliser plus de 14 types distincts.");
      }
      feature.priority = static_cast<int>(priority);
      layout.features.push_back(std::move(feature));
    }

    std::stable_sort(layout.features.begin(), layout.features.end(),
      [](ProceduralLiquidLayoutFeature const& left,
         ProceduralLiquidLayoutFeature const& right)
      {
        return left.priority < right.priority;
      });
    return {std::move(layout), {}};
  }

  ProceduralLiquidLayoutSample sampleProceduralLiquidFeature(
    ProceduralLiquidLayout const& layout,
    std::size_t feature_index,
    float u,
    float v,
    float map_width,
    float map_height)
  {
    ProceduralLiquidLayoutSample sample;
    if (feature_index >= layout.features.size()
        || feature_index >= procedural_layout_max_features)
    {
      return sample;
    }
    auto const valid_dimensions = std::isfinite(map_width)
      && std::isfinite(map_height) && map_width > 0.0f && map_height > 0.0f;
    auto const minimum_dimension = valid_dimensions
      ? std::min(map_width, map_height) : 1.0f;
    auto const scale_x = valid_dimensions ? map_width / minimum_dimension : 1.0f;
    auto const scale_z = valid_dimensions ? map_height / minimum_dimension : 1.0f;
    auto const normalized_u = std::isfinite(u) ? std::clamp(u, 0.0f, 1.0f) : 0.0f;
    auto const normalized_v = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    auto const sample_x = normalized_u * scale_x;
    auto const sample_z = normalized_v * scale_z;

    auto const& feature = layout.features[feature_index];
    if (feature.points.empty() || feature.liquid_type_id == 0
        || !std::isfinite(feature.depth) || feature.depth <= 0.0f)
    {
      return sample;
    }
    auto const nearest = distanceToProceduralShape(
      feature.points, feature.shape, sample_x, sample_z, scale_x, scale_z);
    if (!std::isfinite(nearest.distance) || !std::isfinite(nearest.height))
    {
      return sample;
    }
    auto distance = nearest.distance;
    if (layout.edge_noise_ratio > 0.0f
        && std::isfinite(layout.edge_noise_ratio))
    {
      auto const amplitude = std::min(
        layout.edge_noise_ratio, feature.half_width_ratio * 0.75f);
      if (distance <= feature.half_width_ratio
                        + feature.transition_width_ratio + amplitude)
      {
        distance += amplitude
          * proceduralEdgeNoise(sample_x, sample_z, feature.name);
      }
    }
    auto const mask = proceduralShapeMask(
      feature.half_width_ratio, feature.transition_width_ratio, distance);
    sample.has_liquid = mask > 0.0f;
    sample.feature_index = feature_index;
    sample.liquid_type_id = feature.liquid_type_id;
    sample.height = nearest.height;
    sample.mask = mask;
    sample.depth = std::clamp(feature.depth, 0.0f, 1.0f) * mask;
    return sample;
  }

  ProceduralLiquidLayoutSample sampleProceduralLiquidLayout(
    ProceduralLiquidLayout const& layout,
    float u,
    float v,
    float map_width,
    float map_height)
  {
    ProceduralLiquidLayoutSample sample;
    auto best_priority = std::numeric_limits<int>::min();
    auto const feature_count = std::min(
      layout.features.size(), procedural_layout_max_features);
    for (std::size_t index = 0; index < feature_count; ++index)
    {
      auto candidate = sampleProceduralLiquidFeature(
        layout, index, u, v, map_width, map_height);
      if (!candidate.has_liquid
          || layout.features[index].priority < best_priority)
      {
        continue;
      }
      best_priority = layout.features[index].priority;
      sample = std::move(candidate);
    }
    return sample;
  }
}
