//===--- PreamblePruning.h - Prune preamble PCH to reachable decls -*- C++ -*-//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// LURE-local extension. Prune the AST decls serialized into a clangd preamble
// PCH down to those reachable from the TU body's name-lookup roots. Cuts
// preamble RAM at the cost of preamble-build CPU. Disk shards remain
// authoritative; lazy fault-in via ASTReader::FindExternalVisibleDeclsByName
// recovers any decl the conservative tier dropped that the body actually
// queries.
//
// See docs/plans/2026-05-01-clangd-pch-ast-pruning.md for the full design.
//
// LATTNER-LOCK: canonical wire-format strings.
//   This header owns the spelling of PreambleASTPruning enumerator names
//   for cross-plan consumption (sister plan
//   docs/plans/2026-05-01-clangd-preamble-persistence.md uses
//   toCanonicalString() for cache-key LURETags).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PREAMBLEPRUNING_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PREAMBLEPRUNING_H

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace clang {
class ASTContext;
class Decl;
class MacroDirective;
class Preprocessor;
class Sema;
namespace clangd {

/// Tier of preamble AST pruning.
///
/// Off          - Upstream behavior: preamble PCH carries every parsed decl.
/// Conservative - Drop top-level decls unreachable from the TU body's
///                name-lookup roots. Keep all macros + all template
///                specializations of any reached primary template.
///                Query-result-equivalent to Off on the existing test corpus.
/// Aggressive   - Conservative plus: drop macros not name-reachable from a
///                kept decl, drop low-fanout implicit template instantiations.
///                May regress hover/signature-help on highly-templated code.
///                Recommended for UE-scale workloads (see plan §14).
enum class PreambleASTPruning { Off, Conservative, Aggressive };

/// Canonical wire-format spelling of the tier value. Sister plan
/// (clangd-preamble-persistence) consumes this for the cache-key LURETag.
/// Stable; never returns a different string for the same enumerator across
/// LLVM versions.
llvm::StringRef toCanonicalString(PreambleASTPruning);

/// Compute the set of Decl pointers in `Ctx` reachable from the TU body's
/// name-lookup roots (as recorded by `Sema`). Only the decls in the
/// returned set should be serialized into the PCH; the rest are recovered
/// lazily by `ASTReader::FindExternalVisibleDeclsByName` when the body
/// re-faults them.
///
/// Commit 1 (this commit): stub returning `std::nullopt` regardless of
/// `Tier`. Commit 2 lands the conservative reachability pass; Commit 3
/// lands the aggressive tier. Returning `nullopt` means "no install" -
/// `ASTWriter::setEmittablePreambleDecls` is never called and `isEmittable`
/// returns true for every decl, so the resulting PCH is byte-equivalent
/// to upstream output.
///
/// LATTNER-LOCK: return shape - std::optional<DenseSet>, not bare DenseSet.
///   nullopt = tier=Off (no install). empty-set = legal but never produced
///   by Conservative/Aggressive in real TUs.
///
/// INVARIANT: result is a superset of the seed set described in plan
///   section 3.3.1 (TU body's first-pass name-lookup decls plus the
///   TranslationUnitDecl itself).
/// INVARIANT: result is closed under plan section 3.3.2's transitive-dep
///   relation.
std::optional<llvm::DenseSet<const Decl *>>
computeReachablePreambleDecls(ASTContext &Ctx, Sema &S,
                              PreambleASTPruning Tier);

/// Plan §3.3.3 (v4) aggressive tier: compute the set of
/// MacroDirectives allowed to land in the PCH. Returns nullopt when
/// Tier != Aggressive (Off and Conservative keep all macros).
///
/// When non-nullopt, the returned set contains every MacroDirective
/// whose IdentifierInfo names a macro whose expansion site lies inside
/// a kept-Decl SourceRange. The expansion-site lookup reads the
/// PreprocessingRecord on the Preprocessor; the caller (Preamble.cpp
/// buildPreamble) is responsible for setting
/// PreprocessorOpts::DetailedRecord = true on the CompilerInvocation
/// when Tier == Aggressive -- see plan §3.3.3.B.
///
/// If PP.getPreprocessingRecord() returns nullptr (the
/// CompilerInvocation flag was not set, or the preprocessor was
/// constructed before the flag took effect), this function logs a
/// warning and returns std::nullopt -- degrading aggressive-tier
/// macro filter to conservative behavior for macros only.
///
/// CARMACK-LOCK: aggressive macro filter index strategy (v4).
///   Naive O(macros × kept-decls) = O(10^9) on UE preambles. Index
///   kept-Decl SourceRange begins as a sorted interval table; each
///   macro-expansion SourceRange is a range-overlap query -- O(log N
///   + k) for k overlapping intervals. Total cost ~1.7e6 ops on
///   UE-scale preambles + 8 MB transient RAM during preamble build.
std::optional<llvm::DenseSet<const clang::MacroDirective *>>
computeReachablePreambleMacros(
    clang::ASTContext &Ctx, clang::Sema &S, clang::Preprocessor &PP,
    const llvm::DenseSet<const clang::Decl *> &KeptDecls,
    PreambleASTPruning Tier);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PREAMBLEPRUNING_H
