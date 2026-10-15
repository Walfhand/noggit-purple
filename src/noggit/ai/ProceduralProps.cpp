// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/ProceduralProps.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
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
  }

  ProceduralPropsParseResult parseProceduralProps(nlohmann::json const& arguments)
  {
    static auto const root_fields = std::set<std::string>{"props"};
    static auto const prop_fields = std::set<std::string>{
      "name", "path", "u", "v", "scale", "yaw_degrees", "height_offset"};
    auto fail = [](std::string error)
    {
      return ProceduralPropsParseResult{std::nullopt, std::move(error)};
    };

    if (!exactFields(arguments, root_fields))
      return fail("place_props_on_map exige exactement props.");
    auto const& values = arguments.at("props");
    if (!values.is_array() || values.empty()
        || values.size() > procedural_props_max_count)
      return fail("props doit contenir entre 1 et 256 placements.");

    ProceduralProps props;
    std::set<std::string> names;
    for (auto const& value : values)
    {
      ProceduralProp prop;
      if (!exactFields(value, prop_fields) || !value.at("name").is_string()
          || !value.at("path").is_string()
          || !finiteFloat(value.at("u"), prop.u)
          || !finiteFloat(value.at("v"), prop.v)
          || !finiteFloat(value.at("scale"), prop.scale)
          || !finiteFloat(value.at("yaw_degrees"), prop.yaw_degrees)
          || !finiteFloat(value.at("height_offset"), prop.height_offset))
        return fail("Chaque prop exige name, path, u, v, scale, yaw_degrees et height_offset valides.");
      prop.name = value.at("name").get<std::string>();
      prop.path = value.at("path").get<std::string>();
      if (prop.name.empty() || prop.name.size() > 64 || !printableAscii(prop.name)
          || !names.insert(prop.name).second
          || prop.path.empty() || prop.path.size() > 260 || !printableAscii(prop.path)
          || prop.u < 0.0f || prop.u > 1.0f || prop.v < 0.0f || prop.v > 1.0f
          || prop.scale < 0.05f || prop.scale > 10.0f
          || prop.yaw_degrees < 0.0f || prop.yaw_degrees >= 360.0f
          || prop.height_offset < -100.0f || prop.height_offset > 100.0f)
        return fail("Nom unique, chemin, coordonnées, échelle, orientation ou offset de prop invalide.");
      props.props.push_back(std::move(prop));
    }
    return {std::move(props), {}};
  }
}
