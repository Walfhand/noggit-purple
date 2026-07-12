// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/BlueprintSnapshot.hpp>

#include <system_error>

namespace Noggit::Ai
{
  namespace
  {
    std::optional<std::string> validatePaths(
      std::filesystem::path const& source, std::filesystem::path const& target)
    {
      if (source.empty() || target.empty()) return "Les chemins du snapshot sont vides.";
      if (std::filesystem::absolute(source).lexically_normal()
          == std::filesystem::absolute(target).lexically_normal())
        return "La source et la destination du snapshot sont identiques.";
      if (!std::filesystem::is_directory(source))
        return "Le dossier source du snapshot n'existe pas : " + source.string();
      return std::nullopt;
    }

    std::string errorMessage(char const* action, std::error_code const& error)
    {
      return std::string(action) + " : " + error.message();
    }
  }

  std::optional<std::string> captureBlueprintSnapshot(
    std::filesystem::path const& source, std::filesystem::path const& snapshot)
  {
    if (auto error = validatePaths(source, snapshot)) return error;
    if (std::filesystem::exists(snapshot)) return "Le snapshot existe déjà.";

    auto const temporary = snapshot.string() + ".tmp";
    std::error_code error;
    std::filesystem::remove_all(temporary, error);
    error.clear();
    std::filesystem::create_directories(snapshot.parent_path(), error);
    if (error) return errorMessage("Impossible de créer le dossier des snapshots", error);
    std::filesystem::copy(source, temporary, std::filesystem::copy_options::recursive, error);
    if (error)
    {
      std::filesystem::remove_all(temporary);
      return errorMessage("Impossible de copier la baseline", error);
    }
    std::filesystem::rename(temporary, snapshot, error);
    if (error)
    {
      std::filesystem::remove_all(temporary);
      return errorMessage("Impossible de finaliser la baseline", error);
    }
    return std::nullopt;
  }

  std::optional<std::string> restoreBlueprintSnapshot(
    std::filesystem::path const& snapshot, std::filesystem::path const& target)
  {
    if (auto error = validatePaths(snapshot, target)) return error;
    if (!std::filesystem::is_directory(target))
      return "Le dossier de la carte n'existe pas : " + target.string();

    auto const temporary = std::filesystem::path(target.string() + ".regenerate-tmp");
    auto const previous = std::filesystem::path(target.string() + ".regenerate-previous");
    std::error_code error;
    std::filesystem::remove_all(temporary, error);
    std::filesystem::remove_all(previous, error);
    error.clear();
    std::filesystem::copy(snapshot, temporary, std::filesystem::copy_options::recursive, error);
    if (error)
    {
      std::filesystem::remove_all(temporary);
      return errorMessage("Impossible de préparer la régénération", error);
    }
    std::filesystem::rename(target, previous, error);
    if (error)
    {
      std::filesystem::remove_all(temporary);
      return errorMessage("Impossible de mettre la carte actuelle de côté", error);
    }
    std::filesystem::rename(temporary, target, error);
    if (error)
    {
      auto const restore_error = error;
      std::filesystem::rename(previous, target, error);
      std::filesystem::remove_all(temporary);
      return errorMessage("Impossible de restaurer la baseline", restore_error);
    }
    std::filesystem::remove_all(previous, error);
    return std::nullopt;
  }
}
