// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/GroundEffectPreview.hpp>

#include <algorithm>

namespace Noggit::Rendering
{
  namespace
  {
    std::uint32_t hash(std::uint32_t value)
    {
      value ^= value >> 16;
      value *= 0x7feb352du;
      value ^= value >> 15;
      value *= 0x846ca68bu;
      return value ^ (value >> 16);
    }

    float unit(std::uint32_t value)
    {
      return static_cast<float>(value & 0x00ffffffu) / 16777216.0f;
    }
  }

  std::vector<GroundEffectPreviewChoice> groundEffectPreviewChoices(
    unsigned amount, std::array<unsigned, 4> const& weights,
    std::uint32_t seed)
  {
    auto const count = std::min(amount ? amount : 8u, 16u);
    auto const total_weight = weights[0] + weights[1] + weights[2] + weights[3];
    std::vector<GroundEffectPreviewChoice> result;
    result.reserve(count);
    for (unsigned i = 0; i < count; ++i)
    {
      auto const cell = (hash(seed) + i * 37u) & 63u;
      auto choice = total_weight ? hash(seed + i * 11u + 1u) % total_weight : 0u;
      std::size_t doodad = 0;
      while (doodad + 1 < weights.size() && choice >= weights[doodad])
      {
        choice -= weights[doodad++];
      }
      result.push_back({
        static_cast<std::uint8_t>(cell & 7u),
        static_cast<std::uint8_t>(cell >> 3u), doodad,
        unit(hash(seed + i * 11u + 2u)) - 0.5f,
        unit(hash(seed + i * 11u + 3u)) - 0.5f,
        unit(hash(seed + i * 11u + 4u)) * 360.0f
      });
    }
    return result;
  }
}
