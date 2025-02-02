//--------------------------------------------------------------------*- C++ -*-
// clad - the C++ Clang-based Automatic Differentiator
// version: $Id: ClangPlugin.cpp 7 2013-06-01 22:48:03Z v.g.vassilev@gmail.com $
// author:  Vassil Vassilev <vvasilev-at-cern.ch>
//------------------------------------------------------------------------------

#include "clad/Differentiator/VisitorBase.h"

#include "ConstantFolder.h"

#include "clad/Differentiator/DiffPlanner.h"
#include "clad/Differentiator/ErrorEstimator.h"
#include "clad/Differentiator/StmtClone.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/TemplateBase.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Overload.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaInternal.h"
#include "clang/Sema/Template.h"

#include <algorithm>
#include <numeric>

#include "clad/Differentiator/Compatibility.h"

using namespace clang;

namespace clad {
  clang::CompoundStmt* VisitorBase::MakeCompoundStmt(const Stmts& Stmts) {
    auto Stmts_ref = llvm::makeArrayRef(Stmts.data(), Stmts.size());
    return clad_compat::CompoundStmt_Create(m_Context, Stmts_ref, noLoc, noLoc);
  }

  DiffParamsWithIndices VisitorBase::parseDiffArgs(const Expr* diffArgs,
                                                   const FunctionDecl* FD) {
    DiffParams params{};
    auto E = diffArgs->IgnoreParenImpCasts();
    // Case 1)
    if (auto SL = dyn_cast<StringLiteral>(E)) {
      IndexIntervalTable indexes{};
      llvm::StringRef string = SL->getString().trim();
      if (string.empty()) {
        diag(DiagnosticsEngine::Error,
             diffArgs->getEndLoc(),
             "No parameters were provided");
        return {};
      }
      // Split the string by ',' characters, trim whitespaces.
      llvm::SmallVector<llvm::StringRef, 16> names{};
      llvm::StringRef name{};
      do {
        std::tie(name, string) = string.split(',');
        names.push_back(name.trim());
      } while (!string.empty());
      // Stores parameters and field declarations to be used as candidates for
      // independent arguments.
      // If we are differentiating a call operator that have no parameters, 
      // then candidates for independent argument are member variables of the 
      // class that defines the call operator.
      // Otherwise, candidates are parameters of the function that we are
      // differentiating.
      llvm::SmallVector<std::pair<llvm::StringRef, ValueDecl*>, 16>
          candidates{};

      // find and store candidate parameters.
      if (FD->param_empty() && m_Functor) {
        for (FieldDecl* fieldDecl : m_Functor->fields())
          candidates.emplace_back(fieldDecl->getName(), fieldDecl);

      } else {
        for (auto PVD : FD->parameters())
          candidates.emplace_back(PVD->getName(), PVD);
      }

      for (const auto& name : names) {
        size_t loc = name.find('[');
        loc = (loc == llvm::StringRef::npos) ? name.size() : loc;
        llvm::StringRef base = name.substr(0, loc);

        auto it = std::find_if(
            std::begin(candidates),
            std::end(candidates),
            [&base](const std::pair<llvm::StringRef, ValueDecl*>& p) {
              return p.first == base;
            });

        if (it == std::end(candidates)) {
          // Fail if the function has no parameter with specified name.
          diag(DiagnosticsEngine::Error,
               diffArgs->getEndLoc(),
               "Requested parameter name '%0' was not found among function "
               "parameters",
               {base});
          return {};
        }

        auto f_it = std::find(std::begin(params), std::end(params), it->second);

        if (f_it != params.end()) {
          diag(DiagnosticsEngine::Error,
               diffArgs->getEndLoc(),
               "Requested parameter '%0' was specified multiple times",
               {it->second->getName()});
          return {};
        }

        params.push_back(it->second);

        if (loc != name.size()) {
          llvm::StringRef interval(
              name.slice(loc + 1, name.find(']')));
          llvm::StringRef firstStr, lastStr;
          std::tie(firstStr, lastStr) = interval.split(':');

          if (lastStr.empty()) {
            // The string is not a range just a single index
            size_t index;
            firstStr.getAsInteger(10, index);
            indexes.push_back(IndexInterval(index));
          } else {
            size_t first, last;
            firstStr.getAsInteger(10, first);
            lastStr.getAsInteger(10, last);
            if (first >= last) {
              diag(DiagnosticsEngine::Error,
                   diffArgs->getEndLoc(),
                   "Range specified in '%0' is in incorrect format",
                   {name});
              return {};
            }
            indexes.push_back(IndexInterval(first, last));
          }
        } else {
          indexes.push_back(IndexInterval());
        }
      }
      // Return a sequence of function's parameters.
      return {params, indexes};
    }
    // Case 2)
    // Check if the provided literal can be evaluated as an integral value.
    llvm::APSInt intValue;
    if (clad_compat::Expr_EvaluateAsInt(E, intValue, m_Context)) {
      auto idx = intValue.getExtValue();
      // If we are differentiating a call operator that have no parameters, then
      // we need to search for independent parameters in fields of the
      // class that defines the call operator instead.
      if (FD->param_empty() && m_Functor) {
        size_t totalFields = std::distance(m_Functor->field_begin(),
                                           m_Functor->field_end());
        // Fail if the specified index is invalid.
        if ((idx < 0) || idx >= totalFields) {
          diag(DiagnosticsEngine::Error, diffArgs->getEndLoc(),
               "Invalid member variable index '%0' of '%1' member variable(s)",
               {std::to_string(idx), std::to_string(totalFields)});
          return {};
        }
        params.push_back(*std::next(m_Functor->field_begin(), idx));
      } else {
        // Fail if the specified index is invalid.
        if ((idx < 0) || (idx >= FD->getNumParams())) {
          diag(DiagnosticsEngine::Error,
              diffArgs->getEndLoc(),
              "Invalid argument index '%0' of '%1' argument(s)",
              {std::to_string(idx), std::to_string(FD->getNumParams())});
          return {};
        }
        params.push_back(FD->getParamDecl(idx));
      }
      // Returns a single parameter.
      return {params, {}};
    }
    // Case 3)
    // Treat the default (unspecified) argument as a special case, as if all
    // function's arguments were requested.
    if (isa<CXXDefaultArgExpr>(E)) {
      std::copy(FD->param_begin(), FD->param_end(), std::back_inserter(params));
      // If the function has no parameters, then we cannot differentiate it."
      if (params.empty())
        diag(DiagnosticsEngine::Error,
             diffArgs->getEndLoc(),
             "Attempted to differentiate a function without parameters");
      // Returns the sequence with all the function's parameters.
      return {params, {}};
    }
    // Fail if the argument is not a string or numeric literal.
    diag(DiagnosticsEngine::Error,
         diffArgs->getEndLoc(),
         "Failed to parse the parameters, must be a string or numeric literal");
    return {{}, {}};
  }

