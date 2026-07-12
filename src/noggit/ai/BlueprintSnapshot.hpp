// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Noggit::Ai
{
  std::optional<std::string> captureBlueprintSnapshot(
    std::filesystem::path const& source, std::filesystem::path const& snapshot);

  std::optional<std::string> restoreBlueprintSnapshot(
    std::filesystem::path const& snapshot, std::filesystem::path const& target);
}
