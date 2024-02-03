// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/symbol_table.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <fstream>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/types/span.h"
#include "./centipede/command.h"
#include "./centipede/control_flow.h"
#include "./centipede/logging.h"
#include "./centipede/pc_info.h"
#include "./centipede/thread_pool.h"
#include "./centipede/util.h"

namespace centipede {

bool SymbolTable::operator==(const SymbolTable &other) const {
  return this->entries_ == other.entries_;
}

void SymbolTable::ReadFromLLVMSymbolizer(std::istream &in) {
  // We remove some useless file prefixes for better human readability.
  const std::string_view file_prefixes_to_remove[] = {"/proc/self/cwd/", "./"};
  while (in) {
    // We (mostly) blindly trust the input format is correct.
    std::string func, file, empty;
    std::getline(in, func);
    std::getline(in, file);
    std::getline(in, empty);
    CHECK(empty.empty()) << "Unexpected symbolizer output format: " << VV(func)
                         << VV(file) << VV(empty);
    if (!in) break;
    for (auto &bad_prefix : file_prefixes_to_remove) {
      file = absl::StripPrefix(file, bad_prefix);
    }
    AddEntry(func, file);
  }
}

void SymbolTable::WriteToLLVMSymbolizer(std::ostream &out) {
  for (const Entry &entry : entries_) {
    out << entry.func << '\n';
    out << entry.file_line_col() << '\n';
    out << std::endl;
  }
}

void SymbolTable::GetSymbolsFromOneDso(absl::Span<const PCInfo> pc_infos,
                                       std::string_view dso_path,
                                       std::string_view symbolizer_path,
                                       std::string_view tmp_dir_path) {
  static std::atomic_size_t unique_id = 0;
  size_t unique_id_value = unique_id.fetch_add(1);
  std::string dso_basename = std::filesystem::path{dso_path}.filename();
  ScopedFile pcs_file{tmp_dir_path,
                      absl::StrCat(dso_basename, ".pcs.", unique_id_value)};
  ScopedFile symbols_file{
      tmp_dir_path, absl::StrCat(dso_basename, ".symbols.", unique_id_value)};

  // Create the input file (one PC per line).
  std::string pcs_string;
  for (const auto &pc_info : pc_infos) {
    absl::StrAppend(&pcs_string, "0x", absl::Hex(pc_info.pc), "\n");
  }
  WriteToLocalFile(pcs_file.path(), pcs_string);
  // Run the symbolizer.
  Command cmd(symbolizer_path,
              {
                  "--no-inlines",
                  "-e",
                  std::string(dso_path),
                  "<",
                  std::string(pcs_file.path()),
              },
              /*env=*/{}, symbols_file.path());

  LOG(INFO) << "Symbolizing " << pc_infos.size() << " PCs from "
            << dso_basename;

  int exit_code = cmd.Execute();
  if (exit_code != EXIT_SUCCESS) {
    LOG(ERROR) << "system() failed: " << VV(cmd.ToString()) << VV(exit_code);
    return;
  }
  // Get and process the symbolizer output.
  std::ifstream symbolizer_output(std::string{symbols_file.path()});
  size_t old_size = size();
  ReadFromLLVMSymbolizer(symbolizer_output);
  size_t new_size = size();
  size_t added_size = new_size - old_size;
  if (added_size != pc_infos.size())
    LOG(ERROR) << "Symbolization failed: debug symbols will not be used";
}

void SymbolTable::GetSymbolsFromBinary(const PCTable &pc_table,
                                       const DsoTable &dso_table,
                                       std::string_view symbolizer_path,
                                       std::string_view tmp_dir_path) {
  // NOTE: --symbolizer_path=/dev/null is a somewhat expected alternative to
  // "" that users might pass.
  if (symbolizer_path.empty() || symbolizer_path == "/dev/null") {
    LOG(WARNING) << "Symbolizer unspecified: debug symbols will not be used";
    SetAllToUnknown(pc_table.size());
    return;
  }

  LOG(INFO) << "Symbolizing " << dso_table.size() << " instrumented DSOs.";

  // Iterate all DSOs, symbolize their respective PCs.
  // Symbolizing the PCs can take time, so we
  // record them in parallel into separate symbol tables,
  // and later merge.
  std::vector<SymbolTable> symbol_tables(dso_table.size());
  size_t pc_idx_begin = 0;
  {
    // Symbolization is quite IO-bound so we arbitrarily run 30 at once
    // even if we have few CPUs.
    const size_t num_threads = std::min(dso_table.size(), 30UL);
    centipede::ThreadPool thread_pool(num_threads);
    for (size_t dso_id = 0; dso_id < dso_table.size(); ++dso_id) {
      const auto &dso_info = dso_table[dso_id];
      auto &symbol_table = symbol_tables[dso_id];
      CHECK_LE(pc_idx_begin + dso_info.num_instrumented_pcs, pc_table.size())
          << VV(pc_idx_begin) << VV(dso_info.num_instrumented_pcs);
      const absl::Span<const PCInfo> pc_infos = {pc_table.data() + pc_idx_begin,
                                                 dso_info.num_instrumented_pcs};
      thread_pool.Schedule([&dso_info, pc_infos, symbolizer_path, tmp_dir_path,
                            &symbol_table]() {
        symbol_table.GetSymbolsFromOneDso(pc_infos, dso_info.path,
                                          symbolizer_path, tmp_dir_path);
      });
      pc_idx_begin += dso_info.num_instrumented_pcs;
    }
  }
  for (const auto &table : symbol_tables) {
    AddEntries(table);
  }
  CHECK_EQ(pc_idx_begin, pc_table.size());

  if (size() != pc_table.size()) {
    // Something went wrong. Set symbols to unknown so the sizes of pc_table and
    // symbols always match.
    SetAllToUnknown(pc_table.size());
  }
}

void SymbolTable::SetAllToUnknown(size_t size) {
  entries_.resize(size);
  for (auto &entry : entries_) {
    entry = {"?", "?"};
  }

  table_.clear();
}

void SymbolTable::AddEntry(std::string_view func,
                           std::string_view file_line_col) {
  if (absl::StrContains(file_line_col, "?")) {
    AddEntryInternal(func, file_line_col, 0, 0);
    return;
  }
  const std::vector<std::string_view> file_line_col_split =
      absl::StrSplit(file_line_col, ':');
  CHECK_LE(file_line_col_split.size(), 3);
  CHECK_GE(file_line_col_split.size(), 1)
      << "Unexpected symbolizer input format when getting source location: "
      << file_line_col;
  int line = -1;
  int col = -1;
  if (file_line_col_split.size() >= 2) {
    CHECK(absl::SimpleAtoi(file_line_col_split[1], &line))
        << "Unable to convert line number string to an int: "
        << file_line_col_split[1];
  }
  if (file_line_col_split.size() == 3) {
    CHECK(absl::SimpleAtoi(file_line_col_split[2], &col))
        << "Unable to convert column number string to an int: "
        << file_line_col_split[2];
  }
  AddEntryInternal(func, file_line_col_split[0], line, col);
}

void SymbolTable::AddEntryInternal(std::string_view func, std::string_view file,
                                   int line, int col) {
  entries_.emplace_back(Entry{GetOrInsert(func), GetOrInsert(file), line, col});
}

std::string_view SymbolTable::GetOrInsert(std::string_view str) {
  return *table_.insert(std::string{str}).first;
}

void SymbolTable::AddEntries(const SymbolTable &other) {
  for (const auto &entry : other.entries_) {
    AddEntryInternal(entry.func, entry.file, entry.line, entry.col);
  }
}

}  // namespace centipede