  bool VisitorBase::isUnusedResult(const Expr* E) {
    const Expr* ignoreExpr;
    SourceLocation ignoreLoc;
    SourceRange ignoreRange;
    return E->isUnusedResultAWarning(
        ignoreExpr, ignoreLoc, ignoreRange, ignoreRange, m_Context);
  }

  bool VisitorBase::addToBlock(Stmt* S, Stmts& block) {
    if (!S)
      return false;
    if (Expr* E = dyn_cast<Expr>(S)) {
      if (isUnusedResult(E))
        return false;
    }
    block.push_back(S);
    return true;
  }

  bool VisitorBase::addToCurrentBlock(Stmt* S) {
    return addToBlock(S, getCurrentBlock());
  }

  VarDecl* VisitorBase::BuildVarDecl(QualType Type, IdentifierInfo* Identifier,
                                     Expr* Init, bool DirectInit,
                                     TypeSourceInfo* TSI,
                                     VarDecl::InitializationStyle IS) {

    auto VD =
        VarDecl::Create(m_Context, m_Sema.CurContext, m_Function->getLocation(),
                        m_Function->getLocation(), Identifier, Type, TSI,
                        SC_None);

    if (Init) {
      m_Sema.AddInitializerToDecl(VD, Init, DirectInit);
      VD->setInitStyle(IS);
    }
    // Add the identifier to the scope and IdResolver
    m_Sema.PushOnScopeChains(VD, getCurrentScope(), /*AddToContext*/ false);
    return VD;
  }

