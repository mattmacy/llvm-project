//===--- PreamblePruningTests.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Algorithm-output mutation pins for the conservative reachability pass.
// Plan §6.1. Each test asserts a property of the kept-set returned by
// computeReachablePreambleDecls; the documented mutation in comments
// describes the input/structure mutation that breaks the test.
// Rule 11: no counter-only assertions.
//
//===----------------------------------------------------------------------===//

#include "Preamble.h"
#include "PreamblePruning.h"
#include "TestTU.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Type.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <cassert>

namespace clang {
namespace clangd {
namespace {

bool containsDeclNamed(const llvm::DenseSet<const Decl *> &Set,
                       llvm::StringRef Name) {
  for (const Decl *D : Set) {
    if (const auto *ND = llvm::dyn_cast<NamedDecl>(D)) {
      if (const auto *II = ND->getIdentifier()) {
        if (II->getName() == Name)
          return true;
      }
    }
  }
  return false;
}

bool containsImplicitSpec(const llvm::DenseSet<const Decl *> &Set,
                          llvm::StringRef PrimaryName) {
  for (const Decl *D : Set) {
    if (const auto *Spec = llvm::dyn_cast<ClassTemplateSpecializationDecl>(D)) {
      if (const auto *II = Spec->getIdentifier()) {
        if (II->getName() == PrimaryName)
          return true;
      }
    }
  }
  return false;
}

bool containsClassSpecWithBuiltinArg(const llvm::DenseSet<const Decl *> &Set,
                                     llvm::StringRef PrimaryName,
                                     BuiltinType::Kind Kind) {
  for (const Decl *D : Set) {
    const auto *Spec = llvm::dyn_cast<ClassTemplateSpecializationDecl>(D);
    if (!Spec)
      continue;
    const auto *II = Spec->getIdentifier();
    if (!II || II->getName() != PrimaryName)
      continue;
    const TemplateArgumentList &Args = Spec->getTemplateArgs();
    if (Args.size() != 1 || Args[0].getKind() != TemplateArgument::Type)
      continue;
    const auto *BT = Args[0].getAsType()->getAs<BuiltinType>();
    if (BT && BT->getKind() == Kind)
      return true;
  }
  return false;
}

llvm::DenseSet<const Decl *> runConservative(llvm::StringRef HeaderCode,
                                             llvm::StringRef Code) {
  TestTU TU;
  TU.HeaderCode = std::string(HeaderCode);
  TU.Code = std::string(Code);
  auto AST = TU.build();
  auto ROpt = computeReachablePreambleDecls(AST.getASTContext(), AST.getSema(),
                                            PreambleASTPruning::Conservative);
  EXPECT_TRUE(ROpt.has_value());
  return ROpt ? std::move(*ROpt) : llvm::DenseSet<const Decl *>();
}

struct CapturedKeptSets {
  std::optional<llvm::DenseSet<const clang::Decl *>> KeptDecls;
  std::optional<llvm::DenseSet<const clang::MacroDirective *>> KeptMacros;
  std::optional<PreambleASTPruning> PreambleDataPruning;
  llvm::StringMap<bool> KeptByName;

