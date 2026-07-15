// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBCFile.h>
#include <noggit/Log.h>
#include <noggit/project/CurrentProject.hpp>

#include <ClientFile.hpp>

#include <filesystem>
#include <fstream>

namespace
{
  template<typename T>
  void write(std::ostream& stream, T const& value)
  {
    stream.write(reinterpret_cast<char const*>(&value), sizeof(T));
  }
}

void DBCFile::open(std::shared_ptr<BlizzardArchive::ClientData> clientData)
{
  BlizzardArchive::ClientFile file(filename, clientData.get());

  if (file.isEof())
  {
    LogError << "The DBC file \"" << filename
             << "\" could not be opened. This application may crash soon as the file is most likely needed."
             << std::endl;
    return;
  }
  LogDebug << "Opening DBC \"" << filename << "\"" << std::endl;

  char header[4];
  file.read(header, 4);
  assert(header[0] == 'W' && header[1] == 'D'
         && header[2] == 'B' && header[3] == 'C');
  file.read(&recordCount, 4);
  file.read(&fieldCount, 4);
  file.read(&recordSize, 4);
  file.read(&stringSize, 4);

  if (!fieldCount || !recordSize)
    throw std::logic_error(
      "DBC error, field count or record size is 0 : " + filename);
  if (fieldCount * 4 != recordSize)
    throw std::logic_error(
      "non four-byte-columns not supported : " + filename);

  data.resize(recordSize * recordCount);
  file.read(data.data(), data.size());
  stringTable.resize(stringSize);
  file.read(stringTable.data(), stringTable.size());
  file.close();
}

void DBCFile::save()
{
  auto const project_filename = std::filesystem::path(
    Noggit::Project::CurrentProject::get()->ProjectPath)
    / BlizzardArchive::ClientData::normalizeFilenameInternal(filename);
  std::error_code directory_error;
  std::filesystem::create_directories(
    project_filename.parent_path(), directory_error);
  if (directory_error)
    throw std::runtime_error(
      "Could not create DBC directory: " + directory_error.message());

  std::ofstream stream(
    project_filename,
    std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);
  if (!stream)
    throw std::runtime_error(
      "Could not open DBC for writing: " + project_filename.string());

  stream << 'W' << 'D' << 'B' << 'C';
  write(stream, recordCount);
  write(stream, fieldCount);
  write(stream, recordSize);
  write(stream, stringSize);
  stream.write(reinterpret_cast<char*>(data.data()), data.size());
  stream.write(stringTable.data(), stringSize);
  if (!stream)
    throw std::runtime_error(
      "Could not write DBC: " + project_filename.string());
}
