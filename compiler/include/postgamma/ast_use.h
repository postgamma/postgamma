/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_COMPILER_AST_USE_H
#define POSTGAMMA_COMPILER_AST_USE_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include <string>

namespace postgamma::compiler {

using clang::ASTContext;
using clang::BinaryOperator;
using clang::DeclRefExpr;
using clang::DynTypedNode;
using clang::Expr;
using clang::ExprWithCleanups;
using clang::ImplicitCastExpr;
using clang::MaterializeTemporaryExpr;
using clang::ParenExpr;
using clang::UnaryOperator;
using clang::UO_AddrOf;
using llvm::isa;

inline const Expr *stripTransparentParents(const Expr *expression, ASTContext &context,
                                    DynTypedNode &node) {
  const Expr *current = expression;
  node = DynTypedNode::create(*expression);
  for (;;) {
    auto parents = context.getParents(node);
    if (parents.size() != 1)
      return current;
    const Expr *parent = parents[0].get<Expr>();
    if (!parent)
      return current;
    if (!isa<ParenExpr>(parent) && !isa<ImplicitCastExpr>(parent) &&
        !isa<ExprWithCleanups>(parent) && !isa<MaterializeTemporaryExpr>(parent))
      return current;
    current = parent;
    node = parents[0];
  }
}

inline std::string classifyUse(const DeclRefExpr *reference, ASTContext &context) {
  DynTypedNode node;
  const Expr *current = stripTransparentParents(reference, context, node);
  auto parents = context.getParents(node);
  if (parents.size() != 1)
    return "read";

  if (const auto *unary = parents[0].get<UnaryOperator>()) {
    if (unary->getOpcode() == UO_AddrOf)
      return "address";
    if (unary->isIncrementDecrementOp())
      return "read_write";
  }
  if (const auto *binary = parents[0].get<BinaryOperator>()) {
    if (binary->getLHS()->IgnoreParenImpCasts() == reference ||
        binary->getLHS()->IgnoreParenImpCasts() == current->IgnoreParenImpCasts()) {
      if (binary->isCompoundAssignmentOp())
        return "read_write";
      if (binary->isAssignmentOp())
        return "write";
    }
  }
  return "read";
}

} // namespace postgamma::compiler

#endif