  bool IdentifierKept(llvm::StringRef Name) const {
    auto It = KeptByName.find(Name);
    return It != KeptByName.end() && It->second;
  }
};

CapturedKeptSets buildPreambleAndCaptureMacros(TestTU &TU,
                                               PreambleASTPruning Tier) {
  CapturedKeptSets Out;
  MockFS FS;
  auto Inputs = TU.inputs(FS);
  Inputs.Opts.Pruning = Tier;
  IgnoreDiagnostics Diags;
  auto CI = buildCompilerInvocation(Inputs, Diags);
  assert(CI && "Failed to build compiler invocation.");
  PreambleBuildCaptureHooks Hooks;
  Hooks.OnKeptDecls = [&](const llvm::DenseSet<const Decl *> &KeptDecls) {
    Out.KeptDecls = KeptDecls;
  };
  Hooks.OnKeptMacros =
      [&](const llvm::DenseSet<const MacroDirective *> &KeptMacros,
          Preprocessor &PP) {
        Out.KeptMacros = KeptMacros;
        for (const auto &MM : PP.macros()) {
          const IdentifierInfo *II = MM.first;
          MacroDirective *MD = PP.getLocalMacroDirectiveHistory(II);
          bool AnyKept = false;
          while (MD) {
            if (KeptMacros.contains(MD)) {
              AnyKept = true;
              break;
            }
            MD = MD->getPrevious();
          }
          Out.KeptByName[II->getName()] = AnyKept;
        }
      };
  setPreambleBuildCaptureHooksForTest(&Hooks);
  auto ResetHooks = llvm::make_scope_exit(
      [] { setPreambleBuildCaptureHooksForTest(nullptr); });
  auto Preamble = buildPreamble(testPath(TU.Filename), *CI, Inputs,
                                /*StoreInMemory=*/true,
                                /*PreambleCallback=*/nullptr);
  if (Preamble)
    Out.PreambleDataPruning = Preamble->Pruning;
  return Out;
}

TEST(PreamblePruningTest, KeepsBodyReachableDecls) {
  TestTU TU;
  TU.HeaderCode = R"cpp(
    namespace ns { struct Used { int x; }; struct Unused {}; }
    template<class T> struct V { T v; };
  )cpp";
  TU.Code = R"cpp(
    int f(ns::Used u) { return u.x; }
    V<int> vi;
  )cpp";
  auto AST = TU.build();
  auto ROpt = computeReachablePreambleDecls(AST.getASTContext(), AST.getSema(),
                                            PreambleASTPruning::Conservative);
  ASSERT_TRUE(ROpt.has_value());
  auto &R = *ROpt;
  EXPECT_TRUE(containsDeclNamed(R, "Used"));
  EXPECT_TRUE(containsDeclNamed(R, "V"));
  EXPECT_TRUE(containsImplicitSpec(R, "V"));
  EXPECT_FALSE(containsDeclNamed(R, "Unused"));
}

// Mutation pin 1: replacing the IdResolver-walk in
// ReachabilityWalker::seed() with a no-op causes "Used" to never enter
// the seed and (per plan §3.3.2 closure rule 1) never to reach the kept
// set. Test fails when the pin is mutated.
TEST(PreamblePruningTest, MutationPin_SeedIncludesIdResolver) {
  TestTU TU;
  TU.HeaderCode = R"cpp(
    struct Used { int x; };
    struct Unused {};
  )cpp";
  TU.Code = R"cpp(
    Used* g() { return nullptr; }
  )cpp";
  auto AST = TU.build();
  auto ROpt = computeReachablePreambleDecls(AST.getASTContext(), AST.getSema(),
                                            PreambleASTPruning::Conservative);
  ASSERT_TRUE(ROpt.has_value());
  EXPECT_TRUE(containsDeclNamed(*ROpt, "Used"));
  EXPECT_FALSE(containsDeclNamed(*ROpt, "Unused"));
}

// Mutation pin 2 (Torvalds MED on explicit specs, plan §3.3.2 step 8):
// explicit specialization of a reached primary template. Mutating the
// restrictive-spec collector to skip step 7/8 collapses the kept set
// such that "specialized_member" disappears.
TEST(PreamblePruningTest, MutationPin_ExplicitSpecOfReachedPrimary) {
  TestTU TU;
  TU.HeaderCode = R"cpp(
    template<class T> struct V { T v; };
    template<> struct V<int> { int specialized_member; };
    template<> struct V<float> { float other_member; };
  )cpp";
  TU.Code = R"cpp(
    V<int> vi;
  )cpp";
  auto AST = TU.build();
  auto ROpt = computeReachablePreambleDecls(AST.getASTContext(), AST.getSema(),
                                            PreambleASTPruning::Conservative);
  ASSERT_TRUE(ROpt.has_value());
  EXPECT_TRUE(
      containsClassSpecWithBuiltinArg(*ROpt, "V", BuiltinType::Int));
  EXPECT_FALSE(
      containsClassSpecWithBuiltinArg(*ROpt, "V", BuiltinType::Float));
}

// Off-tier: function returns nullopt - wired in C1, kept invariant in C2.
TEST(PreamblePruningTest, OffTierReturnsNullopt) {
  TestTU TU;
  TU.HeaderCode = "struct S {};";
  TU.Code = "S s;";
  auto AST = TU.build();
  auto ROpt = computeReachablePreambleDecls(AST.getASTContext(), AST.getSema(),
                                            PreambleASTPruning::Off);
  EXPECT_FALSE(ROpt.has_value());
}

TEST(PreamblePruningTest, ConservativeKeepsFieldDeclInClassInits) {
  auto Kept = runConservative(R"cpp(
    inline int compute();
    struct S { int x = compute(); };
  )cpp",
                              "S s;");
  EXPECT_TRUE(containsDeclNamed(Kept, "compute"));
}