  VarDecl* VisitorBase::BuildVarDecl(QualType Type, llvm::StringRef prefix,
                                     Expr* Init, bool DirectInit,
                                     TypeSourceInfo* TSI,
                                     VarDecl::InitializationStyle IS) {
    return BuildVarDecl(Type, CreateUniqueIdentifier(prefix), Init, DirectInit,
                        TSI, IS);
  }

  NamespaceDecl* VisitorBase::BuildNamespaceDecl(IdentifierInfo* II,
                                                 bool isInline) {
    // Check if the namespace is being redeclared.
    NamespaceDecl* PrevNS = nullptr;
    // From Sema::ActOnStartNamespaceDef:
    if (II) {
      LookupResult R(m_Sema,
                     II,
                     noLoc,
                     Sema::LookupOrdinaryName,
                     clad_compat::Sema_ForVisibleRedeclaration);
      m_Sema.LookupQualifiedName(R, m_Sema.CurContext->getRedeclContext());
      NamedDecl* FoundDecl =
          R.isSingleResult() ? R.getRepresentativeDecl() : nullptr;
      PrevNS = dyn_cast_or_null<NamespaceDecl>(FoundDecl);
    } else {
      // Is anonymous namespace.
      DeclContext* Parent = m_Sema.CurContext->getRedeclContext();
      if (TranslationUnitDecl* TU = dyn_cast<TranslationUnitDecl>(Parent)) {
        PrevNS = TU->getAnonymousNamespace();
      } else {
        NamespaceDecl* ND = cast<NamespaceDecl>(Parent);
        PrevNS = ND->getAnonymousNamespace();
      }
    }
    NamespaceDecl* NDecl = NamespaceDecl::Create(
        m_Context, m_Sema.CurContext, isInline, noLoc, noLoc, II, PrevNS);
    if (II)
      m_Sema.PushOnScopeChains(NDecl, m_CurScope);
    else {
      // Link the anonymous namespace into its parent.
      // From Sema::ActOnStartNamespaceDef:
      DeclContext* Parent = m_Sema.CurContext->getRedeclContext();
      if (TranslationUnitDecl* TU = dyn_cast<TranslationUnitDecl>(Parent)) {
        TU->setAnonymousNamespace(NDecl);
      } else {
        cast<NamespaceDecl>(Parent)->setAnonymousNamespace(NDecl);
      }
      m_Sema.CurContext->addDecl(NDecl);
      if (!PrevNS) {
        UsingDirectiveDecl* UD =
            UsingDirectiveDecl::Create(m_Context,
                                       Parent,
                                       noLoc,
                                       noLoc,
                                       NestedNameSpecifierLoc(),
                                       noLoc,
                                       NDecl,
                                       Parent);
        UD->setImplicit();
        Parent->addDecl(UD);
      }
    }
    // Namespace scope and declcontext. Must be exited by the user.
    beginScope(Scope::DeclScope);
    m_Sema.PushDeclContext(m_CurScope, NDecl);
    return NDecl;
  }

  NamespaceDecl* VisitorBase::RebuildEnclosingNamespaces(DeclContext* DC) {
    if (NamespaceDecl* ND = dyn_cast_or_null<NamespaceDecl>(DC)) {
      NamespaceDecl* Head = RebuildEnclosingNamespaces(ND->getDeclContext());
      NamespaceDecl* NewD =
          BuildNamespaceDecl(ND->getIdentifier(), ND->isInline());
      return Head ? Head : NewD;
    } else {
      m_Sema.CurContext = DC;
      return nullptr;
    }
  }

  DeclStmt* VisitorBase::BuildDeclStmt(Decl* D) {
    Stmt* DS =
        m_Sema.ActOnDeclStmt(m_Sema.ConvertDeclToDeclGroup(D), noLoc, noLoc)
            .get();
    return cast<DeclStmt>(DS);
  }

  DeclStmt* VisitorBase::BuildDeclStmt(llvm::MutableArrayRef<Decl*> Decls) {
    auto DGR = DeclGroupRef::Create(m_Context, Decls.data(), Decls.size());
    return new (m_Context) DeclStmt(DGR, noLoc, noLoc);
  }

  DeclRefExpr* VisitorBase::BuildDeclRef(DeclaratorDecl* D) {
    QualType T = D->getType();
    T = T.getNonReferenceType();
    return cast<DeclRefExpr>(clad_compat::GetResult<Expr*>(
        m_Sema.BuildDeclRefExpr(D, T, VK_LValue, noLoc)));
  }

