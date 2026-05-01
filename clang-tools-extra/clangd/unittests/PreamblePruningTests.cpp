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

#include "PreamblePruning.h"
#include "TestTU.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Type.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

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

} // namespace
} // namespace clangd
} // namespace clang
