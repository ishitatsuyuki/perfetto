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

#include "src/trace_processor/duckdb/duckdb_engine.h"

#include <duckdb.h>
#include <sqlite3.h>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/core/dataframe/cursor_impl.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_parser.h"
#include "src/trace_processor/perfetto_sql/preprocessor/perfetto_sql_preprocessor.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/sql_modules.h"

namespace perfetto::trace_processor {
namespace {

std::string QuoteIdent(std::string_view ident) {
  std::string res = "\"";
  for (char c : ident) {
    if (c == '"') {
      res += "\"\"";
    } else {
      res += c;
    }
  }
  res += "\"";
  return res;
}

std::string ApplyDuckDbSqlRewrites(std::string sql) {
  struct Rewrite {
    const char* from;
    const char* to;
  };
  static constexpr Rewrite kRewrites[] = {
      // DuckDB has GLOB, but SQLite's `R*` pattern means DuckDB's `R%` LIKE.
      {" GLOB 'R*'", " LIKE 'R%'"},
      {" glob 'R*'", " LIKE 'R%'"},
  };
  for (const Rewrite& rewrite : kRewrites) {
    sql = base::ReplaceAll(sql, rewrite.from, rewrite.to);
  }
  return sql;
}

std::string CreatePerfettoTableSql(
    const PerfettoSqlParser::CreateTable& create_table) {
  std::string sql = "CREATE ";
  if (create_table.replace) {
    sql += "OR REPLACE ";
  }
  sql += "TABLE ";
  sql += QuoteIdent(create_table.name);
  sql += " AS ";
  sql += create_table.sql.sql();
  return sql;
}

std::string CreatePerfettoViewSql(
    const PerfettoSqlParser::CreateView& create_view) {
  if (!create_view.replace) {
    return create_view.create_view_sql.sql();
  }
  std::string sql = create_view.create_view_sql.sql();
  static constexpr const char* kCreateView = "CREATE VIEW ";
  static constexpr const char* kCreateViewLower = "create view ";
  size_t create_view_len = strlen(kCreateView);
  if (sql.rfind(kCreateView, 0) == 0) {
    return "CREATE OR REPLACE VIEW " + sql.substr(create_view_len);
  }
  if (sql.rfind(kCreateViewLower, 0) == 0) {
    return "CREATE OR REPLACE VIEW " + sql.substr(create_view_len);
  }
  return sql;
}

const std::string* FindSqlModule(const std::vector<SqlPackage>& sql_packages,
                                 const std::string& include_key) {
  for (const SqlPackage& package : sql_packages) {
    if (!sql_modules::IsPackagePrefixOf(package.name, include_key)) {
      continue;
    }
    for (const auto& module : package.modules) {
      if (module.first == include_key) {
        return &module.second;
      }
    }
    return nullptr;
  }
  return nullptr;
}

const char* DuckDbTypeForColumn(
    const core::dataframe::ColumnSpec& column_spec) {
  using StorageType = core::dataframe::StorageType;
  using Id = core::dataframe::Id;
  using Uint32 = core::dataframe::Uint32;
  using Int32 = core::dataframe::Int32;
  using Int64 = core::dataframe::Int64;
  using Double = core::dataframe::Double;
  using String = core::dataframe::String;

  const auto& type = column_spec.type;
  switch (type.index()) {
    case StorageType::GetTypeIndex<Id>():
    case StorageType::GetTypeIndex<Uint32>():
    case StorageType::GetTypeIndex<Int32>():
    case StorageType::GetTypeIndex<Int64>():
      return "BIGINT";
    case StorageType::GetTypeIndex<Double>():
      return "DOUBLE";
    case StorageType::GetTypeIndex<String>():
      return "VARCHAR";
    default:
      PERFETTO_FATAL("Unsupported dataframe storage type for DuckDB");
  }
}

duckdb_type DuckDbLogicalTypeForColumn(
    const core::dataframe::ColumnSpec& column_spec) {
  using StorageType = core::dataframe::StorageType;
  using Id = core::dataframe::Id;
  using Uint32 = core::dataframe::Uint32;
  using Int32 = core::dataframe::Int32;
  using Int64 = core::dataframe::Int64;
  using Double = core::dataframe::Double;
  using String = core::dataframe::String;

  const auto& type = column_spec.type;
  switch (type.index()) {
    case StorageType::GetTypeIndex<Id>():
    case StorageType::GetTypeIndex<Uint32>():
    case StorageType::GetTypeIndex<Int32>():
    case StorageType::GetTypeIndex<Int64>():
      return DUCKDB_TYPE_BIGINT;
    case StorageType::GetTypeIndex<Double>():
      return DUCKDB_TYPE_DOUBLE;
    case StorageType::GetTypeIndex<String>():
      return DUCKDB_TYPE_VARCHAR;
    default:
      PERFETTO_FATAL("Unsupported dataframe storage type for DuckDB");
  }
}

const char* DuckDbTypeForSqliteColumn(const char* decl_type) {
  if (!decl_type) {
    return "VARCHAR";
  }
  std::string type = base::ToUpper(std::string(decl_type));
  if (type.find("INT") != std::string::npos ||
      type.find("BOOL") != std::string::npos) {
    return "BIGINT";
  }
  if (type.find("DOUBLE") != std::string::npos ||
      type.find("FLOAT") != std::string::npos ||
      type.find("REAL") != std::string::npos) {
    return "DOUBLE";
  }
  if (type.find("BLOB") != std::string::npos ||
      type.find("BYTES") != std::string::npos) {
    return "BLOB";
  }
  return "VARCHAR";
}

base::Status DuckDbAppendStateToStatus(duckdb_state state,
                                       duckdb_appender appender,
                                       const char* table_name,
                                       const char* prefix) {
  if (state == DuckDBSuccess) {
    return base::OkStatus();
  }
  const char* err = duckdb_appender_error(appender);
  return base::ErrStatus("%s for table %s: %s", prefix, table_name,
                         err ? err : "unknown DuckDB error");
}

base::Status DuckDbStateToStatus(duckdb_state state,
                                 duckdb_result* result,
                                 const char* prefix) {
  if (state == DuckDBSuccess) {
    return base::OkStatus();
  }
  const char* err = result ? duckdb_result_error(result) : nullptr;
  return base::ErrStatus("%s: %s", prefix, err ? err : "unknown DuckDB error");
}

struct AppenderCellCallback : core::dataframe::CellCallback {
  explicit AppenderCellCallback(duckdb_appender _appender)
      : appender(_appender) {}

