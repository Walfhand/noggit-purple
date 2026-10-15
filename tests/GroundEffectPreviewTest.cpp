// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/GroundEffectPreview.hpp>

#include <array>
#include <stdexcept>

int main()
{
  auto const first = Noggit::Rendering::groundEffectPreviewChoices(40, {8, 4, 2, 1}, 42);
  auto const second = Noggit::Rendering::groundEffectPreviewChoices(40, {8, 4, 2, 1}, 42);
  if (first.size() != 1536 || first.size() != second.size())
    throw std::runtime_error("ground effect preview cap changed");
  auto const lush = Noggit::Rendering::groundEffectPreviewChoices(12, {6, 39, 43, 12}, 1);
  if (lush.size() != 768)
    throw std::runtime_error("lush grass preview density changed");
  std::array<bool, 64> occupied_cells{};
  std::size_t occupied_cell_count = 0;
  for (std::size_t splat = 0; splat < 64; ++splat)
  {
    auto const& first_choice = lush[splat * 12];
    auto const cell = first_choice.z * 8u + first_choice.x;
    if (!occupied_cells[cell])
    {
      occupied_cells[cell] = true;
      ++occupied_cell_count;
    }
    for (std::size_t i = 1; i < 12; ++i)
    {
      auto const& choice = lush[splat * 12 + i];
      if (choice.x != first_choice.x || choice.z != first_choice.z)
        throw std::runtime_error("lush grass preview splat is not dense");
    }
  }
  if (occupied_cell_count == 64)
    throw std::runtime_error("lush grass preview no longer has organic gaps");
  for (std::size_t i = 0; i < first.size(); ++i)
    if (first[i].x != second[i].x || first[i].z != second[i].z
        || first[i].doodad != second[i].doodad
        || first[i].jitter_x != second[i].jitter_x
        || first[i].jitter_z != second[i].jitter_z
        || first[i].yaw_degrees != second[i].yaw_degrees
        || first[i].scale != second[i].scale
        || first[i].scale < 0.67f || first[i].scale > 1.33f
        || first[i].x > 7 || first[i].z > 7 || first[i].doodad > 3)
      throw std::runtime_error("ground effect preview is not deterministic");

  auto const meadow = Noggit::Rendering::groundEffectAssetScore("8rivgrass02.m2");
  auto const jungle = Noggit::Rendering::groundEffectAssetScore("8zulgrass02.m2");
  auto const kultiras = Noggit::Rendering::groundEffectAssetScore("8kulgrass04.m2");
  auto const flowers = Noggit::Rendering::groundEffectAssetScore("8drkflowers01.m2");
  auto const dry = Noggit::Rendering::groundEffectAssetScore("8bardrygrass01.m2");
  auto const lush_legacy = Noggit::Rendering::groundEffectAssetScore("ElwGra03.mdl");
  auto const yellow_legacy = Noggit::Rendering::groundEffectAssetScore("WesGra03.mdl");
  if (!(kultiras > jungle && jungle > meadow && meadow > flowers && jungle > dry
        && lush_legacy > yellow_legacy))
    throw std::runtime_error("recent lush grass ranking changed");
}
