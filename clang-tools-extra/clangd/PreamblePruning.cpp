//===--- PreamblePruning.cpp - Prune preamble PCH to reachable decls ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Conservative reachability pass: from the seed of decls Sema kept alive
// to parse the TU body, walk transitive type/template/base/using deps and
// return the closed set. ASTWriter::GetDeclRef gates emission against this
// set (gate already landed in Commit 1 at ASTWriter.cpp:6249-6258); the
// pass is the seed, the existing DeclTypesToEmit queue is the closure
// mechanism. See plan §3.3 + §3.5.0.
//
// Macros are out of scope here (Conservative keeps all macros per plan
// §3.3.3). Aggressive tier macro filter + restrictive specs lands in
// Commit 3.
//
//===----------------------------------------------------------------------===//

#include "PreamblePruning.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/Type.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/IdentifierResolver.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

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
  return "off";
}

namespace {

// Plan §3.3.4: bound on default-arg / initializer walker recursion.
constexpr unsigned kMaxRecursionDepth = 32;

// LATTNER-LOCK: name + polarity. Single positive-name kept-set; no
//   negative-polarity predicate. Population is monotone (plan §5
//   INVARIANT: monotone, fixed-point).
struct ReachabilityWalker {
  ASTContext &Ctx;
  Sema &S;
  PreambleASTPruning Tier;
  llvm::DenseSet<const Decl *> Kept;
  // CARMACK-LOCK §3.3.4: memoize Type -> underlying-Decls so default-arg
  //   trees and parameter-type walks don't redo work. Pass-lifetime; not
  //   ASTWriter-lifetime.
  llvm::DenseMap<const Type *, llvm::SmallVector<const Decl *, 4>>
      TypeUnderlyingDecls;
  llvm::SmallVector<const Decl *, 64> Worklist;
  uint64_t MemoHits = 0;
  uint64_t MemoMisses = 0;

  ReachabilityWalker(ASTContext &Ctx, Sema &S, PreambleASTPruning Tier)
      : Ctx(Ctx), S(S), Tier(Tier) {}

  void enqueue(const Decl *D) {
    if (!D)
      return;
    if (Kept.insert(D).second)
      Worklist.push_back(D);
  }

  llvm::ArrayRef<const Decl *> underlyingDecls(QualType QT) {
    if (QT.isNull())
      return {};
    llvm::SmallVector<const Decl *, 4> Out;
    if (const TagDecl *TD = QT->getAsTagDecl())
      Out.push_back(TD);
    if (const auto *RT = QT->getAs<RecordType>())
      Out.push_back(RT->getDecl());
    const Type *T = QT.getTypePtrOrNull();
    if (!T)
      return {};
    auto It = TypeUnderlyingDecls.find(T);
    if (It != TypeUnderlyingDecls.end()) {
      ++MemoHits;
      return It->second;
    }
    ++MemoMisses;
    if (const TypedefType *TT = T->getAs<TypedefType>())
      Out.push_back(TT->getDecl());
    if (const TemplateTypeParmType *TP = T->getAs<TemplateTypeParmType>())
      Out.push_back(TP->getDecl());
    if (const TemplateSpecializationType *TST =
            T->getAs<TemplateSpecializationType>()) {
      if (auto *TplD = TST->getTemplateName().getAsTemplateDecl())
        Out.push_back(TplD);
    }
    auto &Slot = TypeUnderlyingDecls[T];
    Slot.assign(Out.begin(), Out.end());
    return Slot;
  }

  // Plan §3.3.2 step 3 / 4: depth-bounded RecursiveASTVisitor over
  // expressions in default-args / VarDecl initializers. On overflow we
  // give up the walk; the TU-level GetDeclRef force-emit path is still
  // gated by the kept set, so a missed transitive enqueue here means the
  // closure recovers the dep through the DeclTypesToEmit queue at
  // WriteAST time.
  struct ExprWalker : RecursiveASTVisitor<ExprWalker> {
    ReachabilityWalker &W;
    unsigned Depth = 0;
    explicit ExprWalker(ReachabilityWalker &W) : W(W) {}

    bool TraverseStmt(Stmt *St) {
      if (!St)
        return true;
      if (Depth >= kMaxRecursionDepth)
        return true;
      ++Depth;
      bool R = RecursiveASTVisitor<ExprWalker>::TraverseStmt(St);
      --Depth;
      return R;
    }
    bool VisitDeclRefExpr(DeclRefExpr *E) {
      W.enqueue(E->getDecl());
      return true;
    }
    bool VisitMemberExpr(MemberExpr *E) {
      W.enqueue(E->getMemberDecl());
      return true;
    }
    bool VisitCXXConstructExpr(CXXConstructExpr *E) {
      W.enqueue(E->getConstructor());
      for (const Decl *D : W.underlyingDecls(E->getType()))
        W.enqueue(D);
      return true;
    }
    bool VisitCXXTemporaryObjectExpr(CXXTemporaryObjectExpr *E) {
      for (const Decl *D : W.underlyingDecls(E->getType()))
        W.enqueue(D);
      return true;
    }
  };