  IdentifierInfo*
  VisitorBase::CreateUniqueIdentifier(llvm::StringRef nameBase) {
    // For intermediate variables, use numbered names (_t0), for everything
    // else first try a name without number (e.g. first try to use _d_x and
    // use _d_x0 only if _d_x is taken).
    bool countedName = nameBase.startswith("_") &&
                       !nameBase.startswith("_d_") &&
                       !nameBase.startswith("_delta_");
    std::size_t idx = 0;
    std::size_t& id = countedName ? m_idCtr[nameBase.str()] : idx;
    std::string idStr = countedName ? std::to_string(id) : "";
    if (countedName)
      id += 1;
    for (;;) {
      IdentifierInfo* name = &m_Context.Idents.get(nameBase.str() + idStr);
      LookupResult R(
          m_Sema, DeclarationName(name), noLoc, Sema::LookupOrdinaryName);
      m_Sema.LookupName(R, m_CurScope, /*AllowBuiltinCreation*/ false);
      if (R.empty()) {
        return name;
      } else {
        idStr = std::to_string(id);
        id += 1;
      }
    }
  }

  Expr* VisitorBase::BuildParens(Expr* E) {
    if (!E)
      return nullptr;
    Expr* ENoCasts = E->IgnoreCasts();
    // In our case, there is no reason to build parentheses around something
    // that is not a binary or ternary operator.
    if (isa<BinaryOperator>(ENoCasts) ||
        (isa<CXXOperatorCallExpr>(ENoCasts) &&
         cast<CXXOperatorCallExpr>(ENoCasts)->getNumArgs() == 2) ||
        isa<ConditionalOperator>(ENoCasts))
      return m_Sema.ActOnParenExpr(noLoc, noLoc, E).get();
    else
      return E;
  }

  Expr* VisitorBase::StoreAndRef(Expr* E, llvm::StringRef prefix,
                                 bool forceDeclCreation,
                                 VarDecl::InitializationStyle IS) {
    return StoreAndRef(E, getCurrentBlock(), prefix, forceDeclCreation, IS);
  }
  Expr* VisitorBase::StoreAndRef(Expr* E, Stmts& block, llvm::StringRef prefix,
                                 bool forceDeclCreation,
                                 VarDecl::InitializationStyle IS) {
    assert(E && "cannot infer type from null expression");
    QualType Type = E->getType();
    if (E->isModifiableLvalue(m_Context) == Expr::MLV_Valid)
      Type = m_Context.getLValueReferenceType(Type);
    return StoreAndRef(E, Type, block, prefix, forceDeclCreation, IS);
  }

  /// For an expr E, decides if it is useful to store it in a temporary variable
  /// and replace E's further usage by a reference to that variable to avoid
  /// recomputiation.
  static bool UsefulToStore(Expr* E) {
    if (!E)
      return false;
    Expr* B = E->IgnoreParenImpCasts();
    // FIXME: find a more general way to determine that or add more options.
    if (isa<DeclRefExpr>(B) || isa<FloatingLiteral>(B) ||
        isa<IntegerLiteral>(B))
      return false;
    if (isa<UnaryOperator>(B)) {
      auto UO = cast<UnaryOperator>(B);
      auto OpKind = UO->getOpcode();
      if (OpKind == UO_Plus || OpKind == UO_Minus)
        return UsefulToStore(UO->getSubExpr());
      return false;
    }
    if (isa<ArraySubscriptExpr>(B)) {
      auto ASE = cast<ArraySubscriptExpr>(B);
      return UsefulToStore(ASE->getBase()) || UsefulToStore(ASE->getIdx());
    }
    return true;
  }

  Expr* VisitorBase::StoreAndRef(Expr* E, QualType Type, Stmts& block,
                                 llvm::StringRef prefix, bool forceDeclCreation,
                                 VarDecl::InitializationStyle IS) {
    if (!forceDeclCreation) {
      // If Expr is simple (i.e. a reference or a literal), there is no point
      // in storing it as there is no evaluation going on.
      if (!UsefulToStore(E))
        return E;
    }
    // Create variable declaration.
    VarDecl* Var = BuildVarDecl(Type, CreateUniqueIdentifier(prefix), E,
                                /*DirectInit=*/false,
                                /*TSI=*/nullptr, IS);

    // Add the declaration to the body of the gradient function.
    addToBlock(BuildDeclStmt(Var), block);

    // Return reference to the declaration instead of original expression.
    return BuildDeclRef(Var);
  }

