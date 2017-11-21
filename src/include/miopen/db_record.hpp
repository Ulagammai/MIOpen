/*******************************************************************************
*
* MIT License
*
* Copyright (c) 2017 Advanced Micro Devices, Inc.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
*******************************************************************************/
#ifndef GUARD_MIOPEN_DB_RECORD_HPP_
#define GUARD_MIOPEN_DB_RECORD_HPP_

#include <miopen/config.h>
#include <miopen/logger.hpp>

#include <boost/optional.hpp>
#include <sstream>
#include <string>
#include <atomic>
#include <unordered_map>
#include <iostream>

namespace miopen {

class Db;

/// db consists of 0 or more records.
/// Each record is an ASCII text line.
/// Record format:
///   [ KEY "=" ID ":" VALUES { ";" ID ":" VALUES} ]
///
/// KEY - An identifer of a record.
/// ID - Can be considered as a sub-key under which respective VALUES are stored.
/// VALUES - A data associated with specific ID under the KEY. Intended to represent a set of
/// values, hence the name.
///
/// Neither of ";:=" within KEY, ID and VALUES is allowed.
/// There should be none identical KEYs in the same db file.
/// There should be none identical IDs within the same record.
///
/// Intended usage:
/// KEY: A stringized problem config.
/// ID: A symbolic name of the Solver applicable for the KEY (problem config). There could be
/// several Solvers able to handle the same config, so several IDs can be put under a KEY.
/// Format of VALUES stored under each ID is Solver-specific. in other words, how a set of values
/// (or whatever a Solver wants to store in VALUES) is encoded into a string depends on the Solver.
/// Note: If VALUES is used to represent a set of numeric values, then it is recommended to use ","
/// as a separator.

/// Represents a db record associated with specific KEY.
/// Ctor arguments are path to db file and a KEY (or an object able to provide a KEY).
/// Upon construction, allows getting and modifying contents of a record (IDs and VALUES).
///
/// \todo Separate "db file" and "db record" abstractions.
/// \todo The Store() operation is neither MP- nor MT-safe.
class DbRecord
{
    private:
    std::string key;
    std::unordered_map<std::string, std::string> map;

    template <class T>
    static // 'static' is for calling from ctor
        std::string
        Serialize(const T& data)
    {
        std::ostringstream ss;
        data.Serialize(ss);
        return ss.str();
    }

    bool ParseContents(const std::string& contents);
    void WriteContents(std::ostream &stream) const;
    bool SetValues(const std::string& id, const std::string& values);
    bool GetValues(const std::string& id, std::string& values) const;
    bool Erase(const std::string& id);

    DbRecord(const std::string& key_) : key(key_) {}

    public:
    /// T shall provide a db KEY by means of the "void Serialize(std::ostream&) const" member
    /// function.
    template <class T>
    DbRecord(const T& problem_config_) : DbRecord(Serialize(problem_config_))
    {
    }

    /// Merges data from this record to data from that record if their keys are same.
    /// This record would contain all ID:VALUES pairs from that record that are not in this.
    /// E.g. this = {ID1:VALUE1}
    ///      that = {ID1:VALUE3, ID2:VALUE2}
    ///      this.Merge(that) = {ID1:VALUE1, ID2:VALUE2}
    void Merge(const DbRecord& that);

    /// Obtains VALUES from an object of class T and sets it in record (in association with ID,
    /// under the current KEY).
    /// T shall have the "void Serialize(std::ostream&) const" member function available.
    ///
    /// Returns true if records data was changed.
    template <class T>
    bool SetValues(const std::string& id, const T& values)
    {
        return SetValues(id, Serialize(values));
    }

    /// Get VALUES associated with ID under the current KEY and delivers those to a member function
    /// of a class T object. T shall have the "bool Deserialize(const std::string& str)"
    /// member function available.
    ///
    /// Returns false if there is none ID:VALUES in the record or in case of any error, e.g. if
    /// VALUES cannot be deserialized due to incorrect format.
    template <class T>
    bool GetValues(const std::string& id, T& values) const
    {
        std::string s;
        if(!GetValues(id, s))
            return false;

        const bool ok = values.Deserialize(s);
        if(!ok)
            MIOPEN_LOG(LoggingLevel::Error, "deserialize failed: " << s);
        return ok;
    }

    friend class Db;
};

class Db
{
    private:
    struct RecordPositions
    {
        std::streamoff begin = -1;
        std::streamoff end   = -1;
    };

    std::string filename;

    boost::optional<DbRecord> FindRecord(const std::string& key, RecordPositions* pos) const;

    bool Flush(const DbRecord& record, const RecordPositions* pos) const;

    public:
    Db(const std::string& filename_) : filename(filename_) {}

    /// Searches db for provided key and returns found reconrd or none if key not found in database
    boost::optional<DbRecord> FindRecord(const std::string& key) const
    {
        return FindRecord(key, nullptr);
    }

    template <class T>
    boost::optional<DbRecord> FindRecord(const T& problem_config) const
    {
        std::string key = DbRecord::Serialize(problem_config);
        return FindRecord(key, nullptr);
    }

    /// Stores provided record in database. If record with same key is already in database it is
    /// replaced by provided record.
    /// Returns true if store was successful, false otherwise.
    bool StoreRecord(const DbRecord& record) const;

    /// Stores provided record in database. If record with same key is already in database it is
    /// updated with values from provided record. Provided records data is also updated via
    /// DbRecord::Merge().
    /// Returns true if update was successful, false otherwise.
    bool UpdateRecord(DbRecord& record) const;

    /// Updates record under key PROBLEM_CONFIG  with data ID:VALUES in database.
    /// Both T and V classes should have "void Serialize(std::ostream&) const" member function
    /// available.
    /// Returns updated record or none if update was unsuccessful.
    template <class T, class V>
    boost::optional<DbRecord> Store(const T& problem_config, const std::string& id, const V& values)
    {
        DbRecord record(problem_config);
        record.SetValues(id, values);
        bool ok = UpdateRecord(record);
        if(ok)
            return record;
        else
            return boost::none;
    }

    /// Searches for record with key PROBLEM_CONFIG and gets VALUES under the ID from it.
    /// Class T should have "void Serialize(std::ostream&) const" member function available.
    /// Class V shall have "bool Deserialize(const std::string& str)" member function available.
    /// Returns false if there is none PROBLEM_CONFIG=ID:VALUES in the database
    /// or in case of any error, e.g. if VALUES cannot be deserialized due to incorrect format.
    template <class T, class V>
    bool Load(const T& problem_config, const std::string& id, V& values)
    {
        auto record = FindRecord(problem_config);
        if(!record)
            return false;
        return record->GetValues(id, values);
    }
};
} // namespace miopen

#endif // GUARD_MIOPEN_DB_RECORD_HPP_
