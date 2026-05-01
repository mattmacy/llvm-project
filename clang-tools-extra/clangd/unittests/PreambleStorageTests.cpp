//===--- PreambleStorageTests.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PreambleStorage.h"
#include "Preamble.h"
#include "TestTU.h"
#include "gtest/gtest.h"
#include <memory>
#include <string>
#include <vector>

namespace clang {
namespace clangd {
namespace {

std::shared_ptr<const PreambleData> buildTinyPreamble() {
  TestTU TU;
  TU.Code = "int probe;";
  TU.Filename = "test.cc";
  return TU.preamble();
}

PreambleKey makeKey(llvm::StringRef Path, llvm::ArrayRef<std::string> Argv,
                    llvm::StringRef LURETag) {
  PreambleKeyInputs Inputs;
  Inputs.AbsoluteTUPath = Path;
  Inputs.CompileCommandArgv = Argv;
  Inputs.ABITag = "test-abi";
  Inputs.LURETag = LURETag;
  return PreambleKey::compute(Inputs);
}

TEST(PreambleStorageTest, MemoryRoundTripStoreThenLoadPointerEqual) {
  auto Storage = createMemoryPreambleStorage(/*MaxEntries=*/4);
  auto Preamble = buildTinyPreamble();
  ASSERT_NE(Preamble, nullptr);
  std::vector<std::string> Argv = {"clang", "-std=c++17", "test.cc"};
  auto Key = makeKey("/abs/test.cc", Argv,
                     "preamble-cache-v1:pruning=off:codec=0:dict=0:fmt=1");

  Storage->store(Key, Preamble);
  auto Loaded = Storage->loadIfFresh(Key);
  ASSERT_NE(Loaded, nullptr);
  EXPECT_EQ(Loaded.get(), Preamble.get());
}

TEST(PreambleStorageTest, LURETagMismatchLoadReturnsNullptr) {
  auto Storage = createMemoryPreambleStorage(/*MaxEntries=*/4);
  auto Preamble = buildTinyPreamble();
  ASSERT_NE(Preamble, nullptr);
  std::vector<std::string> Argv = {"clang", "-std=c++17", "test.cc"};
  auto Off = makeKey("/abs/test.cc", Argv,
                     "preamble-cache-v1:pruning=off:codec=0:dict=0:fmt=1");
  auto Aggressive =
      makeKey("/abs/test.cc", Argv,
              "preamble-cache-v1:pruning=aggressive:codec=0:dict=0:fmt=1");

  Storage->store(Off, Preamble);
  EXPECT_EQ(Storage->loadIfFresh(Aggressive), nullptr);
}

TEST(PreambleStorageTest, MemoryLRUTightBudgetEvictsOldestFirst) {
  auto Storage = createMemoryPreambleStorage(/*MaxEntries=*/2);
  auto Preamble = buildTinyPreamble();
  ASSERT_NE(Preamble, nullptr);
  std::vector<std::string> Argv = {"clang", "-std=c++17", "test.cc"};

  auto K1 = makeKey("/abs/a.cc", Argv,
                    "preamble-cache-v1:pruning=off:codec=0:dict=0:fmt=1");
  auto K2 = makeKey("/abs/b.cc", Argv,
                    "preamble-cache-v1:pruning=off:codec=0:dict=0:fmt=1");
  auto K3 = makeKey("/abs/c.cc", Argv,
                    "preamble-cache-v1:pruning=off:codec=0:dict=0:fmt=1");
  auto K4 = makeKey("/abs/d.cc", Argv,
                    "preamble-cache-v1:pruning=off:codec=0:dict=0:fmt=1");

  Storage->store(K1, Preamble);
  Storage->store(K2, Preamble);
  Storage->store(K3, Preamble);
  EXPECT_EQ(Storage->loadIfFresh(K1), nullptr);
  EXPECT_NE(Storage->loadIfFresh(K2), nullptr);

  Storage->store(K4, Preamble);
  EXPECT_NE(Storage->loadIfFresh(K2), nullptr);
  EXPECT_EQ(Storage->loadIfFresh(K3), nullptr);
  EXPECT_NE(Storage->loadIfFresh(K4), nullptr);
}

} // namespace
} // namespace clangd
} // namespace clang