  Stmt* VisitorBase::Clone(const Stmt* S) {
    Stmt* clonedStmt = m_Builder.m_NodeCloner->Clone(S);
    updateReferencesOf(clonedStmt);
    return clonedStmt;
  }
  Expr* VisitorBase::Clone(const Expr* E) {
    const Stmt* S = E;
    return llvm::cast<Expr>(Clone(S));
  }

  Expr* VisitorBase::BuildOp(UnaryOperatorKind OpCode, Expr* E,
                             SourceLocation OpLoc) {
    if (!E)
      return nullptr;
    return m_Sema.BuildUnaryOp(nullptr, OpLoc, OpCode, E).get();
  }

  Expr* VisitorBase::BuildOp(clang::BinaryOperatorKind OpCode, Expr* L, Expr* R,
                             SourceLocation OpLoc) {
    if (!L || !R)
      return nullptr;
    return m_Sema.BuildBinOp(nullptr, OpLoc, OpCode, L, R).get();
  }

  Expr* VisitorBase::getZeroInit(QualType T) {
    if (T->isScalarType())
      return ConstantFolder::synthesizeLiteral(m_Context.IntTy, m_Context, 0);
    else
      return m_Sema.ActOnInitList(noLoc, {}, noLoc).get();
  }

  std::pair<const clang::Expr*, llvm::SmallVector<const clang::Expr*, 4>>
  VisitorBase::SplitArraySubscript(const Expr* ASE) {
    llvm::SmallVector<const clang::Expr*, 4> Indices{};
    const Expr* E = ASE->IgnoreParenImpCasts();
    while (auto S = dyn_cast<ArraySubscriptExpr>(E)) {
      Indices.push_back(S->getIdx());
      E = S->getBase()->IgnoreParenImpCasts();
    }
    std::reverse(std::begin(Indices), std::end(Indices));
    return std::make_pair(E, std::move(Indices));
  }

  Expr* VisitorBase::BuildArraySubscript(
      Expr* Base, const llvm::SmallVectorImpl<clang::Expr*>& Indices) {
    Expr* result = Base;
    for (Expr* I : Indices)
      result =
          m_Sema.CreateBuiltinArraySubscriptExpr(result, noLoc, I, noLoc).get();
    return result;
  }

  NamespaceDecl* VisitorBase::GetCladNamespace() {
    static NamespaceDecl* Result = nullptr;
    if (Result)
      return Result;
    DeclarationName CladName = &m_Context.Idents.get("clad");
    LookupResult CladR(m_Sema,
                       CladName,
                       noLoc,
                       Sema::LookupNamespaceName,
                       clad_compat::Sema_ForVisibleRedeclaration);
    m_Sema.LookupQualifiedName(CladR, m_Context.getTranslationUnitDecl());
    assert(!CladR.empty() && "cannot find clad namespace");
    Result = cast<NamespaceDecl>(CladR.getFoundDecl());
    return Result;
  }

  TemplateDecl* VisitorBase::GetCladClassDecl(llvm::StringRef ClassName) {
    NamespaceDecl* CladNS = GetCladNamespace();
    CXXScopeSpec CSS;
    CSS.Extend(m_Context, CladNS, noLoc, noLoc);
    DeclarationName TapeName = &m_Context.Idents.get(ClassName);
    LookupResult TapeR(m_Sema,
                       TapeName,
                       noLoc,
                       Sema::LookupUsingDeclName,
                       clad_compat::Sema_ForVisibleRedeclaration);
    m_Sema.LookupQualifiedName(TapeR, CladNS, CSS);
    assert(!TapeR.empty() && isa<TemplateDecl>(TapeR.getFoundDecl()) &&
           "cannot find clad::tape");
    return cast<TemplateDecl>(TapeR.getFoundDecl());
  }

