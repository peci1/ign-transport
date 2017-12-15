/*
 * Copyright (C) 2017 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <chrono>
#include <functional>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <ignition/common/Console.hh>

#include "ignition/transport/log/Log.hh"
#include "src/raii-sqlite3.hh"
#include "build_config.hh"


using namespace ignition::transport;
using namespace ignition::transport::log;


//////////////////////////////////////////////////
/// \brief allow pair of strings to be a key in a map
namespace std {
  template <> struct hash<std::pair<std::string, std::string>>
  {
    size_t operator()(const std::pair<std::string, std::string> &_topic) const
    {
      // Terrible, gets the job done
      return (std::hash<std::string>()(_topic.first) << 16)
        + std::hash<std::string>()(_topic.second);
    }
  };
}

/// \brief Private implementation
class ignition::transport::log::LogPrivate
{
  /// \brief End transaction
  public: bool EndTransaction();

  /// \brief Begin transaction
  public: bool BeginTransaction();

  /// \brief Get topic_id associated with a topic name and message type
  /// \param[in] _name the name of the topic
  /// \param[in] _type the name of the message type
  /// \return topic_id or -1 if one could not be produced
  public: int64_t TopicId(const std::string &_name, const std::string &_type);

  /// \brief Insert a message into the database
  public: bool InsertMessage(const common::Time _time, int64_t _topic,
      const void *_data, std::size_t _len);

  /// \brief Return true if enough time has passed since the last transaction
  public: bool TimeForNewTransaction();

  /// \brief SQLite3 database pointer wrapper
  public: std::unique_ptr<raii_sqlite3::Database> db;

  /// \brief True if a transaction is in progress
  public: bool inTransaction = false;

  /// \brief Map of topic name/type pairs
  public:
    std::unordered_map<std::pair<std::string, std::string>, int64_t> topics;

  /// \brief last time the transaction was ended
  public: std::chrono::steady_clock::time_point lastTransaction;

  /// \brief duration between transactions
  public: std::chrono::milliseconds transactionPeriod;
};

//////////////////////////////////////////////////
bool LogPrivate::EndTransaction()
{
  // End the transaction
  int returnCode = sqlite3_exec(
      this->db->Handle(), "END;", NULL, 0, nullptr);
  if (returnCode != SQLITE_OK)
  {
    ignerr << "Failed to end transaction" << returnCode << "\n";
    return false;
  }
  igndbg << "Ended transaction\n";
  this->inTransaction = false;
  return true;
}

//////////////////////////////////////////////////
bool LogPrivate::BeginTransaction()
{
  int returnCode = sqlite3_exec(
      this->db->Handle(), "BEGIN;", NULL, 0, nullptr);
  if (returnCode != SQLITE_OK)
  {
    ignerr << "Failed to begin transaction" << returnCode << "\n";
    return false;
  }
  this->inTransaction = true;
  igndbg << "Began transaction\n";
  this->lastTransaction = std::chrono::steady_clock::now();
  return true;
}

//////////////////////////////////////////////////
bool LogPrivate::TimeForNewTransaction()
{
  auto now = std::chrono::steady_clock::now();
  return now - this->transactionPeriod > this->lastTransaction;
}

//////////////////////////////////////////////////
int64_t LogPrivate::TopicId(const std::string &_name, const std::string &_type)
{
  int returnCode;
  // If the name and type is known, return a cached ID
  auto key = std::make_pair(_name, _type);
  auto topicIter = this->topics.find(key);
  if (topicIter != this->topics.end())
  {
    return topicIter->second;
  }

  // Otherwise insert it into the database and return the new topic_id
  const std::string sql_message_type =
    "INSERT OR IGNORE INTO message_types (name) VALUES (?001);";
  const std::string sql_topic =
    "INSERT INTO topics (name, message_type_id)"
    " SELECT ?002, id FROM message_types WHERE name = ?001 LIMIT 1;";

  raii_sqlite3::Statement message_type_statement(
      *(this->db), sql_message_type);
  if (!message_type_statement)
  {
    ignerr << "Failed to compile statement to insert message type\n";
    return -1;
  }
  raii_sqlite3::Statement topic_statement(
      *(this->db), sql_topic);
  if (!topic_statement)
  {
    ignerr << "Failed to compile statement to insert topic\n";
    return -1;
  }

  // Bind parameters
  returnCode = sqlite3_bind_text(
      message_type_statement.Handle(), 1, _type.c_str(), _type.size(), nullptr);
  if (returnCode != SQLITE_OK)
  {
    ignerr << "Failed to bind message type name(1): " << returnCode << "\n";
    return -1;
  }
  returnCode = sqlite3_bind_text(
      topic_statement.Handle(), 1, _type.c_str(), _type.size(), nullptr);
  if (returnCode != SQLITE_OK)
  {
    ignerr << "Failed to bind message type name(2): " << returnCode << "\n";
    return -1;
  }
  returnCode = sqlite3_bind_text(
      topic_statement.Handle(), 2, _name.c_str(), _name.size(), nullptr);
  if (returnCode != SQLITE_OK)
  {
    ignerr << "Failed to bind topic name: " << returnCode << "\n";
    return -1;
  }

  // Execute the statements
  returnCode = sqlite3_step(message_type_statement.Handle());
  if (returnCode != SQLITE_DONE)
  {
    ignerr << "Failed to insert message type: " << returnCode << "\n";
    return -1;
  }
  returnCode = sqlite3_step(topic_statement.Handle());
  if (returnCode != SQLITE_DONE)
  {
    ignerr << "Faild to insert topic: " << returnCode << "\n";
    return -1;
  }

  // topics.id is an alias for rowid
  int64_t id = sqlite3_last_insert_rowid(this->db->Handle());
  this->topics[key] = id;
  igndbg << "Inserted '" << _name << "'[" << _type << "]\n";
  return id;
}

//////////////////////////////////////////////////
bool LogPrivate::InsertMessage(const common::Time _time, int64_t _topic,
      const void *_data, std::size_t _len)
{
  int returnCode;
  const std::string sql_message =
    "INSERT INTO messages (time_recv_sec, time_recv_nano, message, topic_id)"
    "VALUES (?001, ?002, ?003, ?004);";

  // Compile the statement
  raii_sqlite3::Statement statement(*(this->db), sql_message);
  if (!statement)
  {
    ignerr << "Failed to compile insert message statement\n";
    return false;
  }

  // Bind parameters
  returnCode = sqlite3_bind_int(statement.Handle(), 1, _time.sec);
  if (returnCode != SQLITE_OK)
  {
    ignerr << "Failed to bind time received(s): " << returnCode << "\n";
    return false;
  }
  returnCode = sqlite3_bind_int(statement.Handle(), 2, _time.nsec);
  if (returnCode != SQLITE_OK)
  {
    ignerr << "Failed to bind time received(ns): " << returnCode << "\n";
    return false;
  }
  returnCode = sqlite3_bind_blob(statement.Handle(), 3, _data, _len, nullptr);
  if (returnCode != SQLITE_OK)
  {
    ignerr << "Failed to bind message data: " << returnCode << "\n";
    return false;
  }
  returnCode = sqlite3_bind_int(statement.Handle(), 4, _topic);
  if (returnCode != SQLITE_OK)
  {
    ignerr << "Failed to bind topic_id: " << returnCode << "\n";
    return false;
  }

  // Execute the statement
  returnCode = sqlite3_step(statement.Handle());
  if (returnCode != SQLITE_DONE)
  {
    ignerr << "Failed to insert message: " << returnCode << "\n";
    return false;
  }
  return true;
}

//////////////////////////////////////////////////
Log::Log()
  : dataPtr(new LogPrivate)
{
  // Default to 2 transactions per second
  this->dataPtr->transactionPeriod = std::chrono::milliseconds(500);
}

//////////////////////////////////////////////////
Log::Log(Log &&_other)  // NOLINT
  : dataPtr(std::move(_other.dataPtr))
{
}

//////////////////////////////////////////////////
Log::~Log()
{
  if (this->dataPtr->inTransaction)
  {
    this->dataPtr->EndTransaction();
  }
}

//////////////////////////////////////////////////
bool Log::Open(const std::string &_file, int64_t _mode)
{
  int returnCode;

  // Open the SQLite3 database
  if (this->dataPtr->db)
  {
    ignerr << "A database is already open\n";
    return false;
  }
  int64_t modeSQL;
  switch (_mode)
  {
    case READ:
      modeSQL = SQLITE_OPEN_READONLY;
      break;
    case READ_WRITE:
      modeSQL = SQLITE_OPEN_READWRITE;
      break;
    case READ_WRITE_CREATE:
      modeSQL = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
      break;
    default:
      ignerr << "Unknown mode passed to Log::Open()\n";
      return false;
  };
  this->dataPtr->db.reset(new raii_sqlite3::Database(_file, modeSQL));
  if (!*(this->dataPtr->db))
  {
    ignerr << "Failed to open sqlite3 database\n";
    return 1;
  }

  const char *schemaFile = SCHEMA_INSTALL_PATH "/0.1.0.sql";

  // Assume the file didn't exist before and create a blank schema
  igndbg << "Schema file: " << schemaFile << "\n";
  std::string schema;
  std::ifstream fin(schemaFile, std::ifstream::in);
  if (!fin)
  {
    ignerr << "Failed to open schema [" << schemaFile << "]\n";
    return false;
  }

  // get length of file:
  fin.seekg(0, fin.end);
  int length = fin.tellg();
  fin.seekg(0, fin.beg);

  // Try to read all of the schema at once
  char *buffer = new char[length];
  fin.read(buffer, length);
  schema = buffer;
  delete [] buffer;
  if (!fin)
  {
    ignerr << "Failed to read schema file [" << schemaFile << "[\n";
    return false;
  }

  // Apply the schema to the database
  returnCode = sqlite3_exec(
      this->dataPtr->db->Handle(), schema.c_str(), NULL, 0, NULL);
  if (returnCode != SQLITE_OK)
  {
    ignerr << "Failed to create log: " << sqlite3_errmsg(
        this->dataPtr->db->Handle()) << "\n";
    return false;
  }

  return true;
}

//////////////////////////////////////////////////
bool Log::InsertMessage(
    const common::Time &_time,
    const std::string &_topic, const std::string &_type,
    const void *_data, std::size_t _len)
{
  // Need to insert multiple messages pertransaction for best performance
  if (!this->dataPtr->inTransaction
      && !this->dataPtr->BeginTransaction())
  {
    return false;
  }

  // Get the topic_id for this name and message type
  int64_t topicId = this->dataPtr->TopicId(_topic, _type);
  if (topicId < 0)
  {
    return false;
  }

  // Insert the message into the database
  if (!this->dataPtr->InsertMessage(_time, topicId, _data, _len))
  {
    return false;
  }

  // Finish the transaction if enough time has passed
  if (this->dataPtr->TimeForNewTransaction())
  {
    this->dataPtr->EndTransaction();
  }

  return true;
}
