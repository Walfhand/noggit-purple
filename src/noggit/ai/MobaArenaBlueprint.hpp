// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Noggit::Ai
{
  // The canonical SR river joins the two lane junctions; it deliberately does
  // not touch the outer map borders.
  inline constexpr double moba_arena_minimum_liquid_span_u = .55;
  inline constexpr double moba_arena_minimum_liquid_span_v = .60;

  nlohmann::json defaultMobaArenaSpecification();

  std::optional<std::string> validateMobaArenaFootprint(
    std::vector<std::pair<std::size_t, std::size_t>> const& tiles);

  nlohmann::json compileMobaArenaBlueprint(
    nlohmann::json const& arguments,
    std::size_t footprint_side_tiles,
    std::size_t min_tile_x = 0,
    std::size_t min_tile_z = 0);
}
