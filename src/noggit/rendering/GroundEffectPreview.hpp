// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Noggit::Rendering
{
  struct GroundEffectPreviewChoice
  {
    std::uint8_t x = 0;
    std::uint8_t z = 0;
    std::size_t doodad = 0;
    float jitter_x = 0.0f;
    float jitter_z = 0.0f;
    float yaw_degrees = 0.0f;
    float scale = 1.0f;
  };

  std::vector<GroundEffectPreviewChoice> groundEffectPreviewChoices(
    unsigned amount, std::array<unsigned, 4> const& weights,
    std::uint32_t seed);

  int groundEffectAssetScore(std::string_view path);
}
