// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/ProceduralSkybox.hpp>

#include <noggit/DBC.h>

#include <optional>
#include <stdexcept>

namespace Noggit::Ai
{
  namespace
  {
    constexpr std::size_t clear_param = 0;
    constexpr std::uint32_t int_band_count = 18;
    constexpr std::uint32_t float_band_count = 6;

    void requireFields(DBCFile const& dbc, std::size_t minimum, char const* name)
    {
      if (dbc.getFieldCount() < minimum || dbc.getRecordSize() < minimum * 4)
        throw std::invalid_argument(std::string{name} + " has an incompatible schema");
    }

    std::optional<std::uint32_t> globalLightId(DBCFile& light, std::uint32_t map_id)
    {
      for (auto record = light.begin(); record != light.end(); ++record)
        if (record->getUInt(LightDB::Map) == map_id
            && record->getFloat(LightDB::PositionX) == 0.0f
            && record->getFloat(LightDB::PositionY) == 0.0f
            && record->getFloat(LightDB::PositionZ) == 0.0f)
          return record->getUInt(LightDB::ID);
      return std::nullopt;
    }

    std::size_t parameterReferenceCount(DBCFile& light, std::uint32_t param_id)
    {
      auto count = std::size_t{0};
      for (auto record = light.begin(); record != light.end(); ++record)
        for (std::size_t offset = 0; offset < 8; ++offset)
          if (record->getUInt(LightDB::DataIDs + offset) == param_id) ++count;
      return count;
    }

    std::optional<std::uint32_t> matchingSkyboxId(
      DBCFile& skyboxes, std::string const& path, std::uint32_t flags)
    {
      for (auto record = skyboxes.begin(); record != skyboxes.end(); ++record)
        if (record->getString(LightSkyboxDB::filename) == path
            && record->getUInt(LightSkyboxDB::flags) == flags)
          return record->getUInt(LightSkyboxDB::ID);
      return std::nullopt;
    }

    std::uint32_t bandStart(std::uint32_t param_id, std::uint32_t count)
    {
      return (param_id - 1) * count + 1;
    }

    void requireBandSources(
      DBCFile& bands, std::uint32_t param_id, std::uint32_t count,
      char const* name)
    {
      auto const first = bandStart(param_id, count);
      for (std::uint32_t offset = 0; offset < count; ++offset)
        if (!bands.CheckIfIdExists(first + offset))
          throw std::runtime_error(std::string{name} + " source band is missing");
    }

    bool bandRangeExists(
      DBCFile& bands, std::uint32_t param_id, std::uint32_t count)
    {
      auto const first = bandStart(param_id, count);
      for (std::uint32_t offset = 0; offset < count; ++offset)
        if (!bands.CheckIfIdExists(first + offset)) return false;
      return true;
    }

    bool bandRangeIsFree(DBCFile& bands, std::uint32_t param_id, std::uint32_t count)
    {
      auto const first = bandStart(param_id, count);
      for (std::uint32_t offset = 0; offset < count; ++offset)
        if (bands.CheckIfIdExists(first + offset)) return false;
      return true;
    }

    bool parameterPresetMatches(
      DBCFile& params, std::uint32_t target_id, std::uint32_t source_id)
    {
      auto const target = params.getByID(target_id);
      auto const source = params.getByID(source_id);
      for (std::size_t field = 1; field < params.getFieldCount(); ++field)
        if (field != LightParamsDB::skybox
            && target.getUInt(field) != source.getUInt(field))
          return false;
      return true;
    }

    bool bandRangeMatches(
      DBCFile& bands, std::uint32_t target_param_id,
      std::uint32_t source_param_id, std::uint32_t count)
    {
      auto const target_start = bandStart(target_param_id, count);
      auto const source_start = bandStart(source_param_id, count);
      for (std::uint32_t offset = 0; offset < count; ++offset)
      {
        auto const target = bands.getByID(target_start + offset);
        auto const source = bands.getByID(source_start + offset);
        for (std::size_t field = 1; field < bands.getFieldCount(); ++field)
          if (target.getUInt(field) != source.getUInt(field)) return false;
      }
      return true;
    }

    void copyParameterPreset(
      DBCFile& params, std::uint32_t target_id, std::uint32_t source_id)
    {
      auto target = params.getByID(target_id);
      auto const source = params.getByID(source_id);
      for (std::size_t field = 1; field < params.getFieldCount(); ++field)
        if (field != LightParamsDB::skybox)
          target.write(field, source.getUInt(field));
    }

    void copyBandRange(
      DBCFile& bands, std::uint32_t target_param_id,
      std::uint32_t source_param_id, std::uint32_t count)
    {
      auto const target_start = bandStart(target_param_id, count);
      auto const source_start = bandStart(source_param_id, count);
      for (std::uint32_t offset = 0; offset < count; ++offset)
      {
        auto target = bands.getByID(target_start + offset);
        auto const source = bands.getByID(source_start + offset);
        for (std::size_t field = 1; field < bands.getFieldCount(); ++field)
          target.write(field, source.getUInt(field));
      }
    }
  }

