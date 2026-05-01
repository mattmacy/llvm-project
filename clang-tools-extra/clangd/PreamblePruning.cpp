//===--- PreamblePruning.cpp - Prune preamble PCH to reachable decls ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Commit 1: scaffolding only. `computeReachablePreambleDecls` returns
// `std::nullopt` regardless of tier, so wiring the option through to here
// is observably a no-op (the caller never installs a kept set on
// `ASTWriter`, `isEmittable` returns true for every decl, and the
// resulting PCH is byte-equivalent to upstream output).
//
// Commit 2 lands the conservative reachability pass; Commit 3 lands the
// aggressive tier. See docs/plans/2026-05-01-clangd-pch-ast-pruning.md.
//
//===----------------------------------------------------------------------===//

#include "PreamblePruning.h"

namespace clang {
namespace clangd {

llvm::StringRef toCanonicalString(PreambleASTPruning Tier) {
  switch (Tier) {
  case PreambleASTPruning::Off:
    return "off";
  case PreambleASTPruning::Conservative:
    return "conservative";
  case PreambleASTPruning::Aggressive:
    return "aggressive";
  }
  // Unreachable; keep llvm_unreachable out of the header to avoid an
  // additional include in callers.
  return "off";
}

std::optional<llvm::DenseSet<const Decl *>>
computeReachablePreambleDecls(ASTContext & /*Ctx*/, Sema & /*S*/,
                              PreambleASTPruning /*Tier*/) {
  // Commit 1 stub: no kept set installed regardless of tier.
  // Commit 2 will branch on Tier and run the reachability walk per
  // plan section 3.3. Until then, returning nullopt preserves upstream
  // behavior even when --preamble-ast-pruning=conservative is passed
  // (the flag is wired but inert).
  return std::nullopt;
}

} // namespace clangd
} // namespace clang
