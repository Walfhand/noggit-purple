// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class DBCFile;

namespace Noggit::Ai
{
  struct ProceduralSkyboxResult
  {
    bool changed = false;
    std::uint32_t light_id = 0;
    std::uint32_t light_params_id = 0;
    std::uint32_t skybox_id = 0;
  };

  ProceduralSkyboxResult attachGlobalSkybox(
    DBCFile& light, DBCFile& light_params, DBCFile& light_skybox,
    DBCFile& light_int_band, DBCFile& light_float_band,
    std::uint32_t map_id, std::string const& skybox_path,
    std::uint32_t flags, std::size_t lighting_param_index = 0);
}