  void OnCell(int64_t v) { Set(duckdb_append_int64(appender, v)); }
  void OnCell(double v) { Set(duckdb_append_double(appender, v)); }
  void OnCell(NullTermStringView v) {
    Set(duckdb_append_varchar_length(appender, v.data(), v.size()));
  }
  void OnCell(std::nullptr_t) { Set(duckdb_append_null(appender)); }
  void OnCell(uint32_t v) { Set(duckdb_append_int64(appender, v)); }
  void OnCell(int32_t v) { Set(duckdb_append_int64(appender, v)); }

  void Set(duckdb_state state) {
    if (status.ok() && state != DuckDBSuccess) {
      const char* err = duckdb_appender_error(appender);
      status = base::ErrStatus("DuckDB append failed: %s",
                               err ? err : "unknown DuckDB error");
    }
  }

  duckdb_appender appender;
  base::Status status = base::OkStatus();
};

struct DataframeScanBindData {
  DuckDbEngine* engine = nullptr;
  std::string table_name;
  dataframe::Dataframe* dataframe = nullptr;
  dataframe::DataframeSpec spec;
};

struct DataframeScanState {
  dataframe::Cursor<dataframe::ErrorValueFetcher> cursor;
  dataframe::ErrorValueFetcher value_fetcher;
  std::vector<uint32_t> projected_columns;
};

void DeleteDataframeScanBindData(void* ptr) {
  delete static_cast<DataframeScanBindData*>(ptr);
}

void DeleteDataframeScanState(void* ptr) {
  delete static_cast<DataframeScanState*>(ptr);
}

base::StatusOr<uint64_t> ColumnBitmapFromProjectedColumns(
    const std::vector<uint32_t>& projected_columns) {
  uint64_t bitmap = 0;
  for (uint32_t col : projected_columns) {
    if (col >= 64) {
      return base::ErrStatus(
          "DuckDB dataframe scan cannot project column %" PRIu32
          " because dataframe plans use a 64-bit projection mask",
          col);
    }
    bitmap |= 1ull << col;
  }
  return bitmap;
}

struct DuckDbCellWriter : dataframe::CellCallback {
  explicit DuckDbCellWriter(duckdb_vector _vector, idx_t _row)
      : vector(_vector), row(_row) {}

  void OnCell(int64_t v) {
    auto* data = static_cast<int64_t*>(duckdb_vector_get_data(vector));
    data[row] = v;
  }
  void OnCell(double v) {
    auto* data = static_cast<double*>(duckdb_vector_get_data(vector));
    data[row] = v;
  }
  void OnCell(NullTermStringView v) {
    duckdb_unsafe_vector_assign_string_element_len(
        vector, row, v.data(), static_cast<idx_t>(v.size()));
  }
  void OnCell(std::nullptr_t) {
    duckdb_vector_ensure_validity_writable(vector);
    duckdb_validity_set_row_invalid(duckdb_vector_get_validity(vector), row);
  }
  void OnCell(uint32_t v) {
    auto* data = static_cast<int64_t*>(duckdb_vector_get_data(vector));
    data[row] = static_cast<int64_t>(v);
  }
  void OnCell(int32_t v) {
    auto* data = static_cast<int64_t*>(duckdb_vector_get_data(vector));
    data[row] = static_cast<int64_t>(v);
  }

