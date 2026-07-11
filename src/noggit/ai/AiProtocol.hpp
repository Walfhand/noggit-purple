// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Noggit::Ai
{
  struct FunctionCall
  {
    std::string call_id;
    std::string name;
    std::string arguments;
  };

  nlohmann::json toolDefinitions();
  std::vector<FunctionCall> functionCalls(nlohmann::json const& response);
  std::string outputText(nlohmann::json const& response);
  float terrainRatio(std::string_view preset, float noise, float edge_distance);
  std::array<std::uint8_t, 4> textureBlendAlphas(
    float height, float slope_degrees, float noise,
    float low_height, float high_height, bool has_high,
    float blend_width, float slope_start_degrees,
    float slope_full_degrees, float noise_strength);
}