  ProceduralSkyboxResult attachGlobalSkybox(
    DBCFile& light, DBCFile& light_params, DBCFile& light_skybox,
    DBCFile& light_int_band, DBCFile& light_float_band,
    std::uint32_t map_id, std::string const& skybox_path,
    std::uint32_t flags, std::size_t lighting_param_index)
  {
    if (skybox_path.empty())
      throw std::invalid_argument("skybox path must not be empty");
    if (lighting_param_index >= 8)
      throw std::invalid_argument("lighting parameter index must be between 0 and 7");
    requireFields(light, LightDB::DataIDs + 8, "Light.dbc");
    requireFields(light_params, LightParamsDB::flags + 1, "LightParams.dbc");
    requireFields(light_skybox, LightSkyboxDB::flags + 1, "LightSkybox.dbc");
    requireFields(light_int_band, LightIntBandDB::Values + 16, "LightIntBand.dbc");
    requireFields(light_float_band, LightFloatBandDB::Values + 16, "LightFloatBand.dbc");

    auto const existing_light_id = globalLightId(light, map_id);
    auto const source_light_id = existing_light_id.value_or(1);
    if (!light.CheckIfIdExists(source_light_id))
      throw std::runtime_error("Light.dbc source light is missing");
    auto const target_param_id = light.getByID(source_light_id).getUInt(
      LightDB::DataIDs + clear_param);
    auto const source_param_id = light.getByID(source_light_id).getUInt(
      LightDB::DataIDs + lighting_param_index);
    if (target_param_id == 0 || !light_params.CheckIfIdExists(target_param_id))
      throw std::runtime_error("LightParams.dbc clear source is missing");
    if (source_param_id == 0 || !light_params.CheckIfIdExists(source_param_id))
      throw std::runtime_error("LightParams.dbc lighting preset is missing");
    requireBandSources(light_int_band, source_param_id, int_band_count,
                       "LightIntBand.dbc");
    requireBandSources(light_float_band, source_param_id, float_band_count,
                       "LightFloatBand.dbc");

    auto const update_private_param = existing_light_id
      && parameterReferenceCount(light, target_param_id) == 1
      && bandRangeExists(light_int_band, target_param_id, int_band_count)
      && bandRangeExists(light_float_band, target_param_id, float_band_count);
    if (update_private_param)
    {
      auto const current_skybox_id = light_params.getByID(target_param_id).getUInt(
        LightParamsDB::skybox);
      if (current_skybox_id != 0 && light_skybox.CheckIfIdExists(current_skybox_id))
      {
        auto const current = light_skybox.getByID(current_skybox_id);
        if (current.getString(LightSkyboxDB::filename) == skybox_path
            && current.getUInt(LightSkyboxDB::flags) == flags)
          if (parameterPresetMatches(light_params, target_param_id, source_param_id)
              && bandRangeMatches(light_int_band, target_param_id, source_param_id,
                                  int_band_count)
              && bandRangeMatches(light_float_band, target_param_id, source_param_id,
                                  float_band_count))
            return {false, *existing_light_id, target_param_id, current_skybox_id};
      }
    }

    auto new_param_id = target_param_id;
    if (!update_private_param)
    {
      new_param_id = static_cast<std::uint32_t>(light_params.getEmptyRecordID());
      while (!bandRangeIsFree(light_int_band, new_param_id, int_band_count)
             || !bandRangeIsFree(light_float_band, new_param_id, float_band_count))
        ++new_param_id;
    }
    auto const reused_skybox_id = matchingSkyboxId(light_skybox, skybox_path, flags);
    auto const new_skybox_id = reused_skybox_id.value_or(
      static_cast<std::uint32_t>(light_skybox.getEmptyRecordID()));
    auto const new_light_id = existing_light_id.value_or(
      static_cast<std::uint32_t>(light.getEmptyRecordID()));

    auto const light_backup = light;
    auto const params_backup = light_params;
    auto const skybox_backup = light_skybox;
    auto const int_band_backup = light_int_band;
    auto const float_band_backup = light_float_band;
    try
    {
      if (!reused_skybox_id)
      {
        auto skybox = light_skybox.addRecord(new_skybox_id);
        skybox.writeString(LightSkyboxDB::filename, skybox_path);
        skybox.write(LightSkyboxDB::flags, flags);
      }

      if (update_private_param)
      {
        copyParameterPreset(light_params, target_param_id, source_param_id);
        copyBandRange(light_int_band, target_param_id, source_param_id,
                      int_band_count);
        copyBandRange(light_float_band, target_param_id, source_param_id,
                      float_band_count);
        light_params.getByID(target_param_id).write(
          LightParamsDB::skybox, new_skybox_id);
        return {true, *existing_light_id, target_param_id, new_skybox_id};
      }

      auto params = light_params.addRecordCopy(new_param_id, source_param_id);
      params.write(LightParamsDB::skybox, new_skybox_id);

      auto const source_int_start = bandStart(source_param_id, int_band_count);
      auto const target_int_start = bandStart(new_param_id, int_band_count);
      for (std::uint32_t offset = 0; offset < int_band_count; ++offset)
        light_int_band.addRecordCopy(target_int_start + offset,
                                     source_int_start + offset);
      auto const source_float_start = bandStart(source_param_id, float_band_count);
      auto const target_float_start = bandStart(new_param_id, float_band_count);
      for (std::uint32_t offset = 0; offset < float_band_count; ++offset)
        light_float_band.addRecordCopy(target_float_start + offset,
                                       source_float_start + offset);

      if (!existing_light_id)
      {
        auto target_light = light.addRecordCopy(new_light_id, source_light_id);
        target_light.write(LightDB::Map, map_id);
        target_light.write(LightDB::PositionX, 0.0f);
        target_light.write(LightDB::PositionY, 0.0f);
        target_light.write(LightDB::PositionZ, 0.0f);
      }
      light.getByID(new_light_id).write(
        LightDB::DataIDs + clear_param, new_param_id);
      return {true, new_light_id, new_param_id, new_skybox_id};
    }
    catch (...)
    {
      light.overwriteWith(light_backup);
      light_params.overwriteWith(params_backup);
      light_skybox.overwriteWith(skybox_backup);
      light_int_band.overwriteWith(int_band_backup);
      light_float_band.overwriteWith(float_band_backup);
      throw;
    }
  }
}