  duckdb_vector vector;
  idx_t row;
};

}  // namespace

DuckDbEngine::QueryResult::QueryResult() = default;

DuckDbEngine::QueryResult::~QueryResult() {
  duckdb_destroy_result(&result);
}

DuckDbEngine::QueryResult::QueryResult(QueryResult&& other) noexcept {
  *this = std::move(other);
}

DuckDbEngine::QueryResult& DuckDbEngine::QueryResult::operator=(
    QueryResult&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  duckdb_destroy_result(&result);
  result = other.result;
  other.result = {};
  row_count = other.row_count;
  current_row = other.current_row;
  has_current_row = other.has_current_row;
  is_done = other.is_done;
  status = std::move(other.status);
  sql = std::move(other.sql);
  string_scratch = std::move(other.string_scratch);
  bytes_scratch = std::move(other.bytes_scratch);
  statement_count = other.statement_count;
  statement_count_with_output = other.statement_count_with_output;
  return *this;
}

bool DuckDbEngine::QueryResult::Step() {
  if (!status.ok() || is_done) {
    return false;
  }
  if (!has_current_row) {
    has_current_row = true;
    current_row = 0;
    return true;
  }
  ++current_row;
  if (current_row >= row_count) {
    is_done = true;
    has_current_row = false;
    return false;
  }
  return true;
}

bool DuckDbEngine::QueryResult::IsDone() const {
  return is_done;
}

uint32_t DuckDbEngine::QueryResult::ColumnCount() const {
  return static_cast<uint32_t>(
      duckdb_column_count(const_cast<duckdb_result*>(&result)));
}

uint32_t DuckDbEngine::QueryResult::StatementCount() const {
  return statement_count;
}

uint32_t DuckDbEngine::QueryResult::StatementCountWithOutput() const {
  return statement_count_with_output;
}

std::string DuckDbEngine::QueryResult::GetColumnName(uint32_t col) const {
  const char* name =
      duckdb_column_name(const_cast<duckdb_result*>(&result), col);
  return name ? name : "";
}

SqlValue DuckDbEngine::QueryResult::Get(uint32_t col) const {
  PERFETTO_DCHECK(has_current_row);
  duckdb_result* mutable_result = const_cast<duckdb_result*>(&result);
  idx_t row = static_cast<idx_t>(current_row);
  if (duckdb_value_is_null(mutable_result, col, row)) {
    return SqlValue();
  }

  duckdb_type type = duckdb_column_type(mutable_result, col);
  if (type == DUCKDB_TYPE_BOOLEAN) {
    return SqlValue::Long(duckdb_value_boolean(mutable_result, col, row));
  }
  if (type == DUCKDB_TYPE_TINYINT) {
    return SqlValue::Long(duckdb_value_int8(mutable_result, col, row));
  }
  if (type == DUCKDB_TYPE_SMALLINT) {
    return SqlValue::Long(duckdb_value_int16(mutable_result, col, row));
  }
  if (type == DUCKDB_TYPE_INTEGER) {
    return SqlValue::Long(duckdb_value_int32(mutable_result, col, row));
  }
  if (type == DUCKDB_TYPE_BIGINT) {
    return SqlValue::Long(duckdb_value_int64(mutable_result, col, row));
  }
  if (type == DUCKDB_TYPE_UTINYINT) {
    return SqlValue::Long(duckdb_value_uint8(mutable_result, col, row));
  }
  if (type == DUCKDB_TYPE_USMALLINT) {
    return SqlValue::Long(duckdb_value_uint16(mutable_result, col, row));
  }
  if (type == DUCKDB_TYPE_UINTEGER) {
    return SqlValue::Long(duckdb_value_uint32(mutable_result, col, row));
  }
  if (type == DUCKDB_TYPE_UBIGINT) {
    return SqlValue::Long(
        static_cast<int64_t>(duckdb_value_uint64(mutable_result, col, row)));
  }
  if (type == DUCKDB_TYPE_HUGEINT) {
    duckdb_hugeint value = duckdb_value_hugeint(mutable_result, col, row);
    if (value.upper == 0 &&
        value.lower <=
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return SqlValue::Long(static_cast<int64_t>(value.lower));
    }
    if (value.upper == -1 &&
        value.lower >=
            (uint64_t{1} << (std::numeric_limits<uint64_t>::digits - 1))) {
      uint64_t magnitude = ~value.lower + 1;
      if (magnitude ==
          (uint64_t{1} << (std::numeric_limits<uint64_t>::digits - 1))) {
        return SqlValue::Long(std::numeric_limits<int64_t>::min());
      }
      return SqlValue::Long(-static_cast<int64_t>(magnitude));
    }
  }
  if (type == DUCKDB_TYPE_FLOAT) {
    return SqlValue::Double(
        static_cast<double>(duckdb_value_float(mutable_result, col, row)));
  }
  if (type == DUCKDB_TYPE_DOUBLE) {
    return SqlValue::Double(duckdb_value_double(mutable_result, col, row));
  }
  if (type == DUCKDB_TYPE_VARCHAR || type == DUCKDB_TYPE_STRING_LITERAL) {
    const char* value = duckdb_value_varchar_internal(mutable_result, col, row);
    string_scratch = value ? value : "";
    return SqlValue::String(string_scratch.c_str());
  }
  if (type == DUCKDB_TYPE_BLOB) {
    duckdb_blob blob = duckdb_value_blob(mutable_result, col, row);
    bytes_scratch.assign(static_cast<const uint8_t*>(blob.data),
                         static_cast<const uint8_t*>(blob.data) + blob.size);
    duckdb_free(blob.data);
    SqlValue value;
    value.type = SqlValue::kBytes;
    value.bytes_value = bytes_scratch.data();
    value.bytes_count = bytes_scratch.size();
    return value;
  }
  char* value = duckdb_value_varchar(mutable_result, col, row);
  string_scratch = value ? value : "";
  duckdb_free(value);
  return SqlValue::String(string_scratch.c_str());
}

DuckDbEngine::DuckDbEngine() {
  PERFETTO_CHECK(duckdb_open(nullptr, &db_) == DuckDBSuccess);
  duckdb_add_replacement_scan(db_, &DuckDbEngine::DataframeReplacementScan,
                              this, nullptr);
  PERFETTO_CHECK(duckdb_connect(db_, &conn_) == DuckDBSuccess);
  PERFETTO_CHECK(RegisterDataframeTableFunction().ok());
}

DuckDbEngine::~DuckDbEngine() {
  if (conn_) {
    duckdb_disconnect(&conn_);
  }
  if (db_) {
    duckdb_close(&db_);
  }
}

base::Status DuckDbEngine::ExecForSetup(const std::string& sql) {
  duckdb_result result;
  duckdb_state state = duckdb_query(conn_, sql.c_str(), &result);
  base::Status status = DuckDbStateToStatus(state, &result, "DuckDB setup");
  duckdb_destroy_result(&result);
  return status;
}

base::Status DuckDbEngine::ImportStaticTables(
    const std::vector<StaticTable>& tables,
    std::pair<int64_t, int64_t> trace_bounds) {
  for (const StaticTable& table : tables) {
    RETURN_IF_ERROR(CreateTableFromDataframe(table));
    RETURN_IF_ERROR(AppendRowsFromDataframe(table));
  }
  RETURN_IF_ERROR(
      ExecForSetup("CREATE OR REPLACE TABLE trace_bounds("
                   "start_ts BIGINT, end_ts BIGINT)"));
  return ExecForSetup(
      base::StackString<256>("INSERT INTO trace_bounds VALUES(%" PRId64
                             ", %" PRId64 ")",
                             trace_bounds.first, trace_bounds.second)
          .ToStdString());
}

base::Status DuckDbEngine::CreateTableFromDataframe(const StaticTable& table) {
  auto spec = table.dataframe->CreateSpec();
  std::vector<std::string> cols;
  cols.reserve(spec.column_names.size());
  for (uint32_t i = 0; i < spec.column_names.size(); ++i) {
    cols.push_back(QuoteIdent(spec.column_names[i]) + " " +
                   DuckDbTypeForColumn(spec.column_specs[i]));
  }
  std::string sql = "CREATE OR REPLACE TABLE " + QuoteIdent(table.name) + "(" +
                    base::Join(cols, ", ") + ")";
  return ExecForSetup(sql);
}

base::Status DuckDbEngine::AppendRowsFromDataframe(const StaticTable& table) {
  duckdb_appender appender = nullptr;
  if (duckdb_appender_create(conn_, nullptr, table.name.c_str(), &appender) !=
      DuckDBSuccess) {
    return base::ErrStatus("DuckDB appender creation failed for table %s",
                           table.name.c_str());
  }

  for (uint32_t row = 0; row < table.dataframe->row_count(); ++row) {
    for (uint32_t col = 0; col < table.dataframe->column_count(); ++col) {
      AppenderCellCallback callback(appender);
      table.dataframe->GetCell(row, col, callback);
      if (!callback.status.ok()) {
        duckdb_appender_destroy(&appender);
        return callback.status;
      }
    }
    if (duckdb_appender_end_row(appender) != DuckDBSuccess) {
      const char* err = duckdb_appender_error(appender);
      base::Status status = base::ErrStatus(
          "DuckDB failed to finish row for table %s: %s", table.name.c_str(),
          err ? err : "unknown DuckDB error");
      duckdb_appender_destroy(&appender);
      return status;
    }
  }

  if (duckdb_appender_close(appender) != DuckDBSuccess) {
    const char* err = duckdb_appender_error(appender);
    base::Status status =
        base::ErrStatus("DuckDB failed to close appender for table %s: %s",
                        table.name.c_str(), err ? err : "unknown DuckDB error");
    duckdb_appender_destroy(&appender);
    return status;
  }
  duckdb_appender_destroy(&appender);
  return base::OkStatus();
}

base::Status DuckDbEngine::ImportSqliteObjects(
    sqlite3* db,
    std::pair<int64_t, int64_t> trace_bounds) {
  std::vector<std::string> table_names;
  sqlite3_stmt* stmt = nullptr;
  const char* sql = R"(
    SELECT name
    FROM sqlite_master
    WHERE type IN ('table', 'view')
      AND name NOT LIKE 'sqlite_%'
    ORDER BY type, name
  )";
  int ret = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
  if (ret != SQLITE_OK) {
    return base::ErrStatus("DuckDB import failed to read SQLite catalog: %s",
                           sqlite3_errmsg(db));
  }
  for (;;) {
    ret = sqlite3_step(stmt);
    if (ret == SQLITE_DONE) {
      break;
    }
    if (ret != SQLITE_ROW) {
      base::Status status = base::ErrStatus(
          "DuckDB import failed while reading SQLite catalog: %s",
          sqlite3_errmsg(db));
      sqlite3_finalize(stmt);
      return status;
    }
    const char* name =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (name && *name) {
      table_names.emplace_back(name);
    }
  }
  sqlite3_finalize(stmt);

