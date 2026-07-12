// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/GroundEffectPreview.hpp>

#include <stdexcept>

int main()
{
  auto const first = Noggit::Rendering::groundEffectPreviewChoices(40, {8, 4, 2, 1}, 42);
  auto const second = Noggit::Rendering::groundEffectPreviewChoices(40, {8, 4, 2, 1}, 42);
  if (first.size() != 16 || first.size() != second.size())
    throw std::runtime_error("ground effect preview cap changed");
  for (std::size_t i = 0; i < first.size(); ++i)
    if (first[i].x != second[i].x || first[i].z != second[i].z
        || first[i].doodad != second[i].doodad
        || first[i].jitter_x != second[i].jitter_x
        || first[i].jitter_z != second[i].jitter_z
        || first[i].yaw_degrees != second[i].yaw_degrees
        || first[i].x > 7 || first[i].z > 7 || first[i].doodad > 3)
      throw std::runtime_error("ground effect preview is not deterministic");
}