  QualType
  VisitorBase::GetCladClassOfType(TemplateDecl* CladClassDecl,
                                  MutableArrayRef<QualType> TemplateArgs) {
    // Create a list of template arguments.
    TemplateArgumentListInfo TLI{};
    for (auto T : TemplateArgs) {
      TemplateArgument TA = T;
      TLI.addArgument(
          TemplateArgumentLoc(TA, m_Context.getTrivialTypeSourceInfo(T)));
    }
    // This will instantiate tape<T> type and return it.
    QualType TT =
        m_Sema.CheckTemplateIdType(TemplateName(CladClassDecl), noLoc, TLI);
    // Get clad namespace and its identifier clad::.
    CXXScopeSpec CSS;
    CSS.Extend(m_Context, GetCladNamespace(), noLoc, noLoc);
    NestedNameSpecifier* NS = CSS.getScopeRep();

    // Create elaborated type with namespace specifier,
    // i.e. class<T> -> clad::class<T>
    return m_Context.getElaboratedType(ETK_None, NS, TT);
  }

  TemplateDecl* VisitorBase::GetCladTapeDecl() {
    static TemplateDecl* Result = nullptr;
    if (!Result)
      Result = GetCladClassDecl(/*ClassName=*/"tape");
    return Result;
  }

  LookupResult VisitorBase::LookupCladTapeMethod(llvm::StringRef name) {
    NamespaceDecl* CladNS = GetCladNamespace();
    CXXScopeSpec CSS;
    CSS.Extend(m_Context, CladNS, noLoc, noLoc);
    DeclarationName Name = &m_Context.Idents.get(name);
    LookupResult R(m_Sema, Name, noLoc, Sema::LookupOrdinaryName);
    m_Sema.LookupQualifiedName(R, CladNS, CSS);
    assert(!R.empty() && isa<FunctionTemplateDecl>(R.getRepresentativeDecl()) &&
           "cannot find requested name");
    return R;
  }

  LookupResult& VisitorBase::GetCladTapePush() {
    static llvm::Optional<LookupResult> Result{};
    if (Result)
      return Result.getValue();
    Result = LookupCladTapeMethod("push");
    return Result.getValue();
  }

  LookupResult& VisitorBase::GetCladTapePop() {
    static llvm::Optional<LookupResult> Result{};
    if (Result)
      return Result.getValue();
    Result = LookupCladTapeMethod("pop");
    return Result.getValue();
  }

  LookupResult& VisitorBase::GetCladTapeBack() {
    static llvm::Optional<LookupResult> Result{};
    if (Result)
      return Result.getValue();
    Result = LookupCladTapeMethod("back");
    return Result.getValue();
  }

  QualType VisitorBase::GetCladTapeOfType(QualType T) {
    return GetCladClassOfType(GetCladTapeDecl(), {T});
  }

  Expr* VisitorBase::BuildCallExprToMemFn(Expr* Base, bool isArrow,
                                          StringRef MemberFunctionName,
                                          MutableArrayRef<Expr*> ArgExprs) {
    UnqualifiedId Member;
    Member.setIdentifier(&m_Context.Idents.get(MemberFunctionName), noLoc);
    CXXScopeSpec SS;
    auto ME = m_Sema
                  .ActOnMemberAccessExpr(getCurrentScope(), Base, noLoc,
                                         isArrow ? tok::TokenKind::arrow
                                                 : tok::TokenKind::period,
                                         SS, noLoc, Member,
                                         /*ObjCImpDecl=*/nullptr)
                  .get();
    return m_Sema.ActOnCallExpr(getCurrentScope(), ME, noLoc, ArgExprs, noLoc)
        .get();
  }

  static QualType getRefQualifiedThisType(ASTContext& C, CXXMethodDecl* MD) {
    CXXRecordDecl* RD = MD->getParent();
    auto RDType = RD->getTypeForDecl();
    auto thisObjectQType = C.getQualifiedType(
        RDType, clad_compat::CXXMethodDecl_getMethodQualifiers(MD));        
    if (MD->getRefQualifier() == RefQualifierKind::RQ_LValue)
      thisObjectQType = C.getLValueReferenceType(thisObjectQType);
    else if (MD->getRefQualifier() == RefQualifierKind::RQ_RValue)
      thisObjectQType = C.getRValueReferenceType(thisObjectQType);
    return thisObjectQType;
  }