  table_names.push_back("stats");
  table_names.push_back("sqlstats");
  return ImportSqliteTables(db, table_names, trace_bounds);
}

base::Status DuckDbEngine::ImportSqliteTables(
    sqlite3* db,
    const std::vector<std::string>& table_names,
    std::pair<int64_t, int64_t> trace_bounds) {
  for (const std::string& table_name : table_names) {
    std::string sql = "SELECT * FROM " + QuoteIdent(table_name);
    sqlite3_stmt* stmt = nullptr;
    int ret = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (ret != SQLITE_OK) {
      PERFETTO_DLOG("DuckDB import skipped SQLite table %s: %s",
                    table_name.c_str(), sqlite3_errmsg(db));
      continue;
    }

    base::Status status = CreateTableFromSqliteStatement(table_name, stmt);
    if (status.ok()) {
      status = AppendRowsFromSqliteStatement(table_name, stmt);
    }
    sqlite3_finalize(stmt);
    if (!status.ok()) {
      PERFETTO_DLOG("DuckDB import skipped SQLite table %s: %s",
                    table_name.c_str(), status.c_message());
      continue;
    }
  }
  RETURN_IF_ERROR(
      ExecForSetup("CREATE OR REPLACE TABLE trace_bounds("
                   "start_ts BIGINT, end_ts BIGINT)"));
  return ExecForSetup(
      base::StackString<256>("INSERT INTO trace_bounds VALUES(%" PRId64
                             ", %" PRId64 ")",
                             trace_bounds.first, trace_bounds.second)
          .ToStdString());
}