  void walkExpr(const Expr *E) {
    if (!E)
      return;
    ExprWalker EW(*this);
    EW.TraverseStmt(const_cast<Expr *>(E));
  }

  // Plan §3.3.1 seed.
  void seed() {
    for (const auto &ID : S.getPreprocessor().getIdentifierTable()) {
      const IdentifierInfo *II = ID.second;
      for (const Decl *D : S.IdResolver.decls(II))
        enqueue(D);
    }
    for (const auto &PI : S.PendingInstantiations)
      enqueue(PI.first);
    auto *External = S.getExternalSource();
    for (auto It = S.TentativeDefinitions.begin(External); It != S.TentativeDefinitions.end(); ++It)
      enqueue(*It);
    for (auto It = S.UnusedFileScopedDecls.begin(External); It != S.UnusedFileScopedDecls.end(); ++It)
      enqueue(*It);
    for (auto It = S.DelegatingCtorDecls.begin(External); It != S.DelegatingCtorDecls.end(); ++It)
      enqueue(*It);
    for (auto It = S.ExtVectorDecls.begin(External); It != S.ExtVectorDecls.end(); ++It)
      enqueue(*It);
    for (const auto &VTU : S.VTableUses)
      enqueue(VTU.first);
    enqueue(S.getStdNamespace());
    enqueue(S.getStdBadAlloc());
    enqueue(S.getStdAlignValT());
    enqueue(Ctx.getcudaConfigureCallDecl());
  }

  // Plan §3.3.2 step 1: enclosing DeclContexts up to TU.
  void closeContext(const Decl *D) {
    const DeclContext *DC = D->getDeclContext();
    while (DC && !DC->isTranslationUnit()) {
      if (const Decl *DCDecl = Decl::castFromDeclContext(DC))
        enqueue(DCDecl);
      DC = DC->getParent();
    }
  }

  // Plan §3.3.2 step 2 + ADL operator rule (§3.3.1) applied lazily.
  void closeRecord(const CXXRecordDecl *R) {
    if (!R || !R->hasDefinition())
      return;
    for (const auto &B : R->bases())
      for (const Decl *D : underlyingDecls(B.getType()))
        enqueue(D);
    if (auto *CTD = R->getDescribedClassTemplate())
      enqueue(CTD);
    if (auto *Spec = dyn_cast<ClassTemplateSpecializationDecl>(R))
      enqueue(Spec->getSpecializedTemplate());
    for (const Decl *Member : R->decls()) {
      enqueue(Member);
      if (const auto *FD = dyn_cast<FunctionDecl>(Member)) {
        if (FD->isOverloadedOperator())
          enqueue(FD);
      }
      if (const auto *FTD = dyn_cast<FunctionTemplateDecl>(Member)) {
        if (const auto *Tpl = FTD->getTemplatedDecl()) {
          if (Tpl->isOverloadedOperator())
            enqueue(FTD);
        }
      }
    }
    for (FriendDecl *FD : R->friends()) {
      if (NamedDecl *ND = FD->getFriendDecl())
        enqueue(ND);
    }
  }

  // Plan §3.3.2 step 3.
  void closeFunction(const FunctionDecl *F) {
    if (!F)
      return;
    for (const Decl *D : underlyingDecls(F->getReturnType()))
      enqueue(D);
    for (const ParmVarDecl *P : F->parameters()) {
      for (const Decl *D : underlyingDecls(P->getType()))
        enqueue(D);
      if (P->hasDefaultArg() && !P->hasUninstantiatedDefaultArg()) {
        if (const Expr *DE = P->getDefaultArg())
          walkExpr(DE);
      }
    }
    if (auto *FTD = F->getDescribedFunctionTemplate())
      enqueue(FTD);
    if (auto *Primary = F->getPrimaryTemplate())
      enqueue(Primary);
  }

  // Plan §3.3.2 step 4.
  void closeVar(const VarDecl *V) {
    if (!V)
      return;
    for (const Decl *D : underlyingDecls(V->getType()))
      enqueue(D);
    if (V->hasInit())
      walkExpr(V->getInit());
  }

