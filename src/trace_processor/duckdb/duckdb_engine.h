/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_DUCKDB_ENGINE_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_DUCKDB_ENGINE_H_

#include <duckdb.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"

struct sqlite3;
struct sqlite3_stmt;

namespace perfetto::trace_processor {

class DuckDbEngine {
 public:
  struct QueryResult {
    QueryResult();
    ~QueryResult();

    QueryResult(QueryResult&&) noexcept;
    QueryResult& operator=(QueryResult&&) noexcept;

    QueryResult(const QueryResult&) = delete;
    QueryResult& operator=(const QueryResult&) = delete;

    bool Step();
    bool IsDone() const;
    uint32_t ColumnCount() const;
    uint32_t StatementCount() const;
    uint32_t StatementCountWithOutput() const;
    std::string GetColumnName(uint32_t col) const;
    SqlValue Get(uint32_t col) const;

    base::Status status = base::OkStatus();
    std::string sql;

   private:
    friend class DuckDbEngine;

    duckdb_result result{};
    uint64_t row_count = 0;
    uint64_t current_row = 0;
    bool has_current_row = false;
    bool is_done = true;
    mutable std::string string_scratch;
    mutable std::vector<uint8_t> bytes_scratch;
    uint32_t statement_count = 0;
    uint32_t statement_count_with_output = 0;
  };

  using StaticTable = PerfettoSqlEngine::StaticTable;

  DuckDbEngine();
  ~DuckDbEngine();

  DuckDbEngine(DuckDbEngine&&) noexcept = delete;
  DuckDbEngine& operator=(DuckDbEngine&&) = delete;

  base::Status ImportStaticTables(const std::vector<StaticTable>& tables,
                                  std::pair<int64_t, int64_t> trace_bounds);
  base::Status ImportSqliteObjects(sqlite3* db,
                                   std::pair<int64_t, int64_t> trace_bounds);
  base::Status ImportSqliteTables(sqlite3* db,
                                  const std::vector<std::string>& table_names,
                                  std::pair<int64_t, int64_t> trace_bounds);
  base::Status RegisterDataframes(const std::vector<StaticTable>& tables);
  base::Status InstallPrelude();
  base::StatusOr<QueryResult> Execute(
      const std::string& sql,
      const std::vector<SqlPackage>& sql_packages);
  void Interrupt();

 private:
  struct RegisteredDataframe {
    dataframe::Dataframe* dataframe = nullptr;
    dataframe::DataframeSpec spec;
  };

  base::Status CreateTableFromDataframe(const StaticTable& table);
  base::Status AppendRowsFromDataframe(const StaticTable& table);
  base::Status CreateTableFromSqliteStatement(const std::string& table_name,
                                              sqlite3_stmt* stmt);
  base::Status AppendRowsFromSqliteStatement(const std::string& table_name,
                                             sqlite3_stmt* stmt);
  base::Status RegisterDataframe(const std::string& name,
                                 dataframe::Dataframe* dataframe);
  const RegisteredDataframe* GetRegisteredDataframe(
      const std::string& name) const;
  base::Status RegisterDataframeTableFunction();
  base::Status ExecForSetup(const std::string& sql);
  base::StatusOr<QueryResult> ExecuteReturningStatement(
      const std::string& sql);

  static void DataframeReplacementScan(duckdb_replacement_scan_info info,
                                       const char* table_name,
                                       void* data);
  static void DataframeScanBind(duckdb_bind_info info);
  static void DataframeScanInit(duckdb_init_info info);
  static void DataframeScanFunction(duckdb_function_info info,
                                    duckdb_data_chunk output);

  duckdb_database db_ = nullptr;
  duckdb_connection conn_ = nullptr;
  std::unordered_map<std::string, RegisteredDataframe> dataframes_;
  std::unordered_set<std::string> included_modules_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_DUCKDB_ENGINE_H_