base::Status DuckDbEngine::RegisterDataframes(
    const std::vector<StaticTable>& tables) {
  dataframes_.clear();
  for (const StaticTable& table : tables) {
    RETURN_IF_ERROR(RegisterDataframe(table.name, table.dataframe));
    static constexpr std::string_view kIntrinsicPrefix = "__intrinsic_";
    if (table.name.rfind(kIntrinsicPrefix, 0) == 0) {
      RETURN_IF_ERROR(RegisterDataframe(
          table.name.substr(kIntrinsicPrefix.size()), table.dataframe));
    }
  }
  return base::OkStatus();
}

base::Status DuckDbEngine::InstallPrelude() {
  static constexpr const char* kPreludeSql[] = {
      R"(
        CREATE OR REPLACE VIEW thread AS
        SELECT
          id,
          id AS utid,
          tid,
          name,
          start_ts,
          end_ts,
          upid,
          is_main_thread,
          is_idle,
          machine_id,
          arg_set_id
        FROM __intrinsic_thread
      )",
      R"(
        CREATE OR REPLACE VIEW process AS
        SELECT
          id,
          id AS upid,
          pid,
          name,
          start_ts,
          end_ts,
          parent_upid,
          uid,
          android_appid,
          android_user_id,
          cmdline,
          arg_set_id,
          machine_id
        FROM __intrinsic_process
      )",
      R"(
        CREATE OR REPLACE VIEW args AS
        SELECT
          id,
          arg_set_id,
          flat_key,
          key,
          int_value,
          string_value,
          real_value,
          value_type,
          CASE value_type
            WHEN 'int' THEN CAST(int_value AS VARCHAR)
            WHEN 'uint' THEN printf('%u', int_value)
            WHEN 'string' THEN string_value
            WHEN 'real' THEN CAST(real_value AS VARCHAR)
            WHEN 'pointer' THEN printf('0x%x', int_value)
            WHEN 'bool' THEN CASE WHEN int_value != 0 THEN 'true' ELSE 'false' END
            WHEN 'json' THEN string_value
            ELSE NULL
          END AS display_value
        FROM __intrinsic_args
      )",
      R"(
        CREATE OR REPLACE VIEW sched AS
        SELECT
          id,
          ts,
          dur,
          ucpu AS cpu,
          utid,
          end_state,
          priority,
          ucpu,
          ts + dur AS ts_end
        FROM __intrinsic_sched_slice
      )",
      R"(
        CREATE OR REPLACE VIEW sched_slice AS
        SELECT
          id,
          ts,
          dur,
          cpu,
          utid,
          end_state,
          priority,
          ucpu
        FROM sched
      )",
      R"(
        CREATE OR REPLACE VIEW counter_track AS
        SELECT
          id,
          name,
          parent_id,
          type,
          dimension_arg_set_id,
          source_arg_set_id,
          machine_id,
          counter_unit AS unit,
          NULL::VARCHAR AS description
        FROM __intrinsic_track
        WHERE event_type = 'counter'
      )",
      R"(
        CREATE OR REPLACE VIEW cpu_counter_track AS
        SELECT
          ct.id,
          ct.name,
          ct.type,
          ct.parent_id,
          ct.source_arg_set_id,
          ct.machine_id,
          ct.unit,
          ct.description,
          args.int_value AS cpu
        FROM counter_track AS ct
        JOIN args
          ON ct.dimension_arg_set_id = args.arg_set_id
        WHERE args.key = 'cpu'
      )",
      R"(
        CREATE OR REPLACE VIEW process_counter_track AS
        SELECT
          ct.id,
          ct.name,
          ct.type,
          ct.parent_id,
          ct.source_arg_set_id,
          ct.machine_id,
          ct.unit,
          ct.description,
          args.int_value AS upid
        FROM counter_track AS ct
        JOIN args
          ON ct.dimension_arg_set_id = args.arg_set_id
        WHERE args.key = 'upid'
      )",
      R"(
        CREATE OR REPLACE VIEW thread_counter_track AS
        SELECT
          ct.id,
          ct.name,
          ct.type,
          ct.parent_id,
          ct.source_arg_set_id,
          ct.machine_id,
          ct.unit,
          ct.description,
          args.int_value AS utid
        FROM counter_track AS ct
        JOIN args
          ON ct.dimension_arg_set_id = args.arg_set_id
        WHERE args.key = 'utid'
      )",
      R"(
        CREATE OR REPLACE VIEW counters AS
        SELECT
          v.id,
          v.ts,
          v.track_id,
          v.value,
          v.arg_set_id,
          t.name,
          t.unit
        FROM counter AS v
        JOIN counter_track AS t
          ON v.track_id = t.id
        ORDER BY ts
      )",
  };
  for (const char* sql : kPreludeSql) {
    RETURN_IF_ERROR(ExecForSetup(sql));
  }
  return base::OkStatus();
}

