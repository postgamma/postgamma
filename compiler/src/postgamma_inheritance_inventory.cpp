/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

//===- postgamma_inheritance_inventory.cpp - fork-state contract facts ----===//
//
// Discover mutable static-storage objects read by PostgreSQL's backend-state
// save function.  PostgreSQL maintains this function as the authoritative
// description of state that a fresh EXEC_BACKEND child must inherit.  The
// resulting catalog is an input to human-reviewed thread inheritance policy;
// this tool does not assign ownership or choose a copy strategy.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

namespace {

#ifndef POSTGAMMA_CLANG_RESOURCE_DIR
#define POSTGAMMA_CLANG_RESOURCE_DIR ""
#endif

llvm::cl::OptionCategory InventoryCategory(
    "PostGamma backend inheritance inventory options");
llvm::cl::opt<std::string> SourceRoot(
    "source-root", llvm::cl::desc("Immutable PostgreSQL source root"),
    llvm::cl::value_desc("path"), llvm::cl::Required,
    llvm::cl::cat(InventoryCategory));
llvm::cl::opt<std::string> GeneratedRoot(
    "generated-root",
    llvm::cl::desc("Configured PostgreSQL build root containing generated headers"),
    llvm::cl::value_desc("path"), llvm::cl::init(""),
    llvm::cl::cat(InventoryCategory));
llvm::cl::opt<std::string> ContractFunction(
    "contract-function",
    llvm::cl::desc("PostgreSQL function whose global reads define inheritance"),
    llvm::cl::value_desc("name"), llvm::cl::Required,
    llvm::cl::cat(InventoryCategory));
llvm::cl::opt<std::string> OutputPath(
    "output", llvm::cl::desc("Deterministic JSON output"),
    llvm::cl::value_desc("path"), llvm::cl::Required,
    llvm::cl::cat(InventoryCategory));
llvm::cl::opt<std::string> ClangResourceDir(
    "clang-resource-dir", llvm::cl::desc("Clang builtin-header resource directory"),
    llvm::cl::value_desc("path"), llvm::cl::init(POSTGAMMA_CLANG_RESOURCE_DIR),
    llvm::cl::cat(InventoryCategory));
llvm::cl::opt<bool> SuppressCompilerWarnings(
    "suppress-compiler-warnings",
    llvm::cl::desc("Suppress warnings inherited from a non-Clang compile database"),
    llvm::cl::init(true), llvm::cl::cat(InventoryCategory));

std::string normalizeAbsolutePath(llvm::StringRef value) {
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

std::optional<std::string> relativeToRoot(llvm::StringRef absolute,
                                          llvm::StringRef root) {
  fs::path path(absolute.str());
  fs::path base(root.str());
  auto path_it = path.begin();
  auto base_it = base.begin();
  while (path_it != path.end() && base_it != base.end() &&
         *path_it == *base_it) {
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

struct RootedPath {
  std::string kind;
  std::string path;
};

std::optional<RootedPath> locateInRoots(llvm::StringRef absolute,
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

std::string identityPath(const RootedPath &location) {
  if (location.kind == "source")
    return location.path;
  return "@generated/" + location.path;
}

std::string locationPath(const SourceManager &source_manager,
                         SourceLocation location) {
  if (location.isInvalid())
    return {};
  return normalizeAbsolutePath(source_manager.getFilename(location));
}

std::optional<RootedPath> mainTranslationUnit(
    const SourceManager &source_manager, llvm::StringRef source_root,
    llvm::StringRef generated_root) {
  SourceLocation start =
      source_manager.getLocForStartOfFile(source_manager.getMainFileID());
  return locateInRoots(locationPath(source_manager, start), source_root,
                       generated_root);
}

std::string declarationContext(const VarDecl *declaration) {
  const DeclContext *context = declaration->getDeclContext();
  if (const auto *function = dyn_cast<FunctionDecl>(context))
    return function->getQualifiedNameAsString();
  if (const auto *method = dyn_cast<CXXMethodDecl>(context))
    return method->getQualifiedNameAsString();
  return {};
}

std::string stableIdentity(const VarDecl *declaration,
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
  auto translation_unit =
      mainTranslationUnit(source_manager, source_root, generated_root);
  if (!definition_location || !translation_unit)
    return {};
  std::string context = declarationContext(definition);
  std::string kind = definition->isLocalVarDecl() ? "function_static" : "internal";
  return kind + ":" + identityPath(*translation_unit) + ":" +
         identityPath(*definition_location) + ":" + context + ":" + qualified;
}

std::string declarationUSR(const VarDecl *declaration) {
  llvm::SmallString<128> usr;
  if (clang::index::generateUSRForDecl(declaration->getCanonicalDecl(), usr))
    return {};
  return usr.str().str();
}

struct ReferenceLocation {
  std::string path;
  unsigned line;
  unsigned column;

  auto key() const { return std::tie(path, line, column); }
};

struct StateRecord {
  std::string id;
  std::string name;
  std::string usr;
  std::string canonical_type;
  std::set<std::tuple<std::string, unsigned, unsigned>> references;
};

class FunctionReferenceVisitor
    : public RecursiveASTVisitor<FunctionReferenceVisitor> {
 public:
  FunctionReferenceVisitor(ASTContext &context, llvm::StringRef source_root,
                           llvm::StringRef generated_root,
                           std::map<std::string, StateRecord> &records)
      : context_(context), source_manager_(context.getSourceManager()),
        source_root_(source_root.str()), generated_root_(generated_root.str()),
        records_(records) {}

  bool VisitDeclRefExpr(DeclRefExpr *reference) {
    const auto *declaration = dyn_cast<VarDecl>(reference->getDecl());
    if (declaration == nullptr || !declaration->hasGlobalStorage() ||
        declaration->getType().isConstQualified() ||
        declaration->getType()->isFunctionType())
      return true;

    std::string id = stableIdentity(declaration, source_manager_, source_root_,
                                    generated_root_);
    if (id.empty())
      return true;
    SourceLocation location =
        source_manager_.getSpellingLoc(reference->getLocation());
    auto rooted = locateInRoots(locationPath(source_manager_, location),
                                source_root_, generated_root_);
    if (!rooted)
      return true;
    PresumedLoc presumed = source_manager_.getPresumedLoc(location);
    if (!presumed.isValid())
      return true;

    QualType type = declaration->getType().getCanonicalType();
    PrintingPolicy policy(context_.getLangOpts());
    policy.AnonymousTagLocations = false;
    StateRecord candidate{id,
                          declaration->getNameAsString(),
                          declarationUSR(declaration),
                          type.getAsString(policy),
                          {}};
    auto [found, inserted] = records_.emplace(id, std::move(candidate));
    StateRecord &record = found->second;
    if (!inserted &&
        (record.usr != declarationUSR(declaration) ||
         record.canonical_type != type.getAsString(policy))) {
      llvm::errs() << "postgamma-inheritance-inventory: conflicting declarations for "
                   << id << "\n";
      conflict_ = true;
      return true;
    }
    record.references.emplace(identityPath(*rooted), presumed.getLine(),
                              presumed.getColumn());
    return true;
  }

  bool hasConflict() const { return conflict_; }

 private:
  ASTContext &context_;
  SourceManager &source_manager_;
  std::string source_root_;
  std::string generated_root_;
  std::map<std::string, StateRecord> &records_;
  bool conflict_ = false;
};

class ContractCollector : public MatchFinder::MatchCallback {
 public:
  ContractCollector(std::string source_root, std::string generated_root)
      : source_root_(std::move(source_root)),
        generated_root_(std::move(generated_root)) {}

  void run(const MatchFinder::MatchResult &result) override {
    const auto *function =
        result.Nodes.getNodeAs<FunctionDecl>("contract-function");
    if (function == nullptr || function->getBody() == nullptr ||
        result.Context == nullptr)
      return;
    matches_++;
    auto translation_unit = mainTranslationUnit(
        result.Context->getSourceManager(), source_root_, generated_root_);
    if (translation_unit)
      translation_units_.insert(identityPath(*translation_unit));
    FunctionReferenceVisitor visitor(*result.Context, source_root_,
                                     generated_root_, records_);
    visitor.TraverseStmt(const_cast<Stmt *>(function->getBody()));
    conflict_ |= visitor.hasConflict();
  }

  bool validate() const {
    if (matches_ != 1) {
      llvm::errs() << "postgamma-inheritance-inventory: expected exactly one definition of "
                   << ContractFunction << ", found " << matches_ << "\n";
      return false;
    }
    if (translation_units_.size() != 1) {
      llvm::errs() << "postgamma-inheritance-inventory: contract must belong to one translation unit\n";
      return false;
    }
    if (records_.empty()) {
      llvm::errs() << "postgamma-inheritance-inventory: contract reads no mutable global state\n";
      return false;
    }
    return !conflict_;
  }

  bool write(llvm::StringRef output) const {
    llvm::json::Array states;
    int64_t reference_count = 0;
    for (const auto &[id, record] : records_) {
      llvm::json::Array references;
      for (const auto &[path, line, column] : record.references) {
        references.push_back(llvm::json::Object{
            {"path", path},
            {"line", static_cast<int64_t>(line)},
            {"column", static_cast<int64_t>(column)}});
      }
      reference_count += static_cast<int64_t>(record.references.size());
      states.push_back(llvm::json::Object{
          {"id", id},
          {"name", record.name},
          {"usr", record.usr},
          {"canonical_type", record.canonical_type},
          {"reference_count", static_cast<int64_t>(record.references.size())},
          {"references", std::move(references)}});
    }
    llvm::json::Object document{
        {"schema_version", 1},
        {"kind", "postgamma.backend-inheritance-catalog"},
        {"contract", llvm::json::Object{
             {"function", ContractFunction.getValue()},
             {"translation_unit", *translation_units_.begin()}}},
        {"states", std::move(states)},
        {"summary", llvm::json::Object{
             {"state_count", static_cast<int64_t>(records_.size())},
             {"reference_count", reference_count}}}};

    std::error_code error;
    llvm::raw_fd_ostream stream(output, error);
    if (error) {
      llvm::errs() << "postgamma-inheritance-inventory: cannot write " << output
                   << ": " << error.message() << "\n";
      return false;
    }
    stream << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(document)));
    return true;
  }

 private:
  std::string source_root_;
  std::string generated_root_;
  unsigned matches_ = 0;
  bool conflict_ = false;
  std::set<std::string> translation_units_;
  std::map<std::string, StateRecord> records_;
};

class InventoryAction : public ASTFrontendAction {
 public:
  explicit InventoryAction(ContractCollector &collector) : collector_(collector) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &,
                                                 llvm::StringRef) override {
    finder_.addMatcher(functionDecl(isDefinition(),
                                    hasName(ContractFunction.getValue()))
                           .bind("contract-function"),
                       &collector_);
    return finder_.newASTConsumer();
  }

 private:
  ContractCollector &collector_;
  MatchFinder finder_;
};

class InventoryActionFactory : public FrontendActionFactory {
 public:
  explicit InventoryActionFactory(ContractCollector &collector)
      : collector_(collector) {}

  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<InventoryAction>(collector_);
  }

 private:
  ContractCollector &collector_;
};

}  // namespace

