// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_AI_PROCEDURAL_PROPS_HPP
#define NOGGIT_AI_PROCEDURAL_PROPS_HPP

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Noggit::Ai
{
  inline constexpr std::size_t procedural_props_max_count = 256;

  struct ProceduralProp
  {
    std::string name;
    std::string path;
    float u = 0.0f;
    float v = 0.0f;
    float scale = 1.0f;
    float yaw_degrees = 0.0f;
    float height_offset = 0.0f;
  };

  struct ProceduralProps
  {
    std::vector<ProceduralProp> props;
  };

  struct ProceduralPropsParseResult
  {
    std::optional<ProceduralProps> props;
    std::string error;
  };

  ProceduralPropsParseResult parseProceduralProps(nlohmann::json const& arguments);
}

#endif