base::Status DuckDbEngine::RegisterDataframe(const std::string& name,
                                             dataframe::Dataframe* dataframe) {
  if (!dataframe) {
    return base::ErrStatus("DuckDB cannot register null dataframe %s",
                           name.c_str());
  }
  dataframes_[name] = RegisteredDataframe{dataframe, dataframe->CreateSpec()};
  return base::OkStatus();
}

const DuckDbEngine::RegisteredDataframe* DuckDbEngine::GetRegisteredDataframe(
    const std::string& name) const {
  auto it = dataframes_.find(name);
  return it == dataframes_.end() ? nullptr : &it->second;
}

base::Status DuckDbEngine::RegisterDataframeTableFunction() {
  duckdb_table_function function = duckdb_create_table_function();
  duckdb_table_function_set_name(function, "perfetto_df_scan");

  duckdb_logical_type table_name_type =
      duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
  duckdb_table_function_add_parameter(function, table_name_type);
  duckdb_destroy_logical_type(&table_name_type);

  duckdb_table_function_set_extra_info(function, this, nullptr);
  duckdb_table_function_set_bind(function, &DuckDbEngine::DataframeScanBind);
  duckdb_table_function_set_init(function, &DuckDbEngine::DataframeScanInit);
  duckdb_table_function_set_function(function,
                                     &DuckDbEngine::DataframeScanFunction);
  duckdb_table_function_supports_projection_pushdown(function, true);

  duckdb_state state = duckdb_register_table_function(conn_, function);
  duckdb_destroy_table_function(&function);
  if (state != DuckDBSuccess) {
    return base::ErrStatus("DuckDB failed to register dataframe table function");
  }
  return base::OkStatus();
}

void DuckDbEngine::DataframeReplacementScan(
    duckdb_replacement_scan_info info,
    const char* table_name,
    void* data) {
  if (!table_name || !data) {
    return;
  }
  auto* engine = static_cast<DuckDbEngine*>(data);
  if (!engine->GetRegisteredDataframe(table_name)) {
    return;
  }
  duckdb_replacement_scan_set_function_name(info, "perfetto_df_scan");
  duckdb_value value = duckdb_create_varchar(table_name);
  duckdb_replacement_scan_add_parameter(info, value);
  duckdb_destroy_value(&value);
}

void DuckDbEngine::DataframeScanBind(duckdb_bind_info info) {
  auto* engine = static_cast<DuckDbEngine*>(duckdb_bind_get_extra_info(info));
  if (!engine) {
    duckdb_bind_set_error(info, "Perfetto DuckDB dataframe engine missing");
    return;
  }
  if (duckdb_bind_get_parameter_count(info) != 1) {
    duckdb_bind_set_error(info, "perfetto_df_scan requires one table name");
    return;
  }

  duckdb_value table_value = duckdb_bind_get_parameter(info, 0);
  char* raw_table_name = duckdb_get_varchar(table_value);
  if (!raw_table_name || !*raw_table_name) {
    duckdb_bind_set_error(info, "perfetto_df_scan table name is empty");
    if (raw_table_name) {
      duckdb_free(raw_table_name);
    }
    duckdb_destroy_value(&table_value);
    return;
  }

  std::string table_name(raw_table_name);
  duckdb_free(raw_table_name);
  duckdb_destroy_value(&table_value);

  const RegisteredDataframe* registered =
      engine->GetRegisteredDataframe(table_name);
  if (!registered) {
    duckdb_bind_set_error(
        info,
        base::StackString<256>("Perfetto dataframe table not found: %s",
                               table_name.c_str())
            .c_str());
    return;
  }

  auto* bind_data = new DataframeScanBindData();
  bind_data->engine = engine;
  bind_data->table_name = std::move(table_name);
  bind_data->dataframe = registered->dataframe;
  bind_data->spec = registered->spec;

  for (uint32_t i = 0; i < bind_data->spec.column_names.size(); ++i) {
    duckdb_logical_type type = duckdb_create_logical_type(
        DuckDbLogicalTypeForColumn(bind_data->spec.column_specs[i]));
    duckdb_bind_add_result_column(
        info, bind_data->spec.column_names[i].c_str(), type);
    duckdb_destroy_logical_type(&type);
  }
  duckdb_bind_set_cardinality(
      info, static_cast<idx_t>(bind_data->dataframe->row_count()), true);
  duckdb_bind_set_bind_data(info, bind_data, DeleteDataframeScanBindData);
}

