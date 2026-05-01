//===--- PreambleStorage.h - Process-lifetime preamble cache ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// LURE-local: in-memory preamble cache. Bounded by process lifetime only;
// restarting clangd drops the cache. The on-disk persistence path is C2 and
// depends on extra serialization machinery for clangd-internal payloads.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PREAMBLESTORAGE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PREAMBLESTORAGE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <memory>
#include <string>

namespace clang {
namespace clangd {

struct PreambleData;

struct PreambleKeyInputs {
  llvm::StringRef AbsoluteTUPath;
  llvm::ArrayRef<std::string> CompileCommandArgv;
  llvm::StringRef ABITag;
  llvm::StringRef LURETag;
};

struct PreambleKey {
  std::string Digest;

  // LATTNER-LOCK H1: shardPrefix returns a borrow into the live Digest.
  llvm::StringRef shardPrefix() const {
    return llvm::StringRef(Digest).take_front(4);
  }

  bool operator==(const PreambleKey &O) const { return Digest == O.Digest; }
  bool operator!=(const PreambleKey &O) const { return !(*this == O); }

  static PreambleKey compute(const PreambleKeyInputs &);
};

class PreambleStorage {
public:
  virtual ~PreambleStorage() = default;

  // INVARIANT 1: load-or-rebuild is sound.
  // INVARIANT 4: cache hits are semantically identical to fresh build.
  virtual std::shared_ptr<const PreambleData>
  loadIfFresh(const PreambleKey &Key) const = 0;

  virtual void store(const PreambleKey &Key,
                     std::shared_ptr<const PreambleData> Preamble) = 0;

  virtual void onIncludeStructureInvalidated() = 0;
};

std::unique_ptr<PreambleStorage>
createMemoryPreambleStorage(unsigned MaxEntries = 32);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PREAMBLESTORAGE_H