  Expr* VisitorBase::BuildCallExprToMemFn(
      clang::CXXMethodDecl* FD, llvm::MutableArrayRef<clang::Expr*> argExprs,
      bool useRefQualifiedThisObj) {
    Expr* thisExpr = clad_compat::Sema_BuildCXXThisExpr(m_Sema, FD);
    bool isArrow = true;

    if (useRefQualifiedThisObj) {
      auto thisQType = getRefQualifiedThisType(m_Context, FD);
      // Build `static_cast<ReferenceQualifiedThisObjectType>(*this)`
      // expression.
      thisExpr = m_Sema
                     .BuildCXXNamedCast(noLoc, tok::TokenKind::kw_static_cast,
                                        m_Context.getTrivialTypeSourceInfo(
                                            thisQType),
                                        BuildOp(UnaryOperatorKind::UO_Deref,
                                                thisExpr),
                                        noLoc, noLoc)
                     .get();
      isArrow = false;
    }
    NestedNameSpecifierLoc NNS(FD->getQualifier(),
                               /*Data=*/nullptr);
    auto DAP = DeclAccessPair::make(FD, FD->getAccess());
    auto memberExpr = MemberExpr::
        Create(m_Context, thisExpr, isArrow, noLoc, NNS, noLoc, FD, DAP,
               FD->getNameInfo(),
               /*TemplateArgs=*/nullptr, m_Context.BoundMemberTy,
               CLAD_COMPAT_ExprValueKind_R_or_PR_Value,
               ExprObjectKind::OK_Ordinary
                   CLAD_COMPAT_CLANG9_MemberExpr_ExtraParams(NOUR_None));
    return m_Sema
        .BuildCallToMemberFunction(getCurrentScope(), memberExpr, noLoc,
                                   argExprs, noLoc)
        .get();
  }

  Expr*
  VisitorBase::BuildCallExprToFunction(FunctionDecl* FD,
                                       llvm::MutableArrayRef<Expr*> argExprs,
                                       bool useRefQualifiedThisObj) {
    Expr* call = nullptr;
    if (auto derMethod = dyn_cast<CXXMethodDecl>(FD)) {
      call = BuildCallExprToMemFn(derMethod, argExprs, useRefQualifiedThisObj);
    } else {
      Expr* exprFunc = BuildDeclRef(FD);
      call = m_Sema
                 .ActOnCallExpr(
                     getCurrentScope(),
                     /*Fn=*/exprFunc,
                     /*LParenLoc=*/noLoc,
                     /*ArgExprs=*/llvm::MutableArrayRef<Expr*>(argExprs),
                     /*RParenLoc=*/m_Function->getLocation())
                 .get();
    }
    return call;
  }

  TemplateDecl* VisitorBase::GetCladArrayRefDecl() {
    static TemplateDecl* Result = nullptr;
    if (!Result)
      Result = GetCladClassDecl(/*ClassName=*/"array_ref");
    return Result;
  }

  QualType VisitorBase::GetCladArrayRefOfType(clang::QualType T) {
    return GetCladClassOfType(GetCladArrayRefDecl(), {T});
  }

  TemplateDecl* VisitorBase::GetCladArrayDecl() {
    static TemplateDecl* Result = nullptr;
    if (!Result)
      Result = GetCladClassDecl(/*ClassName=*/"array");
    return Result;
  }

  QualType VisitorBase::GetCladArrayOfType(clang::QualType T) {
    return GetCladClassOfType(GetCladArrayDecl(), {T});
  }

  Expr* VisitorBase::BuildArrayRefSizeExpr(Expr* Base) {
    return BuildCallExprToMemFn(Base, /*isArrow=*/false,
                                /*MemberFunctionName=*/"size", {});
  }

  Expr* VisitorBase::BuildArrayRefSliceExpr(Expr* Base,
                                            MutableArrayRef<Expr*> Args) {
    return BuildCallExprToMemFn(Base, /*isArrow=*/false,
                                /*MemberFunctionName=*/"slice", Args);
  }

  bool VisitorBase::isArrayRefType(QualType QT) {
    return QT.getAsString().find("clad::array_ref") != std::string::npos;
  }