void DuckDbEngine::DataframeScanInit(duckdb_init_info info) {
  auto* bind_data =
      static_cast<DataframeScanBindData*>(duckdb_init_get_bind_data(info));
  if (!bind_data) {
    duckdb_init_set_error(info, "Perfetto DuckDB dataframe bind data missing");
    return;
  }

  auto* state = new DataframeScanState();
  idx_t projected_count = duckdb_init_get_column_count(info);
  state->projected_columns.reserve(static_cast<size_t>(projected_count));
  for (idx_t i = 0; i < projected_count; ++i) {
    state->projected_columns.push_back(
        static_cast<uint32_t>(duckdb_init_get_column_index(info, i)));
  }

  std::vector<dataframe::FilterSpec> filters;
  std::vector<dataframe::DistinctSpec> distinct;
  std::vector<dataframe::SortSpec> sort;
  dataframe::LimitSpec limit;
  base::StatusOr<uint64_t> projected_column_bitmap =
      ColumnBitmapFromProjectedColumns(state->projected_columns);
  if (!projected_column_bitmap.ok()) {
    std::string error = projected_column_bitmap.status().c_message();
    delete state;
    duckdb_init_set_error(info, error.c_str());
    return;
  }
  base::StatusOr<dataframe::Dataframe::QueryPlan> plan =
      bind_data->dataframe->PlanQuery(filters, distinct, sort, limit,
                                      *projected_column_bitmap);
  if (!plan.ok()) {
    std::string error = plan.status().c_message();
    delete state;
    duckdb_init_set_error(info, error.c_str());
    return;
  }

  bind_data->dataframe->PrepareCursor(*plan, state->cursor);
  state->cursor.Execute(state->value_fetcher);
  duckdb_init_set_max_threads(info, 1);
  duckdb_init_set_init_data(info, state, DeleteDataframeScanState);
}

void DuckDbEngine::DataframeScanFunction(duckdb_function_info info,
                                         duckdb_data_chunk output) {
  auto* state =
      static_cast<DataframeScanState*>(duckdb_function_get_init_data(info));
  if (!state) {
    duckdb_function_set_error(info,
                              "Perfetto DuckDB dataframe scan state missing");
    duckdb_data_chunk_set_size(output, 0);
    return;
  }

  idx_t row = 0;
  idx_t max_rows = duckdb_vector_size();
  while (row < max_rows && !state->cursor.Eof()) {
    for (idx_t out_col = 0; out_col < state->projected_columns.size();
         ++out_col) {
      duckdb_vector vector = duckdb_data_chunk_get_vector(output, out_col);
      DuckDbCellWriter writer(vector, row);
      state->cursor.Cell(state->projected_columns[out_col], writer);
    }
    state->cursor.Next();
    ++row;
  }
  duckdb_data_chunk_set_size(output, row);
}

base::Status DuckDbEngine::CreateTableFromSqliteStatement(
    const std::string& table_name,
    sqlite3_stmt* stmt) {
  int column_count = sqlite3_column_count(stmt);
  if (column_count == 0) {
    return base::ErrStatus("DuckDB import found no columns for table %s",
                           table_name.c_str());
  }

  std::vector<std::string> cols;
  cols.reserve(static_cast<size_t>(column_count));
  for (int i = 0; i < column_count; ++i) {
    const char* name = sqlite3_column_name(stmt, i);
    std::string column_name =
        name && *name ? std::string(name)
                      : base::StackString<32>("col_%d", i).ToStdString();
    cols.push_back(QuoteIdent(column_name) + " " +
                   DuckDbTypeForSqliteColumn(sqlite3_column_decltype(stmt, i)));
  }
  std::string sql = "CREATE OR REPLACE TABLE " + QuoteIdent(table_name) + "(" +
                    base::Join(cols, ", ") + ")";
  return ExecForSetup(sql);
}

