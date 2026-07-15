// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/ai/ProceduralSkybox.hpp>

#include <array>
#include <cmath>
#include <stdexcept>

static_assert(LightParamsDB::glow == 3);
static_assert(LightParamsDB::water_shallow_alpha == 4);
static_assert(LightParamsDB::water_deep_alpha == 5);
static_assert(LightParamsDB::ocean_shallow_alpha == 6);
static_assert(LightParamsDB::ocean_deep_alpha == 7);
static_assert(LightParamsDB::flags == 8);

namespace
{
  void require(bool condition, char const* message)
  {
    if (!condition) throw std::runtime_error(message);
  }
}

int main()
{
  auto light = DBCFile::createNew("Light.dbc", 15, 15 * 4);
  auto params = DBCFile::createNew("LightParams.dbc", 9, 9 * 4);
  auto skyboxes = DBCFile::createNew("LightSkybox.dbc", 3, 3 * 4);
  auto int_bands = DBCFile::createNew("LightIntBand.dbc", 34, 34 * 4);
  auto float_bands = DBCFile::createNew("LightFloatBand.dbc", 34, 34 * 4);

  auto source_light = light.addRecord(1);
  source_light.write(LightDB::Map, std::uint32_t{7});
  source_light.write(LightDB::PositionX, 10.0f);
  source_light.write(LightDB::PositionY, 20.0f);
  source_light.write(LightDB::PositionZ, 30.0f);
  source_light.write(LightDB::RadiusInner, 40.0f);
  source_light.write(LightDB::RadiusOuter, 50.0f);
  source_light.write(LightDB::DataIDs, std::uint32_t{2});
  source_light.write(LightDB::DataIDs + 1, std::uint32_t{77});

  auto source_params = params.addRecord(2);
  source_params.write(LightParamsDB::highlightSky, std::uint32_t{1});
  source_params.write(LightParamsDB::skybox, std::uint32_t{4});
  source_params.write(LightParamsDB::glow, 0.75f);
  source_params.write(LightParamsDB::water_shallow_alpha, 0.1f);
  source_params.write(LightParamsDB::water_deep_alpha, 0.2f);
  source_params.write(LightParamsDB::ocean_shallow_alpha, 0.3f);
  source_params.write(LightParamsDB::ocean_deep_alpha, 0.4f);
  source_params.write(LightParamsDB::flags, std::uint32_t{9});

  auto old_skybox = skyboxes.addRecord(4);
  old_skybox.writeString(LightSkyboxDB::filename, "environments/stars/old.m2");
  old_skybox.write(LightSkyboxDB::flags, std::uint32_t{0});

  for (std::uint32_t offset = 0; offset < 18; ++offset)
  {
    auto band = int_bands.addRecord(19 + offset);
    band.write(LightIntBandDB::Entries, std::uint32_t{1});
    band.write(LightIntBandDB::Times, 100u + offset);
    band.write(LightIntBandDB::Values, 200u + offset);
  }
  for (std::uint32_t offset = 0; offset < 6; ++offset)
  {
    auto band = float_bands.addRecord(7 + offset);
    band.write(LightFloatBandDB::Entries, std::uint32_t{1});
    band.write(LightFloatBandDB::Times, 300u + offset);
    band.write(LightFloatBandDB::Values, 0.5f + static_cast<float>(offset));
  }

  auto const first = Noggit::Ai::attachGlobalSkybox(
    light, params, skyboxes, int_bands, float_bands, 42,
    "environments/stars/new.m2", 1);
  require(first.changed && first.light_id == 2 && first.light_params_id == 3
            && first.skybox_id == 5,
          "skybox attachment returned unexpected IDs");
  auto const target_light = light.getByID(first.light_id);
  require(target_light.getUInt(LightDB::Map) == 42
            && target_light.getFloat(LightDB::PositionX) == 0.0f
            && target_light.getFloat(LightDB::PositionY) == 0.0f
            && target_light.getFloat(LightDB::PositionZ) == 0.0f
            && target_light.getUInt(LightDB::DataIDs) == 3
            && target_light.getUInt(LightDB::DataIDs + 1) == 77,
          "global light was not cloned and attached correctly");
  require(light.getByID(1).getUInt(LightDB::DataIDs) == 2
            && params.getByID(2).getUInt(LightParamsDB::skybox) == 4
            && params.getByID(2).getFloat(LightParamsDB::glow) == 0.75f,
          "shared source light parameters were modified");
  auto const target_params = params.getByID(3);
  require(target_params.getUInt(LightParamsDB::skybox) == 5
            && target_params.getFloat(LightParamsDB::glow) == 0.75f
            && target_params.getUInt(LightParamsDB::flags) == 9,
          "cloned light parameters lost source values");
  require(std::string{skyboxes.getByID(5).getString(LightSkyboxDB::filename)}
              == "environments/stars/new.m2"
            && skyboxes.getByID(5).getUInt(LightSkyboxDB::flags) == 1,
          "LightSkybox record was not created");
  for (std::uint32_t offset = 0; offset < 18; ++offset)
    require(int_bands.getByID(37 + offset).getUInt(LightIntBandDB::Values)
              == 200u + offset,
            "integer light bands were not cloned");
  for (std::uint32_t offset = 0; offset < 6; ++offset)
    require(std::abs(float_bands.getByID(13 + offset).getFloat(
                       LightFloatBandDB::Values)
                     - (0.5f + static_cast<float>(offset))) < 0.0001f,
            "float light bands were not cloned");

  auto const counts = std::array<std::size_t, 5>{
    light.getRecordCount(), params.getRecordCount(), skyboxes.getRecordCount(),
    int_bands.getRecordCount(), float_bands.getRecordCount()};
  auto const second = Noggit::Ai::attachGlobalSkybox(
    light, params, skyboxes, int_bands, float_bands, 42,
    "environments/stars/new.m2", 1);
  require(!second.changed && second.light_id == first.light_id
            && second.light_params_id == first.light_params_id
            && second.skybox_id == first.skybox_id,
          "second skybox attachment was not idempotent");
  require(counts == std::array<std::size_t, 5>{
            light.getRecordCount(), params.getRecordCount(), skyboxes.getRecordCount(),
            int_bands.getRecordCount(), float_bands.getRecordCount()},
          "idempotent attachment added DBC records");
}