  Expr* VisitorBase::GetSingleArgCentralDiffCall(
      Expr* targetFuncCall, Expr* targetArg, unsigned targetPos,
      unsigned numArgs, llvm::SmallVectorImpl<Expr*>& args) {
    QualType argType = targetArg->getType();
    int printErrorInf = m_Builder.shouldPrintNumDiffErrs();
    bool isSupported = argType->isArithmeticType();
    if (!isSupported)
      return nullptr;
    IdentifierInfo* II = &m_Context.Idents.get("forward_central_difference");
    DeclarationName name(II);
    DeclarationNameInfo DNInfo(name, noLoc);
    // Build function args.
    llvm::SmallVector<Expr*, 16U> NumDiffArgs;
    NumDiffArgs.push_back(targetFuncCall);
    NumDiffArgs.push_back(targetArg);
    NumDiffArgs.push_back(ConstantFolder::synthesizeLiteral(m_Context.IntTy,
                                                            m_Context,
                                                            targetPos));
    NumDiffArgs.push_back(ConstantFolder::synthesizeLiteral(m_Context.IntTy,
                                                            m_Context,
                                                            printErrorInf));
    NumDiffArgs.insert(NumDiffArgs.end(), args.begin(), args.begin() + numArgs);
    // Return the found overload.
    return m_Builder.findOverloadedDefinition(DNInfo, NumDiffArgs,
                                              /*forCustomDerv=*/false,
                                              /*namespaceShouldExist=*/false);
  }

  Expr* VisitorBase::GetMultiArgCentralDiffCall(
      Expr* targetFuncCall, QualType retType, unsigned numArgs,
      llvm::SmallVectorImpl<Stmt*>& NumericalDiffMultiArg,
      llvm::SmallVectorImpl<Expr*>& args,
      llvm::SmallVectorImpl<Expr*>& outputArgs) {
    int printErrorInf = m_Builder.shouldPrintNumDiffErrs();
    IdentifierInfo* II = &m_Context.Idents.get("central_difference");
    DeclarationName name(II);
    DeclarationNameInfo DNInfo(name, noLoc);
    llvm::SmallVector<Expr*, 16U> NumDiffArgs = {};
    NumDiffArgs.push_back(targetFuncCall);
    // build the clad::tape<clad::array_ref>> = {};
    QualType RefType = GetCladArrayRefOfType(retType);
    QualType TapeType = GetCladTapeOfType(RefType);
    auto VD = BuildVarDecl(TapeType);
    NumericalDiffMultiArg.push_back(BuildDeclStmt(VD));
    Expr* TapeRef = BuildDeclRef(VD);
    NumDiffArgs.push_back(TapeRef);
    NumDiffArgs.push_back(ConstantFolder::synthesizeLiteral(m_Context.IntTy,
                                                            m_Context,
                                                            printErrorInf));

    // Build the tape push expressions.
    VD->setLocation(m_Function->getLocation());
    m_Sema.AddInitializerToDecl(VD, getZeroInit(TapeType), false);
    CXXScopeSpec CSS;
    CSS.Extend(m_Context, GetCladNamespace(), noLoc, noLoc);
    LookupResult& Push = GetCladTapePush();
    auto PushDRE = m_Sema.BuildDeclarationNameExpr(CSS, Push, /*ADL*/ false)
                       .get();
    Expr* PushExpr;
    for (unsigned i = 0, e = numArgs; i < e; i++) {
      Expr* callArgs[] = {TapeRef, outputArgs[i]};
      PushExpr = m_Sema
                     .ActOnCallExpr(getCurrentScope(), PushDRE, noLoc, callArgs,
                                    noLoc)
                     .get();
      NumericalDiffMultiArg.push_back(PushExpr);
      NumDiffArgs.push_back(args[i]);
    }

    return m_Builder.findOverloadedDefinition(DNInfo, NumDiffArgs,
                                              /*forCustomDerv=*/false,
                                              /*namespaceShouldExist=*/false);
  }

  void VisitorBase::CallExprDiffDiagnostics(llvm::StringRef funcName,
                                 SourceLocation srcLoc, bool isDerived){
    if (!isDerived) {
      // Function was not derived => issue a warning.
      diag(DiagnosticsEngine::Warning,
           srcLoc,
           "function '%0' was not differentiated because clad failed to "
           "differentiate it and no suitable overload was found in "
           "namespace 'custom_derivatives', and function may not be "
            "eligible for numerical differentiation.",
           {funcName});
    } else {
      diag(DiagnosticsEngine::Warning, noLoc,
           "Falling back to numerical differentiation for '%0' since no "
           "suitable overload was found and clad could not derive it. "
           "To disable this feature, compile your programs with "
           "-DCLAD_NO_NUM_DIFF.",
           {funcName});
    }
  }

} // end namespace clad