  // Plan §3.3.2 step 5.
  void closeTypedef(const TypedefNameDecl *T) {
    if (!T)
      return;
    for (const Decl *D : underlyingDecls(T->getUnderlyingType()))
      enqueue(D);
  }

  // Plan §3.3.2 step 6.
  void closeUsing(const UsingDecl *U) {
    if (!U)
      return;
    for (auto It = U->shadow_begin(); It != U->shadow_end(); ++It)
      enqueue(*It);
  }

  // Plan §3.3.2 step 7 RESTRICTIVE + step 8 explicit-spec rule. Iterate
  // every reached primary template and add specializations whose argument-
  // type underlying-records are each independently kept.
  bool collectRestrictiveSpecs() {
    bool Added = false;
    llvm::SmallVector<ClassTemplateDecl *, 32> CTDs;
    llvm::SmallVector<FunctionTemplateDecl *, 32> FTDs;
    for (const Decl *D : Kept) {
      if (auto *CTD = dyn_cast<ClassTemplateDecl>(D))
        CTDs.push_back(const_cast<ClassTemplateDecl *>(CTD));
      else if (auto *FTD = dyn_cast<FunctionTemplateDecl>(D))
        FTDs.push_back(const_cast<FunctionTemplateDecl *>(FTD));
    }
    auto allArgsKept = [&](llvm::ArrayRef<TemplateArgument> Args) {
      bool SawTypeDecl = false;
      for (const TemplateArgument &TA : Args) {
        if (TA.getKind() != TemplateArgument::Type)
          continue;
        auto Underlying = underlyingDecls(TA.getAsType());
        if (!Underlying.empty())
          SawTypeDecl = true;
        for (const Decl *Under : Underlying) {
          if (!Kept.contains(Under))
            return false;
        }
      }
      return SawTypeDecl;
    };
    for (ClassTemplateDecl *CTD : CTDs) {
      for (auto *Spec : CTD->specializations()) {
        if (allArgsKept(Spec->getTemplateArgs().asArray())) {
          if (Kept.insert(Spec).second) {
            Worklist.push_back(Spec);
            Added = true;
          }
        }
      }
    }
    for (FunctionTemplateDecl *FTD : FTDs) {
      for (auto *Spec : FTD->specializations()) {
        if (const auto *Args = Spec->getTemplateSpecializationArgs()) {
          if (allArgsKept(Args->asArray())) {
            if (Kept.insert(Spec).second) {
              Worklist.push_back(Spec);
              Added = true;
            }
          }
        }
      }
    }
    return Added;
  }

  void runFixedPoint() {
    while (true) {
      while (!Worklist.empty()) {
        const Decl *D = Worklist.pop_back_val();
        closeContext(D);
        if (const auto *R = dyn_cast<CXXRecordDecl>(D))
          closeRecord(R);
        else if (const auto *F = dyn_cast<FunctionDecl>(D))
          closeFunction(F);
        else if (const auto *V = dyn_cast<VarDecl>(D))
          closeVar(V);
        else if (const auto *T = dyn_cast<TypedefNameDecl>(D))
          closeTypedef(T);
        else if (const auto *U = dyn_cast<UsingDecl>(D))
          closeUsing(U);
      }
      if (!collectRestrictiveSpecs())
        break;
    }
  }
};

} // namespace

// SAFETY: pruning pass runs on the PreambleThread, holds no locks,
//   calls back into neither TUScheduler nor CppFile. Concurrency: none.
//   We are alone in this AST. (Plan §15.3.)
//
// SAFETY: We do not call into Sema's lookup machinery during the pruning
//   pass. We only walk Decl-level pointers and Sema's pre-populated
//   bookkeeping vectors. (Plan §5.)
//
// INVARIANT: When Tier == Off the function is NOT called (the wrapping
//   override in CppFilePreambleCallbacks::computeEmittableDecls returns
//   nullopt before reaching here). When called, the returned set is
//   monotone, fixed-point. (Plan §5.)
//
// INVARIANT: result is closed under getDeclContext(),
//   CXXRecordDecl::bases(), and template specialization (RESTRICTIVE -
//   §3.3.2 step 7). (Plan §5.)
std::optional<llvm::DenseSet<const Decl *>>
computeReachablePreambleDecls(ASTContext &Ctx, Sema &S,
                              PreambleASTPruning Tier) {
  if (Tier == PreambleASTPruning::Off)
    return std::nullopt;

  ReachabilityWalker W(Ctx, S, Tier);
  W.seed();
  W.runFixedPoint();
  return std::move(W.Kept);
}

} // namespace clangd
} // namespace clang
