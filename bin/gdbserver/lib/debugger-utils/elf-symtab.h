// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "lib/ftl/macros.h"

#include "elf-reader.h"

namespace debugserver {
namespace elf {

struct Symbol {
  // weak pointer into the string section of the loaded ELF file
  const char* name = nullptr;
  uint64_t addr = 0;
  uint64_t size = 0;
};

class SymbolTable {
 public:
  SymbolTable(const std::string& file_name, const std::string& contents);
  ~SymbolTable();

  const std::string& file_name() const { return file_name_; }
  const std::string& contents() const { return contents_; }
  size_t num_symbols() const { return num_symbols_; }

  // |symtab_type| is one of SHT_SYMTAB, SHT_DYNSYM.
  bool Populate(Reader* elf, unsigned symtab_type);

  // |index| must be valid.
  const Symbol& GetSymbol(size_t index) const { return symbols_[index]; }

  const Symbol* FindSymbol(uint64_t addr) const;

  void Dump(FILE*) const;

 private:
  void Finalize();

  // For debugging/informational purposes only.
  const std::string file_name_;

  // For debugging/informational purposes only.
  // One may wish to load a file's symbols into different symbol tables.
  // E.g., One may wish to keep SHT_SYMTAB and SHT_DYNSYM separate.
  // This string is for identifying the contents of the symtab in error
  // messages, etc.
  const std::string contents_;

  size_t num_symbols_ = 0;
  // Once Finalize() is called this is sorted by |Symbol.addr|.
  Symbol* symbols_ = nullptr;

  // To separate our lifetime with that of the ELF reader, we store the
  // strings here.
  std::unique_ptr<SectionContents> string_section_;

  FTL_DISALLOW_COPY_AND_ASSIGN(SymbolTable);
};

}  // namespace elf
}  // namespace debugserver
