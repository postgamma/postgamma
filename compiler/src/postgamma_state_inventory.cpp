/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

//===- postgamma_state_inventory.cpp - mutable server state inventory -----===//
//
// Discover every source-owned object with static storage duration that can
// be mutated by PostgreSQL server translation units.  The output is an audit
// catalog, not an ownership guess: humans decide the lifetime of each object
// in a separate policy that must align exactly with this catalog.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
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

llvm::cl::OptionCategory InventoryCategory("PostGamma state inventory options");
llvm::cl::opt<std::string> SourceRoot(
    "source-root", llvm::cl::desc("Immutable PostgreSQL source root"),
    llvm::cl::value_desc("path"), llvm::cl::Required,
    llvm::cl::cat(InventoryCategory));
llvm::cl::opt<std::string> GeneratedRoot(
    "generated-root",
    llvm::cl::desc("Configured PostgreSQL build root containing generated C/headers"),
    llvm::cl::value_desc("path"), llvm::cl::init(""),
    llvm::cl::cat(InventoryCategory));
llvm::cl::opt<std::string> OutputPath(
    "output", llvm::cl::desc("Deterministic JSON output"),
    llvm::cl::value_desc("path"), llvm::cl::Required,
    llvm::cl::cat(InventoryCategory));
llvm::cl::opt<std::string> DefinitionDomain(
    "definition-domain",
    llvm::cl::desc("JSON source-domain manifest controlling owned definitions"),
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

struct SourceDomain {
  std::vector<std::string> prefixes;
  std::set<std::string> files;
};

std::optional<SourceDomain> loadDefinitionDomain(
    llvm::StringRef manifest_path) {
  auto buffer = llvm::MemoryBuffer::getFile(manifest_path);
  if (!buffer) {
    llvm::errs() << "postgamma-state-inventory: cannot read definition domain "
                 << manifest_path << ": " << buffer.getError().message() << "\n";
    return std::nullopt;
  }
  auto parsed = llvm::json::parse(buffer.get()->getBuffer());
  if (!parsed) {
    llvm::errs() << "postgamma-state-inventory: invalid definition domain: "
                 << llvm::toString(parsed.takeError()) << "\n";
    return std::nullopt;
  }
  llvm::json::Object *document = parsed->getAsObject();
  if (document == nullptr || document->getInteger("schema_version") != 1 ||
      document->getString("kind") != "postgamma.postgres-source-domain") {
    llvm::errs() << "postgamma-state-inventory: unsupported definition domain\n";
    return std::nullopt;
  }
  SourceDomain domain;
  auto load_paths = [&](llvm::StringRef field, bool prefix) -> bool {
    llvm::json::Array *raw = document->getArray(field);
    if (raw == nullptr)
      return true;
    for (const llvm::json::Value &value : *raw) {
      auto path = value.getAsString();
      if (!path || path->empty() || path->starts_with("/") ||
          path->contains("..")) {
        llvm::errs() << "postgamma-state-inventory: invalid definition "
                     << (prefix ? "prefix" : "file") << "\n";
        return false;
      }
      if (prefix)
        domain.prefixes.push_back(path->str());
      else
        domain.files.insert(path->str());
    }
    return true;
  };
  if (!load_paths("translation_unit_prefixes", true) ||
      !load_paths("translation_unit_files", false))
    return std::nullopt;
  std::sort(domain.prefixes.begin(), domain.prefixes.end());
  domain.prefixes.erase(
      std::unique(domain.prefixes.begin(), domain.prefixes.end()),
      domain.prefixes.end());
  if (domain.prefixes.empty() && domain.files.empty()) {
    llvm::errs() << "postgamma-state-inventory: definition domain is empty\n";
    return std::nullopt;
  }
  return domain;
}

struct RootedPath {
  std::string kind;
  std::string path;
};

std::optional<std::string> relativeToRoot(llvm::StringRef absolute,
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

std::string declarationUSR(const VarDecl *declaration) {
  llvm::SmallString<128> usr;
  if (clang::index::generateUSRForDecl(declaration->getCanonicalDecl(), usr))
    return {};
  return usr.str().str();
}

std::string storageClassName(StorageClass storage_class) {
  switch (storage_class) {
    case SC_None:
      return "none";
    case SC_Extern:
      return "extern";
    case SC_Static:
      return "static";
    case SC_PrivateExtern:
      return "private_extern";
    case SC_Auto:
      return "auto";
    case SC_Register:
      return "register";
  }
  return "unknown";
}

std::string tlsKindName(VarDecl::TLSKind kind) {
  switch (kind) {
    case VarDecl::TLS_None:
      return "none";
    case VarDecl::TLS_Static:
      return "static";
    case VarDecl::TLS_Dynamic:
      return "dynamic";
  }
  return "unknown";
}

std::string definitionKindName(VarDecl::DefinitionKind kind) {
  switch (kind) {
    case VarDecl::DeclarationOnly:
      return "declaration";
    case VarDecl::TentativeDefinition:
      return "tentative";
    case VarDecl::Definition:
      return "definition";
  }
  return "unknown";
}

int definitionKindRank(llvm::StringRef kind) {
  if (kind == "definition")
    return 2;
  if (kind == "tentative")
    return 1;
  return 0;
}

bool isMutableStaticStorage(const VarDecl *declaration) {
  QualType type = declaration->getType();
  return declaration->hasGlobalStorage() && !type.isNull() &&
         !type.isConstQualified() && !type->isFunctionType();
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

const Expr *stripTransparentParents(const Expr *expression, ASTContext &context,
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

std::string classifyUse(const DeclRefExpr *reference, ASTContext &context) {
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

void collectStaticInitializerOwners(const DynTypedNode &node,
                                    ASTContext &context,
                                    std::set<const VarDecl *> &owners,
                                    unsigned depth) {
  if (depth > 128)
    return;
  for (const DynTypedNode &parent : context.getParents(node)) {
    if (const auto *variable = parent.get<VarDecl>()) {
      if (variable->hasGlobalStorage() && variable->hasInit())
        owners.insert(variable);
      continue;
    }
    /* A function boundary proves that this branch is a runtime expression. */
    if (parent.get<FunctionDecl>() != nullptr)
      continue;
    collectStaticInitializerOwners(parent, context, owners, depth + 1);
  }
}

const VarDecl *staticInitializerOwner(const DeclRefExpr *reference,
                                      ASTContext &context) {
  std::set<const VarDecl *> owners;
  collectStaticInitializerOwners(DynTypedNode::create(*reference), context,
                                 owners, 0);
  return owners.size() == 1 ? *owners.begin() : nullptr;
}

struct DefinitionRecord {
  std::string id;
  std::string name;
  std::string usr;
  std::string canonical_type;
  std::string storage_class;
  std::string tls_kind;
  std::string definition_kind;
  std::string source_kind;
  std::string definition_path;
  unsigned line = 0;
  unsigned column = 0;
  uint64_t size = 0;
  uint64_t alignment = 0;
  bool externally_visible = false;
  bool file_scope = false;
  bool function_static = false;
  bool volatile_qualified = false;
  bool atomic_type = false;
  bool array_type = false;
  bool complete_type = false;
  bool trivially_copyable = false;
  bool has_initializer = false;
  bool constant_initializer = false;
  std::string initializer_class;
  std::set<std::string> declaration_paths;
  std::set<std::string> translation_units;
};

struct UseRecord {
  std::string id;
  std::string name;
  std::string usr;
  std::string source_kind;
  std::string path;
  std::string translation_unit;
  unsigned line = 0;
  unsigned column = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
  std::string use_kind;
  std::string macro_kind;
  bool in_static_initializer = false;
  std::string static_initializer_owner_id;

  auto key() const {
    return std::make_tuple(id, source_kind, path, offset, length, use_kind, macro_kind,
                           translation_unit, in_static_initializer,
                           static_initializer_owner_id);
  }
};

class MutableStateCollector : public MatchFinder::MatchCallback {
 public:
  MutableStateCollector(std::string source_root, std::string generated_root,
                        SourceDomain definition_domain)
      : source_root_(std::move(source_root)),
        generated_root_(std::move(generated_root)),
        definition_domain_(std::move(definition_domain)) {}

  void run(const MatchFinder::MatchResult &result) override {
    if (const auto *declaration =
            result.Nodes.getNodeAs<VarDecl>("state-declaration"))
      collectDefinition(declaration, *result.Context, *result.SourceManager);
    if (const auto *reference =
            result.Nodes.getNodeAs<DeclRefExpr>("state-reference")) {
      if (const auto *declaration =
              result.Nodes.getNodeAs<VarDecl>("state-variable"))
        collectUse(reference, declaration, *result.Context,
                   *result.SourceManager);
    }
  }

  bool validate() const {
    for (const auto &[id, conflicts] : definition_conflicts_) {
      llvm::errs() << "postgamma-state-inventory: conflicting definitions for "
                   << id << ":";
      for (const std::string &conflict : conflicts)
        llvm::errs() << " " << conflict;
      llvm::errs() << "\n";
    }
    return definition_conflicts_.empty();
  }

  bool write(llvm::StringRef output) const {
    std::set<std::string> known_ids;
    for (const auto &[id, record] : definitions_)
      known_ids.insert(id);

    std::map<std::string, std::map<std::string, int64_t>> use_counts;
    std::map<std::string, int64_t> macro_counts;
    std::map<std::string, int64_t> initializer_counts;
    llvm::json::Array uses;
    llvm::json::Array external_references;
    int64_t emitted_use_count = 0;
    int64_t external_reference_count = 0;
    for (const UseRecord &record : uses_) {
      llvm::json::Object value{
          {"id", record.id},
          {"name", record.name},
          {"usr", record.usr},
          {"source_kind", record.source_kind},
          {"path", record.path},
          {"translation_unit", record.translation_unit},
          {"line", static_cast<int64_t>(record.line)},
          {"column", static_cast<int64_t>(record.column)},
          {"offset", static_cast<int64_t>(record.offset)},
          {"length", static_cast<int64_t>(record.length)},
          {"use_kind", record.use_kind},
          {"macro_kind", record.macro_kind},
          {"in_static_initializer", record.in_static_initializer},
          {"static_initializer_owner_id",
           record.static_initializer_owner_id}};
      if (known_ids.count(record.id) != 0) {
        ++emitted_use_count;
        ++use_counts[record.id][record.use_kind];
        if (record.macro_kind != "none")
          ++macro_counts[record.id];
        if (record.in_static_initializer)
          ++initializer_counts[record.id];
        uses.push_back(std::move(value));
      } else if (llvm::StringRef(record.id).starts_with("external:")) {
        ++external_reference_count;
        external_references.push_back(std::move(value));
      }
    }

    llvm::json::Array candidates;
    std::map<std::string, int64_t> by_storage;
    int64_t volatile_count = 0;
    int64_t atomic_count = 0;
    int64_t function_static_count = 0;
    int64_t initializer_use_count = 0;
    for (const auto &[id, record] : definitions_) {
      llvm::json::Object kinds;
      int64_t total_uses = 0;
      auto count_it = use_counts.find(id);
      if (count_it != use_counts.end()) {
        for (const auto &[kind, count] : count_it->second) {
          kinds[kind] = count;
          total_uses += count;
        }
      }
      llvm::json::Array translation_units;
      for (const std::string &path : record.translation_units)
        translation_units.push_back(path);
      llvm::json::Array declaration_paths;
      for (const std::string &path : record.declaration_paths)
        declaration_paths.push_back(path);
      candidates.push_back(llvm::json::Object{
          {"id", id},
          {"name", record.name},
          {"usr", record.usr},
          {"canonical_type", record.canonical_type},
          {"storage_class", record.storage_class},
          {"tls_kind", record.tls_kind},
          {"definition_kind", record.definition_kind},
          {"definition_source_kind", record.source_kind},
          {"definition_path", record.definition_path},
          {"line", static_cast<int64_t>(record.line)},
          {"column", static_cast<int64_t>(record.column)},
          {"size", static_cast<int64_t>(record.size)},
          {"alignment", static_cast<int64_t>(record.alignment)},
          {"externally_visible", record.externally_visible},
          {"file_scope", record.file_scope},
          {"function_static", record.function_static},
          {"volatile", record.volatile_qualified},
          {"atomic", record.atomic_type},
          {"array", record.array_type},
          {"complete_type", record.complete_type},
          {"trivially_copyable", record.trivially_copyable},
          {"has_initializer", record.has_initializer},
          {"constant_initializer", record.constant_initializer},
          {"initializer_class", record.initializer_class},
          {"declaration_paths", std::move(declaration_paths)},
          {"translation_units", std::move(translation_units)},
          {"use_count", total_uses},
          {"macro_use_count", macro_counts[id]},
          {"static_initializer_use_count", initializer_counts[id]},
          {"uses_by_kind", std::move(kinds)}});
      ++by_storage[record.function_static ? "function_static" :
                   (record.externally_visible ? "external" : "internal")];
      volatile_count += record.volatile_qualified;
      atomic_count += record.atomic_type;
      function_static_count += record.function_static;
      initializer_use_count += initializer_counts[id];
    }

    llvm::json::Object storage_summary;
    for (const auto &[kind, count] : by_storage)
      storage_summary[kind] = count;
    llvm::json::Object document{
        {"schema_version", 1},
        {"kind", "postgamma.backend-mutable-state-catalog"},
        {"source_root", "."},
        {"candidates", std::move(candidates)},
        {"uses", std::move(uses)},
        {"external_references", std::move(external_references)},
        {"summary", llvm::json::Object{
             {"candidate_count", static_cast<int64_t>(definitions_.size())},
             {"use_count", emitted_use_count},
             {"external_reference_count", external_reference_count},
             {"volatile_count", volatile_count},
             {"atomic_count", atomic_count},
             {"function_static_count", function_static_count},
             {"static_initializer_use_count", initializer_use_count},
             {"by_storage", std::move(storage_summary)}}}};

    std::error_code error;
    llvm::raw_fd_ostream stream(output, error);
    if (error) {
      llvm::errs() << "postgamma-state-inventory: cannot write " << output
                   << ": " << error.message() << "\n";
      return false;
    }
    stream << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(document)));
    return true;
  }

 private:
  void collectDefinition(const VarDecl *declaration, ASTContext &context,
                         const SourceManager &source_manager) {
    if (!isMutableStaticStorage(declaration) ||
        declaration->isThisDeclarationADefinition() ==
            VarDecl::DeclarationOnly)
      return;
    SourceLocation location =
        source_manager.getSpellingLoc(declaration->getLocation());
    auto definition_location = locateInRoots(
        locationPath(source_manager, location), source_root_, generated_root_);
    if (!definition_location)
      return;
    auto translation_unit =
        mainTranslationUnit(source_manager, source_root_, generated_root_);
    if (!translation_unit)
      return;
    bool matched = definition_domain_.files.count(translation_unit->path) != 0;
    for (const std::string &prefix : definition_domain_.prefixes)
      matched |= llvm::StringRef(translation_unit->path).starts_with(prefix);
    if (!matched)
      return;
    std::string id = stableIdentity(declaration, source_manager, source_root_,
                                    generated_root_);
    if (id.empty())
      return;

    QualType type = declaration->getType().getCanonicalType();
    PrintingPolicy type_policy(context.getLangOpts());
    type_policy.AnonymousTagLocations = false;
    std::string canonical_type = type.getAsString(type_policy);
    bool complete = !type->isIncompleteType() && !type->isUndeducedType();
    uint64_t size = 0;
    uint64_t alignment = 0;
    if (complete && !type->isSizelessType()) {
      size = static_cast<uint64_t>(context.getTypeSizeInChars(type).getQuantity());
      alignment =
          static_cast<uint64_t>(context.getTypeAlignInChars(type).getQuantity());
    }
    const Expr *initializer = declaration->getInit();
    std::string definition_kind =
        definitionKindName(declaration->isThisDeclarationADefinition());
    PresumedLoc presumed = source_manager.getPresumedLoc(location);
    DefinitionRecord record{
        id,
        declaration->getNameAsString(),
        declarationUSR(declaration),
        canonical_type,
        storageClassName(declaration->getStorageClass()),
        tlsKindName(declaration->getTLSKind()),
        definition_kind,
        definition_location->kind,
        definition_location->path,
        presumed.isValid() ? presumed.getLine() : 0,
        presumed.isValid() ? presumed.getColumn() : 0,
        size,
        alignment,
        declaration->isExternallyVisible(),
        declaration->isFileVarDecl(),
        declaration->isLocalVarDecl(),
        type.isVolatileQualified(),
        type->isAtomicType(),
        type->isArrayType(),
        complete,
        complete && type.isTriviallyCopyableType(context),
        initializer != nullptr,
        declaration->hasConstantInitialization(),
        initializer == nullptr ? "implicit_zero" : initializer->getStmtClassName(),
        {identityPath(*definition_location)},
        {identityPath(*translation_unit)}};

    auto [found, inserted] = definitions_.emplace(id, std::move(record));
    if (!inserted) {
      DefinitionRecord &existing = found->second;
      existing.translation_units.insert(identityPath(*translation_unit));
      existing.declaration_paths.insert(identityPath(*definition_location));
      if (existing.usr != declarationUSR(declaration) ||
          existing.canonical_type != canonical_type || existing.size != size ||
          existing.alignment != alignment) {
        definition_conflicts_[id].insert(existing.definition_path);
        definition_conflicts_[id].insert(identityPath(*definition_location));
      } else if (definitionKindRank(definition_kind) >
                 definitionKindRank(existing.definition_kind)) {
        existing.definition_kind = definition_kind;
        existing.source_kind = definition_location->kind;
        existing.definition_path = definition_location->path;
        existing.line = presumed.isValid() ? presumed.getLine() : 0;
        existing.column = presumed.isValid() ? presumed.getColumn() : 0;
        existing.storage_class = storageClassName(declaration->getStorageClass());
        existing.has_initializer = initializer != nullptr;
        existing.constant_initializer = declaration->hasConstantInitialization();
        existing.initializer_class = initializer == nullptr
                                         ? "implicit_zero"
                                         : initializer->getStmtClassName();
      }
    }
  }

  void collectUse(const DeclRefExpr *reference, const VarDecl *declaration,
                  ASTContext &context, const SourceManager &source_manager) {
    if (!isMutableStaticStorage(declaration))
      return;
    std::string id = stableIdentity(declaration, source_manager, source_root_,
                                    generated_root_);
    if (id.empty())
      return;
    SourceLocation raw = reference->getLocation();
    SourceLocation spelling = source_manager.getSpellingLoc(raw);
    auto use_location = locateInRoots(locationPath(source_manager, spelling),
                                      source_root_, generated_root_);
    if (!use_location)
      return;
    auto translation_unit =
        mainTranslationUnit(source_manager, source_root_, generated_root_);
    if (!translation_unit)
      return;
    std::string macro_kind = "none";
    if (raw.isMacroID())
      macro_kind = source_manager.isMacroArgExpansion(raw) ? "argument" : "body";
    unsigned length =
        Lexer::MeasureTokenLength(spelling, source_manager, context.getLangOpts());
    PresumedLoc presumed = source_manager.getPresumedLoc(spelling);
    const VarDecl *initializer_owner =
        staticInitializerOwner(reference, context);
    std::string initializer_owner_id;
    if (initializer_owner != nullptr)
      initializer_owner_id = stableIdentity(initializer_owner, source_manager,
                                            source_root_, generated_root_);
    UseRecord record{id,
                     declaration->getNameAsString(),
                     declarationUSR(declaration),
                     use_location->kind,
                     use_location->path,
                     identityPath(*translation_unit),
                     presumed.isValid() ? presumed.getLine() : 0,
                     presumed.isValid() ? presumed.getColumn() : 0,
                     source_manager.getFileOffset(spelling),
                     length,
                     classifyUse(reference, context),
                     macro_kind,
                     initializer_owner != nullptr,
                     std::move(initializer_owner_id)};
    if (use_keys_.insert(record.key()).second)
      uses_.push_back(std::move(record));
  }

  std::string source_root_;
  std::string generated_root_;
  SourceDomain definition_domain_;
  std::map<std::string, DefinitionRecord> definitions_;
  std::vector<UseRecord> uses_;
  std::set<decltype(std::declval<UseRecord>().key())> use_keys_;
  std::map<std::string, std::set<std::string>> definition_conflicts_;
};

class InventoryAction : public ASTFrontendAction {
 public:
  explicit InventoryAction(MutableStateCollector &collector)
      : collector_(collector) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &,
                                                 llvm::StringRef) override {
    finder_.addMatcher(varDecl(hasGlobalStorage()).bind("state-declaration"),
                       &collector_);
    finder_.addMatcher(
        declRefExpr(to(varDecl(hasGlobalStorage()).bind("state-variable")))
            .bind("state-reference"),
        &collector_);
    return finder_.newASTConsumer();
  }

 private:
  MutableStateCollector &collector_;
  MatchFinder finder_;
};

class InventoryActionFactory : public FrontendActionFactory {
 public:
  explicit InventoryActionFactory(MutableStateCollector &collector)
      : collector_(collector) {}

  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<InventoryAction>(collector_);
  }

 private:
  MutableStateCollector &collector_;
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
    llvm::errs() << "postgamma-state-inventory: invalid source root\n";
    return 2;
  }
  std::string generated_root;
  if (!GeneratedRoot.empty()) {
    generated_root = normalizeAbsolutePath(GeneratedRoot);
    if (generated_root.empty()) {
      llvm::errs() << "postgamma-state-inventory: invalid generated root\n";
      return 2;
    }
  }

  auto definition_domain = loadDefinitionDomain(DefinitionDomain);
  if (!definition_domain)
    return 2;

  MutableStateCollector collector(source_root, generated_root,
                                  std::move(*definition_domain));
  ClangTool tool(options->getCompilations(), options->getSourcePathList());
  if (!ClangResourceDir.empty()) {
    CommandLineArguments resource_arguments{"-resource-dir=" + ClangResourceDir};
    tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
        resource_arguments, ArgumentInsertPosition::BEGIN));
  }
  if (SuppressCompilerWarnings) {
    /*
     * A compile database captured from GCC can expose GCC-only builtins to
     * Clang (currently __cpuidex in src/port/pg_cpu_x86.c).  Keep parsing the
     * exact configured branch while downgrading only the C implicit-function
     * diagnostic; -w then keeps inventory output deterministic and quiet.
     */
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
