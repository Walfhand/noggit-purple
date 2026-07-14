// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/GroundEffectPreview.hpp>

#include <algorithm>
#include <cctype>
#include <string>

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
    auto const density = std::min(amount ? amount : 8u, 24u);
    auto const count = density * 64u;
    auto const total_weight = weights[0] + weights[1] + weights[2] + weights[3];
    std::vector<GroundEffectPreviewChoice> result;
    result.reserve(count);
    for (unsigned i = 0; i < count; ++i)
    {
      auto const splat = i / density;
      auto const cell = hash(seed + splat * 37u) & 63u;
      auto choice = total_weight
        ? hash(seed + splat * 11u + 1u) % total_weight : 0u;
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
        unit(hash(seed + i * 11u + 4u)) * 360.0f,
        0.67f + unit(hash(seed + i * 11u + 5u)) * 0.66f
      });
    }
    return result;
  }

  int groundEffectAssetScore(std::string_view path)
  {
    std::string lower(path);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
    {
      return static_cast<char>(std::tolower(c));
    });
    auto contains = [&](std::string_view value)
    {
      return lower.find(value) != std::string::npos;
    };

    auto score = 0;
    if (contains("grass") || contains("gra")) score += 10000;
    if (contains("fern") || contains("groundcover")) score += 1500;
    if (contains("8kulgrass")) score += 110000;
    else if (contains("8zulgrass")) score += 100000;
    else if (contains("8rivgrass")) score += 80000;
    else if (contains("8drkgrass")) score += 60000;
    else if (contains("8") && contains("grass")) score += 50000;
    else if (contains("7") && contains("grass")) score += 30000;
    else if (contains("6") && contains("grass")) score += 15000;
    else if (contains("elwgra")) score += 25000;
    else if (contains("wesgra")) score -= 10000;
    if (contains("flower") || contains("flo")) score -= 5000;
    if (contains("dry") || contains("dead") || contains("ashy")
        || contains("nightmare") || contains("fel")) score -= 40000;
    return score;
  }
}
