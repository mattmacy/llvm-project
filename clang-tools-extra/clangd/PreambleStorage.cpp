//===--- PreambleStorage.cpp - Process-lifetime preamble cache -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PreambleStorage.h"
#include "Preamble.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SHA256.h"
#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <utility>

namespace clang {
namespace clangd {

PreambleKey PreambleKey::compute(const PreambleKeyInputs &Inputs) {
  llvm::SHA256 Hasher;
  auto AddField = [&](llvm::StringRef Value) {
    Hasher.update(Value);
    Hasher.update(llvm::StringRef("\0", 1));
  };

  AddField(Inputs.AbsoluteTUPath);
  for (const std::string &Arg : Inputs.CompileCommandArgv)
    AddField(Arg);
  AddField(Inputs.ABITag);
  AddField(Inputs.LURETag);

  return {llvm::toHex(Hasher.final(), /*LowerCase=*/true)};
}

namespace {

class MemoryPreambleStorage : public PreambleStorage {
public:
  explicit MemoryPreambleStorage(unsigned MaxEntries) : MaxEntries(MaxEntries) {}

  std::shared_ptr<const PreambleData>
  loadIfFresh(const PreambleKey &Key) const override {
    std::lock_guard<std::mutex> Lock(M);
    auto It = Map.find(Key.Digest);
    if (It == Map.end())
      return nullptr;
    LRU.splice(LRU.begin(), LRU, It->second.second);
    return It->second.first;
  }

  void store(const PreambleKey &Key,
             std::shared_ptr<const PreambleData> Preamble) override {
    if (MaxEntries == 0)
      return;
    std::lock_guard<std::mutex> Lock(M);
    auto It = Map.find(Key.Digest);
    if (It != Map.end()) {
      It->second.first = std::move(Preamble);
      LRU.splice(LRU.begin(), LRU, It->second.second);
      return;
    }
    LRU.push_front(Key);
    Map[Key.Digest] = std::make_pair(std::move(Preamble), LRU.begin());
    while (Map.size() > MaxEntries) {
      const std::string EvictDigest = LRU.back().Digest;
      Map.erase(EvictDigest);
      LRU.pop_back();
    }
  }

  void onIncludeStructureInvalidated() override {
    std::lock_guard<std::mutex> Lock(M);
    Map.clear();
    LRU.clear();
    Generation.fetch_add(1, std::memory_order_relaxed);
  }

private:
  mutable std::mutex M;
  // LRU bookkeeping is mutated on cache hits, which are logically const.
  mutable std::list<PreambleKey> LRU;
  mutable llvm::StringMap<
      std::pair<std::shared_ptr<const PreambleData>,
                std::list<PreambleKey>::iterator>>
      Map;
  unsigned MaxEntries;
  std::atomic<uint64_t> Generation{0};
};

} // namespace

std::unique_ptr<PreambleStorage>
createMemoryPreambleStorage(unsigned MaxEntries) {
  return std::make_unique<MemoryPreambleStorage>(MaxEntries);
}

} // namespace clangd
} // namespace clang