TEST(PreamblePruningTest, ConservativeKeepsDefaultTemplateArg) {
  auto Kept = runConservative(R"cpp(
    struct DefAlloc {};
    template <class T, class A = DefAlloc> struct V { A alloc; };
  )cpp",
                              "V<int> v;");
  EXPECT_TRUE(containsDeclNamed(Kept, "DefAlloc"));
}

TEST(PreamblePruningTest, ConservativeKeepsNNSChain) {
  auto Kept = runConservative(R"cpp(
    namespace N {
    struct Outer { static int value; };
    struct S { int x = N::Outer::value; };
    } // namespace N
  )cpp",
                              "N::S s;");
  EXPECT_TRUE(containsDeclNamed(Kept, "Outer"));
  EXPECT_TRUE(containsDeclNamed(Kept, "value"));
}

TEST(PreamblePruningTest, ConservativeKeepsLambdaCapture) {
  auto Kept = runConservative(R"cpp(
    inline int helper();
    struct S { int y = [x = helper()] { return x; }(); };
  )cpp",
                              "S s;");
  EXPECT_TRUE(containsDeclNamed(Kept, "x"));
}

TEST(PreamblePruningTest, ConservativeKeepsADLBegin) {
  auto Kept = runConservative(R"cpp(
    namespace N {
    struct R {};
    inline int *begin(R &);
    inline int *end(R &);
    struct S {
      int x = [] {
        R r;
        for (auto y : r) {
        }
        return 0;
      }();
    };
    } // namespace N
  )cpp",
                              "N::S s;");
  EXPECT_TRUE(containsDeclNamed(Kept, "begin"));
  EXPECT_TRUE(containsDeclNamed(Kept, "end"));
}

TEST(PreamblePruningTest, AggressiveKeepsMacroExpandedInsideKeptDecl) {
  TestTU TU;
  TU.HeaderCode = R"cpp(
    #define USED 42
    #define UNUSED 7
    struct Reachable { int v = USED; };
  )cpp";
  TU.Code = R"cpp(
    Reachable r;
  )cpp";
  auto Captured =
      buildPreambleAndCaptureMacros(TU, PreambleASTPruning::Aggressive);
  ASSERT_TRUE(Captured.KeptMacros.has_value());
  EXPECT_TRUE(Captured.IdentifierKept("USED"));
  EXPECT_FALSE(Captured.IdentifierKept("UNUSED"));
}

TEST(PreamblePruningTest, AggressiveDropsMacroDefinedButNotExpanded) {
  TestTU TU;
  TU.HeaderCode = R"cpp(
    #define DEFINED_NEVER_USED 1
    int g = 0;
  )cpp";
  TU.Code = R"cpp( int x = g; )cpp";
  auto Captured =
      buildPreambleAndCaptureMacros(TU, PreambleASTPruning::Aggressive);
  ASSERT_TRUE(Captured.KeptMacros.has_value());
  EXPECT_FALSE(Captured.IdentifierKept("DEFINED_NEVER_USED"));
}

TEST(PreamblePruningTest, AggressiveBodyRedefineDoesNotCrash) {
  TestTU TU;
  TU.HeaderCode = R"cpp(
    #define X 1
    int header_x = X;
  )cpp";
  TU.Code = R"cpp(
    #undef X
    #define X 99
    int body_x = X;
  )cpp";
  auto Captured =
      buildPreambleAndCaptureMacros(TU, PreambleASTPruning::Aggressive);
  ASSERT_TRUE(Captured.KeptMacros.has_value());
  EXPECT_TRUE(Captured.IdentifierKept("X"));
}

TEST(PreamblePruningTest, AggressivePreambleDataPruningPlumbed) {
  TestTU TU;
  TU.HeaderCode = "struct S { int x; };";
  TU.Code = "S s;";
  auto Captured =
      buildPreambleAndCaptureMacros(TU, PreambleASTPruning::Aggressive);
  ASSERT_TRUE(Captured.PreambleDataPruning.has_value());
  EXPECT_EQ(*Captured.PreambleDataPruning, PreambleASTPruning::Aggressive);
}

TEST(PreamblePruningTest, ConservativeMacroPathReturnsNullopt) {
  TestTU TU;
  TU.HeaderCode = R"cpp(
    struct Reached { int x; };
  )cpp";
  TU.Code = R"cpp(
    Reached r;
  )cpp";
  auto Captured =
      buildPreambleAndCaptureMacros(TU, PreambleASTPruning::Conservative);
  EXPECT_TRUE(Captured.KeptDecls.has_value());
  EXPECT_FALSE(Captured.KeptMacros.has_value());
}

} // namespace
} // namespace clangd
} // namespace clang
