// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Noggit::Ai
{
  inline constexpr std::size_t moba_arena_audit_preview_resolution = 256;

  struct MobaArenaAuditIssue
  {
    std::string code;
    std::string subject;
    std::string message;
  };

  struct MobaArenaAuditReport
  {
    std::size_t preview_resolution = moba_arena_audit_preview_resolution;
    std::size_t preview_width = moba_arena_audit_preview_resolution * 2;
    std::size_t preview_height = moba_arena_audit_preview_resolution;
    std::vector<std::uint8_t> preview_rgba;
    nlohmann::json metrics = nlohmann::json::object();
    std::vector<MobaArenaAuditIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return issues.empty(); }
    [[nodiscard]] bool hasIssue(std::string_view code) const noexcept;
  };

  MobaArenaAuditReport auditMobaArenaBlueprint(
    nlohmann::json const& blueprint,
    std::size_t footprint_side_tiles,
    std::size_t preview_resolution = moba_arena_audit_preview_resolution,
    std::size_t min_tile_x = 0,
    std::size_t min_tile_z = 0);

  nlohmann::json mobaArenaAuditSummary(MobaArenaAuditReport const& report);
}
