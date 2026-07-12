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
  std::optional<std::string> validateMobaArenaFootprint(
    std::vector<std::pair<std::size_t, std::size_t>> const& tiles);

  nlohmann::json compileMobaArenaBlueprint(nlohmann::json const& arguments);
}