base::Status DuckDbEngine::AppendRowsFromSqliteStatement(
    const std::string& table_name,
    sqlite3_stmt* stmt) {
  duckdb_appender appender = nullptr;
  if (duckdb_appender_create(conn_, nullptr, table_name.c_str(), &appender) !=
      DuckDBSuccess) {
    return base::ErrStatus("DuckDB appender creation failed for table %s",
                           table_name.c_str());
  }

  for (;;) {
    int ret = sqlite3_step(stmt);
    if (ret == SQLITE_DONE) {
      break;
    }
    if (ret != SQLITE_ROW) {
      base::Status status = base::ErrStatus(
          "DuckDB import failed while reading SQLite table %s: %s",
          table_name.c_str(), sqlite3_errmsg(sqlite3_db_handle(stmt)));
      duckdb_appender_destroy(&appender);
      return status;
    }

    int column_count = sqlite3_column_count(stmt);
    for (int col = 0; col < column_count; ++col) {
      base::Status status = base::OkStatus();
      switch (sqlite3_column_type(stmt, col)) {
        case SQLITE_INTEGER:
          status = DuckDbAppendStateToStatus(
              duckdb_append_int64(appender, sqlite3_column_int64(stmt, col)),
              appender, table_name.c_str(), "DuckDB append failed");
          break;
        case SQLITE_FLOAT:
          status = DuckDbAppendStateToStatus(
              duckdb_append_double(appender, sqlite3_column_double(stmt, col)),
              appender, table_name.c_str(), "DuckDB append failed");
          break;
        case SQLITE_TEXT: {
          const char* value =
              reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
          int bytes = sqlite3_column_bytes(stmt, col);
          status = DuckDbAppendStateToStatus(
              duckdb_append_varchar_length(appender, value,
                                           static_cast<idx_t>(bytes)),
              appender, table_name.c_str(), "DuckDB append failed");
          break;
        }
        case SQLITE_BLOB: {
          const void* value = sqlite3_column_blob(stmt, col);
          int bytes = sqlite3_column_bytes(stmt, col);
          status = DuckDbAppendStateToStatus(
              duckdb_append_blob(appender, value, static_cast<idx_t>(bytes)),
              appender, table_name.c_str(), "DuckDB append failed");
          break;
        }
        case SQLITE_NULL:
          status = DuckDbAppendStateToStatus(duckdb_append_null(appender),
                                             appender, table_name.c_str(),
                                             "DuckDB append failed");
          break;
      }
      if (!status.ok()) {
        duckdb_appender_destroy(&appender);
        return status;
      }
    }

    base::Status status = DuckDbAppendStateToStatus(
        duckdb_appender_end_row(appender), appender, table_name.c_str(),
        "DuckDB failed to finish row");
    if (!status.ok()) {
      duckdb_appender_destroy(&appender);
      return status;
    }
  }

  base::Status status = DuckDbAppendStateToStatus(
      duckdb_appender_close(appender), appender, table_name.c_str(),
      "DuckDB failed to close appender");
  duckdb_appender_destroy(&appender);
  return status;
}

base::StatusOr<DuckDbEngine::QueryResult>
DuckDbEngine::ExecuteReturningStatement(const std::string& sql) {
  QueryResult result;
  result.sql = sql;
  duckdb_state state = duckdb_query(conn_, sql.c_str(), &result.result);
  if (state != DuckDBSuccess) {
    result.status =
        DuckDbStateToStatus(state, &result.result, "DuckDB query failed");
    return base::StatusOr<QueryResult>(std::move(result));
  }
  result.statement_count = 1;
  result.statement_count_with_output =
      duckdb_column_count(&result.result) == 0 ? 0 : 1;
  result.row_count = duckdb_row_count(&result.result);
  result.is_done = result.row_count == 0;
  return base::StatusOr<QueryResult>(std::move(result));
}

base::StatusOr<DuckDbEngine::QueryResult> DuckDbEngine::Execute(
    const std::string& sql,
    const std::vector<SqlPackage>& sql_packages) {
  base::FlatHashMap<std::string, PerfettoSqlPreprocessor::Macro> macros;
  QueryResult setup_result;
  std::optional<QueryResult> final_result;

  std::function<base::Status(SqlSource)> execute_source =
      [&](SqlSource source) -> base::Status {
    PerfettoSqlParser parser(std::move(source), macros);
    while (parser.Next()) {
      const PerfettoSqlParser::Statement& stmt = parser.statement();
      std::optional<std::string> duckdb_sql;

      if (std::get_if<PerfettoSqlParser::SqliteSql>(&stmt)) {
        duckdb_sql = parser.statement_sql().sql();
      } else if (const auto* create_table =
                     std::get_if<PerfettoSqlParser::CreateTable>(&stmt)) {
        duckdb_sql = CreatePerfettoTableSql(*create_table);
      } else if (const auto* create_view =
                     std::get_if<PerfettoSqlParser::CreateView>(&stmt)) {
        duckdb_sql = CreatePerfettoViewSql(*create_view);
      } else if (const auto* include =
                     std::get_if<PerfettoSqlParser::Include>(&stmt)) {
        if (included_modules_.find(include->key) != included_modules_.end()) {
          ++setup_result.statement_count;
          continue;
        }
        const std::string* module_sql =
            FindSqlModule(sql_packages, include->key);
        if (!module_sql) {
          return base::ErrStatus("INCLUDE: unknown module '%s'",
                                 include->key.c_str());
        }
        RETURN_IF_ERROR(execute_source(
            SqlSource::FromModuleInclude(*module_sql, include->key)));
        included_modules_.insert(include->key);
        ++setup_result.statement_count;
        continue;
      } else {
        return base::ErrStatus(
            "DuckDB experimental path does not support this PerfettoSQL "
            "statement");
      }

      PERFETTO_CHECK(duckdb_sql);
      if (final_result) {
        setup_result.statement_count += final_result->statement_count;
      }

      ASSIGN_OR_RETURN(final_result,
                       ExecuteReturningStatement(
                           ApplyDuckDbSqlRewrites(std::move(*duckdb_sql))));
    }
    return parser.status();
  };

  RETURN_IF_ERROR(execute_source(SqlSource::FromExecuteQuery(sql)));
  if (!final_result) {
    return base::ErrStatus("No valid SQL to run");
  }
  final_result->statement_count += setup_result.statement_count;
  final_result->statement_count_with_output +=
      setup_result.statement_count_with_output;
  return base::StatusOr<QueryResult>(std::move(*final_result));
}

void DuckDbEngine::Interrupt() {
  duckdb_interrupt(conn_);
}

}  // namespace perfetto::trace_processor
