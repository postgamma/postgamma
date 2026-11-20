/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_COMPILER_SOURCE_IDENTITY_H
#define POSTGAMMA_COMPILER_SOURCE_IDENTITY_H

#include "clang/AST/DeclCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/SmallString.h"
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace postgamma::compiler {

namespace fs = std::filesystem;
using clang::DeclContext;
using clang::FunctionDecl;
using clang::CXXMethodDecl;
using clang::SourceLocation;
using clang::SourceManager;
using clang::VarDecl;
using llvm::dyn_cast;

struct RootedPath {
  std::string kind;
  std::string path;
};

inline std::string normalizeAbsolutePath(llvm::StringRef value) {
  if (value.empty())
    return {};
  std::error_code error;
  fs::path path(value.str());
  if (!path.is_absolute())
    path = fs::absolute(path, error);
  if (error)
    return {};
  path = path.lexically_normal();
  fs::path canonical = fs::weakly_canonical(path, error);
  if (!error)
    path = std::move(canonical);
  return path.generic_string();
}

inline std::optional<std::string> relativeToRoot(llvm::StringRef absolute,
                                          llvm::StringRef root) {
  fs::path path(absolute.str());
  fs::path base(root.str());
  auto path_it = path.begin();
  auto base_it = base.begin();
  while (path_it != path.end() && base_it != base.end() && *path_it == *base_it) {
    ++path_it;
    ++base_it;
  }
  if (base_it != base.end())
    return std::nullopt;
  fs::path relative;
  for (; path_it != path.end(); ++path_it)
    relative /= *path_it;
  if (relative.empty())
    return std::nullopt;
  return relative.generic_string();
}

inline std::optional<RootedPath> locateInRoots(llvm::StringRef absolute,
                                        llvm::StringRef source_root,
                                        llvm::StringRef generated_root) {
  if (auto relative = relativeToRoot(absolute, source_root))
    return RootedPath{"source", std::move(*relative)};
  if (!generated_root.empty()) {
    if (auto relative = relativeToRoot(absolute, generated_root))
      return RootedPath{"generated_build", std::move(*relative)};
  }
  return std::nullopt;
}

inline std::string identityPath(const RootedPath &location) {
  if (location.kind == "source")
    return location.path;
  return "@generated/" + location.path;
}

inline std::string locationPath(const SourceManager &source_manager,
                         SourceLocation location) {
  if (location.isInvalid())
    return {};
  return normalizeAbsolutePath(source_manager.getFilename(location));
}

inline std::optional<RootedPath> mainTranslationUnit(
    const SourceManager &source_manager, llvm::StringRef source_root,
    llvm::StringRef generated_root) {
  SourceLocation start =
      source_manager.getLocForStartOfFile(source_manager.getMainFileID());
  return locateInRoots(locationPath(source_manager, start), source_root,
                       generated_root);
}

inline std::string declarationContext(const VarDecl *declaration) {
  const DeclContext *context = declaration->getDeclContext();
  if (const auto *function = dyn_cast<FunctionDecl>(context))
    return function->getQualifiedNameAsString();
  if (const auto *method = dyn_cast<CXXMethodDecl>(context))
    return method->getQualifiedNameAsString();
  return {};
}

inline std::string stableIdentity(const VarDecl *declaration,
                           const SourceManager &source_manager,
                           llvm::StringRef source_root,
                           llvm::StringRef generated_root) {
  std::string qualified = declaration->getQualifiedNameAsString();
  if (qualified.empty())
    qualified = declaration->getNameAsString();
  if (declaration->isExternallyVisible() && !declaration->isLocalVarDecl())
    return "external:" + qualified;

  const VarDecl *definition = declaration->getDefinition();
  if (definition == nullptr)
    definition = declaration;
  SourceLocation location =
      source_manager.getSpellingLoc(definition->getLocation());
  auto definition_location = locateInRoots(
      locationPath(source_manager, location), source_root, generated_root);
  if (!definition_location)
    return {};
  auto translation_unit =
      mainTranslationUnit(source_manager, source_root, generated_root);
  if (!translation_unit)
    return {};
  std::string context = declarationContext(definition);
  std::string kind = definition->isLocalVarDecl() ? "function_static" : "internal";
  return kind + ":" + identityPath(*translation_unit) + ":" +
         identityPath(*definition_location) + ":" + context + ":" + qualified;
}

inline std::string declarationUSR(const VarDecl *declaration) {
  llvm::SmallString<128> usr;
  if (clang::index::generateUSRForDecl(declaration->getCanonicalDecl(), usr))
    return {};
  return usr.str().str();
}

} // namespace postgamma::compiler

#endif
