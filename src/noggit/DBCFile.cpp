// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBCFile.h>
#include <noggit/Log.h>

#include <cstring>
#include <string>

DBCFile::DBCFile(const std::string& _filename)
  : filename(_filename)
{
}

void DBCFile::overwriteWith(DBCFile const& file)
{
  filename = file.filename;
  recordSize = file.recordSize;
  recordCount = file.recordCount;
  fieldCount = file.fieldCount;
  stringSize = file.stringSize;
  data = file.data;
  stringTable = file.stringTable;
}

DBCFile DBCFile::createNew(std::string filename, std::uint32_t fieldCount, std::uint32_t recordSize)
{
  DBCFile file{};
  file.filename = std::move(filename);
  file.recordSize = recordSize;
  file.fieldCount = fieldCount;
  return file;
}

DBCFile::Record DBCFile::getRecord(size_t id)
{
  return Record(*this, data.data() + id * recordSize);
}

DBCFile::Iterator DBCFile::begin()
{
  return Iterator(*this, data.data());
}

DBCFile::Iterator DBCFile::end()
{
  return Iterator(*this, data.data() + data.size());
}

size_t DBCFile::getRecordCount() const
{
  return recordCount;
}

size_t DBCFile::getFieldCount() const
{
  return fieldCount;
}

size_t DBCFile::getRecordSize() const
{
  return recordSize;
}

DBCFile::Record DBCFile::getByID(unsigned int id, size_t field)
{
  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(field) == id)
      return (*i);
  }
  LogDebug << "Tried to get a not existing row in " << filename << " (ID = " << id << ")!" << std::endl;
  throw NotFound();
}

bool DBCFile::CheckIfIdExists(unsigned int id, size_t field)
{
  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(field) == id)
      return (true);
  }
  return (false);
}

int DBCFile::getRecordRowId(unsigned int id, size_t field)
{
  int row_id = 0;
  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(field) == id)
      return row_id;

    row_id++;
  }
  LogError << "Tried to get a not existing row in " << filename << " (ID = " << id << ")!" << std::endl;
  throw NotFound();
}

DBCFile::Record DBCFile::addRecord(size_t id, size_t id_field)
{
  assert(recordSize > 0);
  assert(id_field < fieldCount);

  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(id_field) == id)
      throw AlreadyExists();
  }

  size_t old_size = data.size();
  data.resize(old_size + recordSize);
  *reinterpret_cast<unsigned int*>(data.data() + old_size + id_field * sizeof(std::uint32_t)) = static_cast<unsigned int>(id);

  recordCount++;

  return Record(*this, data.data() + old_size);
}

DBCFile::Record DBCFile::addRecordCopy(size_t id, size_t id_from, size_t id_field)
{
  recordCount++;

  bool from_found = false;
  size_t from_idx = 0;

  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(id_field) == id)
      throw AlreadyExists();

    if (i->getUInt(id_field) == id_from)
    {
      from_found = true;
    }

    if (!from_found)
    {
      from_idx++;
    }
  }

  if (!from_found)
  {
    throw NotFound();
  }

  size_t old_size = data.size();
  data.resize(old_size + recordSize);

  Record record_from = getRecord(from_idx);
  std::copy(data.data() + from_idx * recordSize, data.data() + from_idx * recordSize + recordSize, data.data() + old_size);
  *reinterpret_cast<unsigned int*>(data.data() + old_size + id_field * sizeof(std::uint32_t)) = static_cast<unsigned int>(id);

  return Record(*this, data.data() + old_size);
}

void DBCFile::removeRecord(size_t id, size_t id_field)
{
  if (recordCount == 0)
  {
    throw NotFound();
  }

  size_t row_counter = 0;

  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(id_field) == id)
    {
      size_t row_position = row_counter * recordSize; // position of the record to remove
      data.erase(data.begin() + row_position, data.begin() + row_position + recordSize);

      recordCount--;
      return;
    }

    row_counter++;

  }

  throw NotFound();

}

int DBCFile::getEmptyRecordID(size_t id_field)
{

  unsigned int id = 0;

  for (Iterator i = begin(); i != end(); ++i)
  {
    id = std::max(i->getUInt(id_field), id);
  }

  return static_cast<int>(++id);
}

const float& DBCFile::Record::getFloat(size_t field) const
{
    assert(field < file.fieldCount);
    return *reinterpret_cast<float*>(offset + field * 4);
}

const unsigned int& DBCFile::Record::getUInt(size_t field) const
{
  assert(field < file.fieldCount);
  return *reinterpret_cast<unsigned int*>(offset + field * 4);
}

const int& DBCFile::Record::getInt(size_t field) const
{
  assert(field < file.fieldCount);
  return *reinterpret_cast<int*>(offset + field * 4);
}

const char* DBCFile::Record::getString(size_t field) const
{
  assert(field < file.fieldCount);
  size_t stringOffset = getUInt(field);
  assert(stringOffset < file.stringSize);
  return file.stringTable.data() + stringOffset;
}

const char* DBCFile::Record::getLocalizedString(size_t field, int locale) const
{
  int loc = locale;
  if (locale == -1)
  {
    assert(field < file.fieldCount - 8);
    for (loc = 0; loc < 15; loc++)
    {
      size_t stringOffset = getUInt(field + loc);
      if (stringOffset != 0)
        break;
    }
  }

  assert(field + loc < file.fieldCount);
  size_t stringOffset = getUInt(field + loc);
  assert(stringOffset < file.stringSize);
  return file.stringTable.data() + stringOffset;
}

void DBCFile::Record::writeString(size_t field, const std::string& val)
{
  assert(field < file.fieldCount);

  if (!val.size())
  {
    *reinterpret_cast<unsigned int*>(offset + field * 4) = 0;
    return;
  }

  size_t old_size = file.stringTable.size();
  *reinterpret_cast<unsigned int*>(offset + field * 4) = static_cast<unsigned int>(file.stringTable.size());
  file.stringTable.resize(old_size + val.size() + 1);
  std::copy(val.c_str(), val.c_str() + val.size() + 1, file.stringTable.data() + old_size);
  file.stringSize += static_cast<std::uint32_t>(val.size() + 1);
}

void DBCFile::Record::writeLocalizedString(size_t field, const std::string& val, unsigned int locale)
{
  assert(field < file.fieldCount);
  assert(locale < 16);

  if (!val.size())
  {
    *reinterpret_cast<unsigned int*>(offset + ((field + locale) * 4)) = 0;
    return;
  }

  size_t old_size = file.stringTable.size();
  *reinterpret_cast<unsigned int*>(offset + ((field + locale) * 4)) = static_cast<unsigned int>(file.stringTable.size());
  file.stringTable.resize(old_size + val.size() + 1);
  std::copy(val.c_str(), val.c_str() + val.size() + 1, file.stringTable.data() + old_size);
  file.stringSize += static_cast<std::uint32_t>(val.size() + 1);
}