int main(int argc, const char **argv) {
  auto options = CommonOptionsParser::create(argc, argv, InventoryCategory);
  if (!options) {
    llvm::errs() << llvm::toString(options.takeError()) << "\n";
    return 2;
  }
  std::string source_root = normalizeAbsolutePath(SourceRoot);
  if (source_root.empty()) {
    llvm::errs() << "postgamma-inheritance-inventory: invalid source root\n";
    return 2;
  }
  std::string generated_root;
  if (!GeneratedRoot.empty()) {
    generated_root = normalizeAbsolutePath(GeneratedRoot);
    if (generated_root.empty()) {
      llvm::errs() << "postgamma-inheritance-inventory: invalid generated root\n";
      return 2;
    }
  }

  ContractCollector collector(source_root, generated_root);
  ClangTool tool(options->getCompilations(), options->getSourcePathList());
  if (!ClangResourceDir.empty()) {
    CommandLineArguments resource_arguments{"-resource-dir=" + ClangResourceDir};
    tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
        resource_arguments, ArgumentInsertPosition::BEGIN));
  }
  if (SuppressCompilerWarnings) {
    CommandLineArguments warning_arguments{
        "-Wno-error=implicit-function-declaration", "-w"};
    tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
        warning_arguments, ArgumentInsertPosition::END));
  }
  InventoryActionFactory factory(collector);
  int result = tool.run(&factory);
  if (result != 0)
    return result;
  if (!collector.validate())
    return 3;
  if (!collector.write(OutputPath))
    return 5;
  return 0;
}
