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

  void addType(QualType QT) {
    for (const Decl *D : underlyingDecls(QT))
      enqueue(D);
  }

  void closeOverNNS(const NestedNameSpecifier *NNS) {
    while (NNS) {
      switch (NNS->getKind()) {
      case NestedNameSpecifier::Namespace:
        enqueue(NNS->getAsNamespace());
        break;
      case NestedNameSpecifier::NamespaceAlias:
        enqueue(NNS->getAsNamespaceAlias());
        break;
      case NestedNameSpecifier::TypeSpec:
      case NestedNameSpecifier::TypeSpecWithTemplate:
        addType(QualType(NNS->getAsType(), 0));
        break;
      case NestedNameSpecifier::Super:
        enqueue(NNS->getAsRecordDecl());
        break;
      case NestedNameSpecifier::Identifier:
      case NestedNameSpecifier::Global:
        break;
      }
      NNS = NNS->getPrefix();
    }
  }

  void closeOverTemplateArg(const TemplateArgument &TA) {
    using Kind = TemplateArgument::ArgKind;
    switch (TA.getKind()) {
    case Kind::Null:
      break;
    case Kind::Type:
      addType(TA.getAsType());
      break;
    case Kind::Declaration:
      enqueue(TA.getAsDecl());
      break;
    case Kind::NullPtr:
      addType(TA.getNullPtrType());
      break;
    case Kind::Integral:
      addType(TA.getIntegralType());
      break;
    case Kind::Template:
    case Kind::TemplateExpansion:
      if (auto *D = TA.getAsTemplate().getAsTemplateDecl())
        enqueue(D);
      break;
    case Kind::Expression:
      walkExpr(TA.getAsExpr());
      break;
    case Kind::StructuralValue:
      addType(TA.getStructuralValueType());
      break;
    case Kind::Pack:
      for (const TemplateArgument &Inner : TA.pack_elements())
        closeOverTemplateArg(Inner);
      break;
    }
  }

  void closeOverTemplateArgs(const TemplateArgumentList &Args) {
    for (const TemplateArgument &TA : Args.asArray())
      closeOverTemplateArg(TA);
  }

  void closeOverADLCustomizationPoints(QualType QT) {
    static constexpr llvm::StringLiteral CPFns[] = {
        "begin", "end",   "cbegin", "cend", "rbegin",
        "rend",  "swap",  "size",   "data", "empty",
    };
    llvm::SmallVector<const NamespaceDecl *, 4> Namespaces;
    for (const Decl *D : underlyingDecls(QT)) {
      const DeclContext *DC = D->getDeclContext();
      while (DC && !DC->isTranslationUnit()) {
        if (const auto *NS = dyn_cast<NamespaceDecl>(DC)) {
          bool Seen = false;
          for (const NamespaceDecl *SeenNS : Namespaces) {
            if (SeenNS == NS) {
              Seen = true;
              break;
            }
          }
          if (!Seen)
            Namespaces.push_back(NS);
        }
        DC = DC->getParent();
      }
    }
    for (const NamespaceDecl *NS : Namespaces) {
      for (const Decl *Member : NS->decls()) {
        const auto *ND = dyn_cast<NamedDecl>(Member);
        if (!ND)
          continue;
        const IdentifierInfo *II = ND->getIdentifier();
        if (!II)
          continue;
        llvm::StringRef Name = II->getName();
        bool Match = false;
        for (llvm::StringLiteral CPFn : CPFns) {
          if (Name == CPFn) {
            Match = true;
            break;
          }
        }
        if (!Match)
          continue;
        if (isa<FunctionDecl>(ND) || isa<FunctionTemplateDecl>(ND))
          enqueue(ND);
      }
    }
  }

  void closeTemplateParameterDefaults(const TemplateParameterList *Params) {
    if (!Params)
      return;
    for (const NamedDecl *Param : *Params) {
      if (const auto *TTP = dyn_cast<TemplateTypeParmDecl>(Param)) {
        if (TTP->hasDefaultArgument())
          closeOverTemplateArg(TTP->getDefaultArgument().getArgument());
        continue;
      }
      if (const auto *NTTP = dyn_cast<NonTypeTemplateParmDecl>(Param)) {
        if (NTTP->hasDefaultArgument())
          closeOverTemplateArg(NTTP->getDefaultArgument().getArgument());
        continue;
      }
      if (const auto *TTPD = dyn_cast<TemplateTemplateParmDecl>(Param)) {
        if (TTPD->hasDefaultArgument())
          closeOverTemplateArg(TTPD->getDefaultArgument().getArgument());
      }
    }
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
      if (NestedNameSpecifierLoc Q = E->getQualifierLoc())
        W.closeOverNNS(Q.getNestedNameSpecifier());
      return true;
    }
    bool VisitMemberExpr(MemberExpr *E) {
      W.enqueue(E->getMemberDecl());
      if (NestedNameSpecifierLoc Q = E->getQualifierLoc())
        W.closeOverNNS(Q.getNestedNameSpecifier());
      return true;
    }
    bool VisitCXXConstructExpr(CXXConstructExpr *E) {
      W.enqueue(E->getConstructor());
      W.addType(E->getType());
      return true;
    }
    bool VisitCXXTemporaryObjectExpr(CXXTemporaryObjectExpr *E) {
      W.enqueue(E->getConstructor());
      W.addType(E->getType());
      return true;
    }
    bool VisitCallExpr(CallExpr *E) {
      W.enqueue(E->getDirectCallee());
      return true;
    }
    bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *E) {
      W.enqueue(E->getDirectCallee());
      return true;
    }
    bool VisitCXXMemberCallExpr(CXXMemberCallExpr *E) {
      W.enqueue(E->getMethodDecl());
      return true;
    }
    bool VisitCXXDependentScopeMemberExpr(CXXDependentScopeMemberExpr *E) {
      if (NestedNameSpecifierLoc Q = E->getQualifierLoc())
        W.closeOverNNS(Q.getNestedNameSpecifier());
      return true;
    }
    bool VisitUnresolvedLookupExpr(UnresolvedLookupExpr *E) {
      for (NamedDecl *D : E->decls())
        W.enqueue(D);
      return true;
    }
    bool VisitUnresolvedMemberExpr(UnresolvedMemberExpr *E) {
      for (NamedDecl *D : E->decls())
        W.enqueue(D);
      return true;
    }
    bool VisitLambdaExpr(LambdaExpr *E) {
      for (const LambdaCapture &Cap : E->captures()) {
        if (Cap.capturesVariable())
          W.enqueue(Cap.getCapturedVar());
      }
      return true;
    }
    bool VisitCXXForRangeStmt(CXXForRangeStmt *S) {
      W.enqueue(S->getLoopVariable());
      if (const Expr *Range = S->getRangeInit()) {
        W.addType(Range->getType());
        W.closeOverADLCustomizationPoints(Range->getType());
      }
      W.walkStmt(S->getBeginStmt());
      W.walkStmt(S->getEndStmt());
      return true;
    }
    bool VisitCXXNewExpr(CXXNewExpr *E) {
      W.enqueue(E->getOperatorNew());
      return true;
    }
    bool VisitCXXDeleteExpr(CXXDeleteExpr *E) {
      W.enqueue(E->getOperatorDelete());
      return true;
    }
  };

  void walkStmt(const Stmt *S) {
    if (!S)
      return;
    ExprWalker EW(*this);
    EW.TraverseStmt(const_cast<Stmt *>(S));
  }

  void walkExpr(const Expr *E) {
    walkStmt(E);
  }

  void closeOverFieldInits(const CXXRecordDecl *RD) {
    if (!RD)
      return;
    for (const FieldDecl *FD : RD->fields()) {
      if (const Expr *Init = FD->getInClassInitializer())
        walkExpr(Init);
    }
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
      addType(B.getType());
    closeOverFieldInits(R);
    if (auto *CTD = R->getDescribedClassTemplate())
      enqueue(CTD);
    if (auto *Spec = dyn_cast<ClassTemplateSpecializationDecl>(R))
      if (auto *Primary = Spec->getSpecializedTemplate()) {
        enqueue(Primary);
        closeOverTemplateArgs(Spec->getTemplateArgs());
      }
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
    addType(F->getReturnType());
    for (const ParmVarDecl *P : F->parameters()) {
      addType(P->getType());
      if (P->hasDefaultArg() && !P->hasUninstantiatedDefaultArg()) {
        if (const Expr *DE = P->getDefaultArg())
          walkExpr(DE);
      }
    }
    if (const Stmt *Body = F->getBody())
      walkStmt(Body);
    if (auto *FTD = F->getDescribedFunctionTemplate())
      enqueue(FTD);
    if (auto *Primary = F->getPrimaryTemplate())
      enqueue(Primary);
    if (const TemplateArgumentList *Args = F->getTemplateSpecializationArgs())
      closeOverTemplateArgs(*Args);
  }

  // Plan §3.3.2 step 4.
  void closeVar(const VarDecl *V) {
    if (!V)
      return;
    addType(V->getType());
    if (V->hasInit())
      walkExpr(V->getInit());
  }

  // Plan §3.3.2 step 5.
  void closeTypedef(const TypedefNameDecl *T) {
    if (!T)
      return;
    addType(T->getUnderlyingType());
  }

  // Plan §3.3.2 step 6.
  void closeUsing(const UsingDecl *U) {
    if (!U)
      return;
    for (auto It = U->shadow_begin(); It != U->shadow_end(); ++It)
      enqueue(*It);
  }

  void closeClassTemplate(const ClassTemplateDecl *CTD) {
    if (!CTD)
      return;
    enqueue(CTD->getTemplatedDecl());
    closeTemplateParameterDefaults(CTD->getTemplateParameters());
  }

  void closeFunctionTemplate(const FunctionTemplateDecl *FTD) {
    if (!FTD)
      return;
    enqueue(FTD->getTemplatedDecl());
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
        else if (const auto *CTD = dyn_cast<ClassTemplateDecl>(D))
          closeClassTemplate(CTD);
        else if (const auto *FTD = dyn_cast<FunctionTemplateDecl>(D))
          closeFunctionTemplate(FTD);
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
