/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

//===- postgamma_cc.cpp - semantic PostgreSQL source planner -------------===//
//
// The compiler binds identifier uses to canonical Clang declarations.
// Text spelling is evidence for humans, never the basis of a replacement.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
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

llvm::cl::OptionCategory PostgammaCategory("postgamma-cc options");
llvm::cl::opt<std::string> Mode(
    "mode", llvm::cl::desc("Operation: scan or plan"), llvm::cl::Required,
    llvm::cl::cat(PostgammaCategory));
llvm::cl::opt<std::string> ManifestPath(
    "manifest", llvm::cl::desc("Semantic ownership manifest"),
    llvm::cl::value_desc("path"), llvm::cl::Required,
    llvm::cl::cat(PostgammaCategory));
llvm::cl::opt<std::string> SourceRoot(
    "source-root", llvm::cl::desc("Immutable input source root"),
    llvm::cl::value_desc("path"), llvm::cl::Required,
    llvm::cl::cat(PostgammaCategory));
llvm::cl::opt<std::string> GeneratedRoot(
    "generated-root",
    llvm::cl::desc("Configured PostgreSQL build root containing generated C/headers"),
    llvm::cl::value_desc("path"), llvm::cl::init(""),
    llvm::cl::cat(PostgammaCategory));
llvm::cl::opt<std::string> OutputPath(
    "output", llvm::cl::desc("Deterministic JSON output"),
    llvm::cl::value_desc("path"), llvm::cl::Required,
    llvm::cl::cat(PostgammaCategory));
llvm::cl::opt<std::string> GeneratedOutputPath(
    "generated-output",
    llvm::cl::desc("Plan output for configured-build generated artifacts"),
    llvm::cl::value_desc("path"), llvm::cl::init(""),
    llvm::cl::cat(PostgammaCategory));
llvm::cl::opt<std::string> ClangResourceDir(
    "clang-resource-dir", llvm::cl::desc("Clang builtin-header resource directory"),
    llvm::cl::value_desc("path"), llvm::cl::init(POSTGAMMA_CLANG_RESOURCE_DIR),
    llvm::cl::cat(PostgammaCategory));
llvm::cl::opt<bool> SuppressCompilerWarnings(
    "suppress-compiler-warnings",
    llvm::cl::desc("Suppress warnings inherited from a non-Clang compile database"),
    llvm::cl::init(true), llvm::cl::cat(PostgammaCategory));
llvm::cl::opt<bool> ReportInjectionIncompatibility(
    "report-injection-incompatibility",
    llvm::cl::desc("Write scan output when runtime injection anchors drift"),
    llvm::cl::init(false), llvm::cl::cat(PostgammaCategory));

struct SymbolSpec {
  std::string id;
  std::string name;
  std::string c_type;
  std::string slot;
  std::string replacement;
  std::vector<std::string> declaration_suffixes;
  bool optional = false;
  bool binding_anchor = true;
};

struct InjectionSpec {
  std::string id;
  std::string enclosing_function;
  std::string anchor_callee;
  std::string position;
  std::string code;
  std::vector<std::string> source_suffixes;
  int64_t expected_matches = 0;
};

struct AssumptionSpec {
  std::string id;
  std::string callee;
  std::vector<std::string> allowed_source_suffixes;
  int64_t expected_matches = 0;
};

struct Manifest {
  int64_t schema_version = 0;
  std::map<std::string, SymbolSpec> by_name;
  std::vector<InjectionSpec> injections;
  std::vector<AssumptionSpec> assumptions;
  std::vector<std::string> binding_anchor_suffixes;
};

struct DeclarationRecord {
  std::string symbol_id;
  std::string name;
  std::string usr;
  std::string canonical_type;
  std::string source_kind;
  std::string path;
  unsigned line = 0;
  unsigned column = 0;
  bool definition = false;
  bool type_matches = false;

  auto key() const {
    return std::make_tuple(symbol_id, source_kind, path, line, column, definition, usr,
                           canonical_type);
  }
};

struct UseRecord {
  std::string symbol_id;
  std::string name;
  std::string usr;
  std::string source_kind;
  std::string path;
  unsigned line = 0;
  unsigned column = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
  std::string spelling;
  std::string use_kind;
  bool macro = false;
  std::string macro_kind;
  std::string expansion_source_kind;
  std::string expansion_path;
  unsigned expansion_line = 0;
  unsigned expansion_column = 0;
  std::string replacement;

  auto key() const {
    return std::make_tuple(symbol_id, source_kind, path, offset, length, use_kind,
                           macro_kind, expansion_source_kind, expansion_path,
                           expansion_line, expansion_column);
  }
};

struct InjectionRecord {
  std::string rule_id;
  std::string enclosing_function;
  std::string anchor_callee;
  std::string position;
  std::string source_kind;
  std::string path;
  unsigned line = 0;
  unsigned column = 0;
  uint64_t offset = 0;
  std::string replacement;

  auto key() const {
    return std::make_tuple(rule_id, source_kind, path, offset, position, replacement);
  }
};

struct AssumptionRecord {
  std::string rule_id;
  std::string callee;
  std::string enclosing_function;
  std::string path;
  unsigned line = 0;
  unsigned column = 0;

  auto key() const {
    return std::make_tuple(rule_id, enclosing_function, path, line, column);
  }
};

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

struct RootedPath {
  std::string kind;
  std::string path;
};

bool hasPathSuffix(llvm::StringRef path, llvm::StringRef suffix) {
  std::string normalized_suffix = fs::path(suffix.str()).lexically_normal().generic_string();
  if (!path.ends_with(normalized_suffix))
    return false;
  if (path.size() == normalized_suffix.size())
    return true;
  return path[path.size() - normalized_suffix.size() - 1] == '/';
}

std::string renderInjectedCode(llvm::StringRef code,
                               llvm::StringRef indentation) {
  std::string result;
  result.reserve(code.size() + indentation.size());
  size_t line_start = 0;
  while (line_start < code.size()) {
    size_t line_end = code.find('\n', line_start);
    bool has_newline = line_end != llvm::StringRef::npos;
    if (!has_newline)
      line_end = code.size();
    llvm::StringRef line = code.slice(line_start, line_end);
    llvm::StringRef content = line.ltrim(" \t");
    if (!content.empty()) {
      if (content.front() == '#')
        result.append(content.data(), content.size());
      else {
        result.append(indentation.data(), indentation.size());
        result.append(line.data(), line.size());
      }
    }
    if (has_newline)
      result.push_back('\n');
    line_start = line_end + (has_newline ? 1 : 0);
  }
  return result;
}

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

std::optional<Manifest> loadManifest(llvm::StringRef path) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    llvm::errs() << "postgamma-cc: cannot read manifest " << path << ": "
                 << buffer.getError().message() << "\n";
    return std::nullopt;
  }
  auto parsed = llvm::json::parse(buffer.get()->getBuffer());
  if (!parsed) {
    llvm::errs() << "postgamma-cc: invalid JSON manifest: "
                 << llvm::toString(parsed.takeError()) << "\n";
    return std::nullopt;
  }
  auto *object = parsed->getAsObject();
  if (!object) {
    llvm::errs() << "postgamma-cc: manifest root must be an object\n";
    return std::nullopt;
  }
  auto version = object->getInteger("schema_version");
  auto *symbols = object->getArray("symbols");
  if (!version || *version != 1 || !symbols) {
    llvm::errs() << "postgamma-cc: expected schema_version 1 and symbols array\n";
    return std::nullopt;
  }

  Manifest result;
  result.schema_version = *version;
  if (auto *suffixes = object->getArray("binding_anchor_file_suffixes")) {
    for (const llvm::json::Value &suffix_value : *suffixes) {
      auto suffix = suffix_value.getAsString();
      if (!suffix || suffix->empty()) {
        llvm::errs() << "postgamma-cc: binding anchor suffix must be a string\n";
        return std::nullopt;
      }
      result.binding_anchor_suffixes.emplace_back(suffix->str());
    }
    std::sort(result.binding_anchor_suffixes.begin(),
              result.binding_anchor_suffixes.end());
    result.binding_anchor_suffixes.erase(
        std::unique(result.binding_anchor_suffixes.begin(),
                    result.binding_anchor_suffixes.end()),
        result.binding_anchor_suffixes.end());
  }
  for (const llvm::json::Value &value : *symbols) {
    auto *symbol_object = value.getAsObject();
    if (!symbol_object) {
      llvm::errs() << "postgamma-cc: each symbol must be an object\n";
      return std::nullopt;
    }
    auto id = symbol_object->getString("id");
    auto name = symbol_object->getString("name");
    auto c_type = symbol_object->getString("c_type");
    auto slot = symbol_object->getString("slot");
    auto replacement = symbol_object->getString("replacement");
    auto *suffixes = symbol_object->getArray("declaration_file_suffixes");
    bool optional = symbol_object->getBoolean("optional").value_or(false);
    bool binding_anchor =
        symbol_object->getBoolean("binding_anchor").value_or(true);
    if (!id || !name || !c_type || !slot || !replacement) {
      llvm::errs() << "postgamma-cc: incomplete symbol entry\n";
      return std::nullopt;
    }
    SymbolSpec spec{id->str(), name->str(), c_type->str(), slot->str(),
                    replacement->str(), {}, optional, binding_anchor};
    if (suffixes) {
      for (const llvm::json::Value &suffix_value : *suffixes) {
        auto suffix = suffix_value.getAsString();
        if (!suffix || suffix->empty()) {
          llvm::errs() << "postgamma-cc: declaration suffix must be a string\n";
          return std::nullopt;
        }
        spec.declaration_suffixes.emplace_back(suffix->str());
      }
    }
    if (!result.by_name.emplace(spec.name, std::move(spec)).second) {
      llvm::errs() << "postgamma-cc: duplicate symbol name " << *name << "\n";
      return std::nullopt;
    }
  }

  if (auto *injections = object->getArray("injections")) {
    for (const llvm::json::Value &value : *injections) {
      auto *injection_object = value.getAsObject();
      if (!injection_object) {
        llvm::errs() << "postgamma-cc: each injection must be an object\n";
        return std::nullopt;
      }
      auto id = injection_object->getString("id");
      auto enclosing_function = injection_object->getString("enclosing_function");
      auto anchor_callee = injection_object->getString("anchor_callee");
      auto position = injection_object->getString("position");
      auto code = injection_object->getString("code");
      auto expected_matches = injection_object->getInteger("expected_matches");
      auto *suffixes = injection_object->getArray("source_file_suffixes");
      if (!id || !enclosing_function || !anchor_callee || !position || !code ||
          !expected_matches || *expected_matches < 1) {
        llvm::errs() << "postgamma-cc: incomplete injection entry\n";
        return std::nullopt;
      }
      if (*position != "before" && *position != "after" &&
          *position != "after_enclosing_statement" && *position != "entry") {
        llvm::errs()
            << "postgamma-cc: injection position must be before, after, "
               "after_enclosing_statement, or entry\n";
        return std::nullopt;
      }
      InjectionSpec spec{id->str(), enclosing_function->str(), anchor_callee->str(),
                         position->str(), code->str(), {}, *expected_matches};
      if (suffixes) {
        for (const llvm::json::Value &suffix_value : *suffixes) {
          auto suffix = suffix_value.getAsString();
          if (!suffix || suffix->empty()) {
            llvm::errs() << "postgamma-cc: injection source suffix must be a string\n";
            return std::nullopt;
          }
          spec.source_suffixes.emplace_back(suffix->str());
        }
      }
      bool duplicate = std::any_of(
          result.injections.begin(), result.injections.end(),
          [&](const InjectionSpec &existing) { return existing.id == spec.id; });
      if (duplicate) {
        llvm::errs() << "postgamma-cc: duplicate injection id " << *id << "\n";
        return std::nullopt;
      }
      result.injections.push_back(std::move(spec));
    }
    std::sort(result.injections.begin(), result.injections.end(),
              [](const InjectionSpec &left, const InjectionSpec &right) {
                return left.id < right.id;
              });
  }
  if (auto *assumptions = object->getArray("assumptions")) {
    for (const llvm::json::Value &value : *assumptions) {
      auto *assumption_object = value.getAsObject();
      if (!assumption_object) {
        llvm::errs() << "postgamma-cc: each assumption must be an object\n";
        return std::nullopt;
      }
      auto id = assumption_object->getString("id");
      auto callee = assumption_object->getString("callee");
      auto expected_matches = assumption_object->getInteger("expected_matches");
      auto *suffixes =
          assumption_object->getArray("allowed_source_file_suffixes");
      if (!id || !callee || !expected_matches || *expected_matches < 0 ||
          !suffixes || suffixes->empty()) {
        llvm::errs() << "postgamma-cc: incomplete execution-model assumption\n";
        return std::nullopt;
      }
      AssumptionSpec spec{id->str(), callee->str(), {}, *expected_matches};
      for (const llvm::json::Value &suffix_value : *suffixes) {
        auto suffix = suffix_value.getAsString();
        if (!suffix || suffix->empty()) {
          llvm::errs() << "postgamma-cc: assumption source suffix must be a string\n";
          return std::nullopt;
        }
        spec.allowed_source_suffixes.emplace_back(suffix->str());
      }
      bool duplicate = std::any_of(
          result.assumptions.begin(), result.assumptions.end(),
          [&](const AssumptionSpec &existing) { return existing.id == spec.id; });
      if (duplicate) {
        llvm::errs() << "postgamma-cc: duplicate assumption id " << *id << "\n";
        return std::nullopt;
      }
      result.assumptions.push_back(std::move(spec));
    }
    std::sort(result.assumptions.begin(), result.assumptions.end(),
              [](const AssumptionSpec &left, const AssumptionSpec &right) {
                return left.id < right.id;
              });
  }
  return result;
}

std::string declarationUSR(const VarDecl *declaration) {
  llvm::SmallString<128> usr;
  if (clang::index::generateUSRForDecl(declaration->getCanonicalDecl(), usr))
    return {};
  return usr.str().str();
}

std::string locationPath(const SourceManager &source_manager, SourceLocation location) {
  if (location.isInvalid())
    return {};
  llvm::StringRef filename = source_manager.getFilename(location);
  return normalizeAbsolutePath(filename);
}

bool declarationMatches(const VarDecl *declaration, const SymbolSpec &spec,
                        const SourceManager &source_manager) {
  if (!declaration->isFileVarDecl() || !declaration->hasGlobalStorage())
    return false;
  if (spec.declaration_suffixes.empty())
    return true;
  for (const VarDecl *redecl : declaration->redecls()) {
    SourceLocation location = source_manager.getSpellingLoc(redecl->getLocation());
    std::string path = locationPath(source_manager, location);
    for (const std::string &suffix : spec.declaration_suffixes) {
      if (hasPathSuffix(path, suffix))
        return true;
    }
  }
  return false;
}

bool declarationTypeMatches(const VarDecl *declaration, llvm::StringRef expected) {
  QualType canonical = declaration->getType().getCanonicalType();
  if (expected == "bool")
    return canonical->isBooleanType();
  if (expected == "int")
    return canonical->isSpecificBuiltinType(BuiltinType::Int);
  if (expected == "enum")
    return canonical->isSpecificBuiltinType(BuiltinType::Int);
  if (expected == "real")
    return canonical->isSpecificBuiltinType(BuiltinType::Double);
  if (expected == "string") {
    if (!canonical->isPointerType())
      return false;
    return canonical->getPointeeType().getCanonicalType()->isSpecificBuiltinType(
        BuiltinType::Char_S);
  }
  if (expected == "any")
    return true;
  return false;
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

llvm::json::Object declarationJSON(const DeclarationRecord &record) {
  return llvm::json::Object{{"symbol_id", record.symbol_id},
                            {"name", record.name},
                            {"usr", record.usr},
                            {"canonical_type", record.canonical_type},
                            {"source_kind", record.source_kind},
                            {"path", record.path},
                            {"line", static_cast<int64_t>(record.line)},
                            {"column", static_cast<int64_t>(record.column)},
                            {"definition", record.definition}};
}

llvm::json::Object useJSON(const UseRecord &record) {
  return llvm::json::Object{
      {"symbol_id", record.symbol_id},
      {"name", record.name},
      {"usr", record.usr},
      {"source_kind", record.source_kind},
      {"path", record.path},
      {"line", static_cast<int64_t>(record.line)},
      {"column", static_cast<int64_t>(record.column)},
      {"offset", static_cast<int64_t>(record.offset)},
      {"length", static_cast<int64_t>(record.length)},
      {"spelling", record.spelling},
      {"use_kind", record.use_kind},
      {"macro", record.macro},
      {"macro_kind", record.macro_kind},
      {"expansion_source_kind", record.expansion_source_kind},
      {"expansion_path", record.expansion_path},
      {"expansion_line", static_cast<int64_t>(record.expansion_line)},
      {"expansion_column", static_cast<int64_t>(record.expansion_column)}};
}

llvm::json::Object injectionJSON(const InjectionRecord &record) {
  return llvm::json::Object{
      {"rule_id", record.rule_id},
      {"enclosing_function", record.enclosing_function},
      {"anchor_callee", record.anchor_callee},
      {"position", record.position},
      {"source_kind", record.source_kind},
      {"path", record.path},
      {"line", static_cast<int64_t>(record.line)},
      {"column", static_cast<int64_t>(record.column)},
      {"offset", static_cast<int64_t>(record.offset)}};
}

class SemanticCollector : public MatchFinder::MatchCallback {
 public:
  SemanticCollector(Manifest manifest, std::string source_root,
                    std::string generated_root)
      : manifest_(std::move(manifest)), source_root_(std::move(source_root)),
        generated_root_(std::move(generated_root)) {}

  void run(const MatchFinder::MatchResult &result) override {
    if (const auto *declaration = result.Nodes.getNodeAs<VarDecl>("declaration"))
      collectDeclaration(declaration, *result.SourceManager);
    if (const auto *reference = result.Nodes.getNodeAs<DeclRefExpr>("reference")) {
      const auto *declaration = result.Nodes.getNodeAs<VarDecl>("referenced-variable");
      if (declaration)
        collectUse(reference, declaration, *result.Context, *result.SourceManager);
    }
    if (const auto *call = result.Nodes.getNodeAs<CallExpr>("anchor-call")) {
      const auto *callee = result.Nodes.getNodeAs<FunctionDecl>("anchor-callee");
      const auto *enclosing = result.Nodes.getNodeAs<FunctionDecl>("enclosing-function");
      if (callee && enclosing)
        collectAssumption(call, callee, enclosing, *result.SourceManager);
      if (callee && enclosing)
        collectInjection(call, callee, enclosing, *result.Context,
                         *result.SourceManager);
    }
    if (const auto *function =
            result.Nodes.getNodeAs<FunctionDecl>("entry-function"))
      collectEntryInjection(function, *result.Context, *result.SourceManager);
  }

  void finalizeBindings() {
    if (manifest_.binding_anchor_suffixes.empty()) {
      collectTypeMismatches();
      validateMacroBodyUses();
      return;
    }

    for (const auto &[name, spec] : manifest_.by_name) {
      if (!spec.binding_anchor)
        continue;
      const auto found = binding_usrs_.find(spec.id);
      const bool missing = found == binding_usrs_.end() || found->second.empty();
      if (missing) {
        if (!spec.optional) {
          binding_errors_.insert("no generated-table binding found for " + name);
        }
        eraseNonTargetRecords(spec.id, std::nullopt);
        continue;
      }
      if (found->second.size() != 1 || found->second.count("") != 0) {
        binding_errors_.insert("expected one generated-table binding for " + name +
                               ", found " +
                               std::to_string(found->second.size()));
        eraseNonTargetRecords(spec.id, std::nullopt);
        continue;
      }
      eraseNonTargetRecords(spec.id, *found->second.begin());
    }
    collectTypeMismatches();
    validateMacroBodyUses();
  }

  bool validate(bool report_integration_incompatibility = false) const {
    bool valid = binding_errors_.empty() && macro_body_errors_.empty();
    bool injections_valid = injection_errors_.empty();
    bool assumptions_valid = true;
    for (const std::string &message : binding_errors_)
      llvm::errs() << "postgamma-cc: " << message << "\n";
    for (const std::string &message : macro_body_errors_)
      llvm::errs() << "postgamma-cc: " << message << "\n";
    for (const auto &[id, messages] : injection_errors_)
      for (const std::string &message : messages)
        llvm::errs() << "postgamma-cc: injection " << id << ": " << message
                     << "\n";
    for (const auto &[name, spec] : manifest_.by_name) {
      std::set<std::string> canonical_usrs;
      for (const DeclarationRecord &record : declarations_) {
        if (record.symbol_id == spec.id)
          canonical_usrs.insert(record.usr);
      }
      if (canonical_usrs.empty()) {
        if (!spec.optional) {
          llvm::errs() << "postgamma-cc: no matching declaration found for " << name << "\n";
          valid = false;
        }
        continue;
      }
      if (canonical_usrs.size() != 1 || canonical_usrs.count("") != 0) {
        llvm::errs() << "postgamma-cc: expected one canonical declaration identity for "
                     << name << ", found " << canonical_usrs.size() << "\n";
        valid = false;
      }
    }
    for (const std::string &message : type_mismatches_) {
      llvm::errs() << "postgamma-cc: " << message << "\n";
      valid = false;
    }
    for (const InjectionSpec &spec : manifest_.injections) {
      int64_t matches = std::count_if(
          injections_.begin(), injections_.end(),
          [&](const InjectionRecord &record) { return record.rule_id == spec.id; });
      if (matches != spec.expected_matches) {
        llvm::errs() << "postgamma-cc: injection " << spec.id << " expected "
                     << spec.expected_matches << " semantic anchor match(es), found "
                     << matches << "\n";
        injections_valid = false;
      }
    }
    for (const AssumptionSpec &spec : manifest_.assumptions) {
      int64_t matches = std::count_if(
          assumptions_.begin(), assumptions_.end(),
          [&](const AssumptionRecord &record) { return record.rule_id == spec.id; });
      if (matches != spec.expected_matches) {
        llvm::errs() << "postgamma-cc: assumption " << spec.id << " expected "
                     << spec.expected_matches << " call(s), found " << matches << "\n";
        for (const AssumptionRecord &record : assumptions_) {
          if (record.rule_id == spec.id)
            llvm::errs() << "postgamma-cc: assumption " << spec.id
                         << " unexpected call in " << record.path << ":"
                         << record.line << ":" << record.column << " (function "
                         << record.enclosing_function << ")\n";
        }
        assumptions_valid = false;
      }
      for (const AssumptionRecord &record : assumptions_) {
        if (record.rule_id != spec.id)
          continue;
        bool allowed = std::any_of(
            spec.allowed_source_suffixes.begin(),
            spec.allowed_source_suffixes.end(),
            [&](const std::string &suffix) {
              return hasPathSuffix(record.path, suffix);
            });
        if (!allowed) {
          llvm::errs() << "postgamma-cc: assumption " << spec.id
                       << " matched disallowed source " << record.path << ":"
                       << record.line << "\n";
          assumptions_valid = false;
        }
      }
    }
    return valid &&
           (report_integration_incompatibility ||
            (injections_valid && assumptions_valid));
  }

  bool write(llvm::StringRef mode, llvm::StringRef output,
             const std::optional<std::string> &source_kind = std::nullopt) const {
    llvm::json::Array declarations;
    for (const DeclarationRecord &record : declarations_)
      if (!source_kind || record.source_kind == *source_kind)
        declarations.push_back(declarationJSON(record));

    llvm::json::Array uses;
    for (const UseRecord &record : uses_)
      if (!source_kind || record.source_kind == *source_kind)
        uses.push_back(useJSON(record));

    llvm::json::Array injections;
    for (const InjectionRecord &record : injections_)
      if (!source_kind || record.source_kind == *source_kind)
        injections.push_back(injectionJSON(record));

    llvm::json::Object document;
    document["schema_version"] = 1;
    document["mode"] = mode;
    /* All recorded paths are relative; never leak checkout-specific paths. */
    document["source_root"] = ".";
    document["declarations"] = std::move(declarations);
    document["uses"] = std::move(uses);
    document["injections"] = std::move(injections);
    document["injection_diagnostics"] = injectionDiagnosticsJSON();
    document["assumption_diagnostics"] = assumptionDiagnosticsJSON();
    document["summary"] = summaryJSON(source_kind);
    if (mode == "plan")
      document["files"] = planFilesJSON(source_kind);

    std::error_code error;
    llvm::raw_fd_ostream stream(output, error);
    if (error) {
      llvm::errs() << "postgamma-cc: cannot write " << output << ": "
                   << error.message() << "\n";
      return false;
    }
    stream << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(document)));
    return true;
  }

 private:
  void collectDeclaration(const VarDecl *declaration,
                          const SourceManager &source_manager) {
    auto found = manifest_.by_name.find(declaration->getNameAsString());
    if (found == manifest_.by_name.end() ||
        !declarationMatches(declaration, found->second, source_manager))
      return;
    SourceLocation location = source_manager.getSpellingLoc(declaration->getLocation());
    std::string absolute = locationPath(source_manager, location);
    auto rooted = locateInRoots(absolute, source_root_, generated_root_);
    if (!rooted)
      return;
    PresumedLoc presumed = source_manager.getPresumedLoc(location);
    std::string canonical_type =
        declaration->getType().getCanonicalType().getAsString();
    DeclarationRecord record{found->second.id,
                             found->second.name,
                             declarationUSR(declaration),
                             canonical_type,
                             rooted->kind,
                             rooted->path,
                             presumed.isValid() ? presumed.getLine() : 0,
                             presumed.isValid() ? presumed.getColumn() : 0,
                             declaration->isThisDeclarationADefinition() !=
                                 VarDecl::DeclarationOnly,
                             declarationTypeMatches(declaration,
                                                    found->second.c_type)};
    if (declaration_keys_.insert(record.key()).second) {
      declarations_.push_back(std::move(record));
      std::sort(declarations_.begin(), declarations_.end(),
                [](const auto &left, const auto &right) { return left.key() < right.key(); });
    }
  }

  void collectUse(const DeclRefExpr *reference, const VarDecl *declaration,
                  ASTContext &context, const SourceManager &source_manager) {
    auto found = manifest_.by_name.find(declaration->getNameAsString());
    if (found == manifest_.by_name.end() ||
        !declarationMatches(declaration, found->second, source_manager))
      return;

    SourceLocation raw = reference->getLocation();
    SourceLocation spelling_location = source_manager.getSpellingLoc(raw);
    SourceLocation expansion_location = source_manager.getExpansionLoc(raw);
    std::string macro_kind = "none";
    if (raw.isMacroID())
      macro_kind = source_manager.isMacroArgExpansion(raw) ? "argument" : "body";
    std::string absolute = locationPath(source_manager, spelling_location);
    if (std::any_of(manifest_.binding_anchor_suffixes.begin(),
                    manifest_.binding_anchor_suffixes.end(),
                    [&](const std::string &suffix) {
                      return hasPathSuffix(absolute, suffix);
                    })) {
      binding_usrs_[found->second.id].insert(declarationUSR(declaration));
      return;
    }
    auto rooted = locateInRoots(absolute, source_root_, generated_root_);
    if (!rooted)
      return;

    llvm::StringRef token = Lexer::getSourceText(
        CharSourceRange::getTokenRange(spelling_location, spelling_location),
        source_manager, context.getLangOpts());
    if (token.empty())
      token = found->second.name;
    unsigned length = Lexer::MeasureTokenLength(spelling_location, source_manager,
                                                context.getLangOpts());
    PresumedLoc spelling_presumed = source_manager.getPresumedLoc(spelling_location);
    PresumedLoc expansion_presumed = source_manager.getPresumedLoc(expansion_location);
    std::string expansion_absolute = locationPath(source_manager, expansion_location);
    auto expansion_rooted =
        locateInRoots(expansion_absolute, source_root_, generated_root_);

    UseRecord record{found->second.id,
                     found->second.name,
                     declarationUSR(declaration),
                     rooted->kind,
                     rooted->path,
                     spelling_presumed.isValid() ? spelling_presumed.getLine() : 0,
                     spelling_presumed.isValid() ? spelling_presumed.getColumn() : 0,
                     source_manager.getFileOffset(spelling_location),
                     length,
                     token.str(),
                     classifyUse(reference, context),
                     raw.isMacroID(),
                     macro_kind,
                     expansion_rooted ? expansion_rooted->kind : std::string(),
                     expansion_rooted ? expansion_rooted->path : std::string(),
                     expansion_presumed.isValid() ? expansion_presumed.getLine() : 0,
                     expansion_presumed.isValid() ? expansion_presumed.getColumn() : 0,
                     found->second.replacement};
    if (use_keys_.insert(record.key()).second) {
      uses_.push_back(std::move(record));
      std::sort(uses_.begin(), uses_.end(),
                [](const auto &left, const auto &right) { return left.key() < right.key(); });
    }
  }

  void collectAssumption(const CallExpr *call, const FunctionDecl *callee,
                         const FunctionDecl *enclosing,
                         const SourceManager &source_manager) {
    SourceLocation location = source_manager.getSpellingLoc(call->getBeginLoc());
    if (location.isInvalid() || location.isMacroID())
      return;
    std::string absolute = locationPath(source_manager, location);
    auto rooted = locateInRoots(absolute, source_root_, generated_root_);
    if (!rooted || rooted->kind != "source")
      return;
    for (const AssumptionSpec &spec : manifest_.assumptions) {
      if (callee->getNameAsString() != spec.callee)
        continue;
      PresumedLoc presumed = source_manager.getPresumedLoc(location);
      AssumptionRecord record{spec.id,
                              spec.callee,
                              enclosing->getNameAsString(),
                              rooted->path,
                              presumed.isValid() ? presumed.getLine() : 0,
                              presumed.isValid() ? presumed.getColumn() : 0};
      if (assumption_keys_.insert(record.key()).second) {
        assumptions_.push_back(std::move(record));
        std::sort(assumptions_.begin(), assumptions_.end(),
                  [](const auto &left, const auto &right) {
                    return left.key() < right.key();
                  });
      }
    }
  }

  void collectInjection(const CallExpr *call, const FunctionDecl *callee,
                        const FunctionDecl *enclosing, ASTContext &context,
                        const SourceManager &source_manager) {
    SourceLocation begin = call->getBeginLoc();
    if (begin.isInvalid() || begin.isMacroID())
      return;
    begin = source_manager.getSpellingLoc(begin);
    std::string absolute = locationPath(source_manager, begin);
    auto rooted = locateInRoots(absolute, source_root_, generated_root_);
    if (!rooted)
      return;

    for (const InjectionSpec &spec : manifest_.injections) {
      if (spec.position == "entry")
        continue;
      if (enclosing->getNameAsString() != spec.enclosing_function ||
          callee->getNameAsString() != spec.anchor_callee)
        continue;
      bool source_matches = spec.source_suffixes.empty() || std::any_of(
          spec.source_suffixes.begin(), spec.source_suffixes.end(),
          [&](const std::string &suffix) {
            return hasPathSuffix(rooted->path, suffix);
          });
      if (!source_matches)
        continue;

      const DeclStmt *declaration_anchor = nullptr;
      const Stmt *enclosing_statement_anchor = nullptr;
      auto parents = context.getParents(*call);
      if (parents.size() != 1) {
        injection_errors_[spec.id].insert("anchor has ambiguous AST parents");
        continue;
      }
      if (spec.position == "after_enclosing_statement") {
        DynTypedNode current = DynTypedNode::create(*call);

        while (true) {
          auto current_parents = context.getParents(current);
          if (current_parents.size() != 1) {
            injection_errors_[spec.id].insert(
                "anchor has ambiguous enclosing-statement parents");
            break;
          }
          const DynTypedNode &parent = current_parents[0];
          if (parent.get<CompoundStmt>() != nullptr) {
            enclosing_statement_anchor = current.get<Stmt>();
            if (enclosing_statement_anchor == nullptr)
              injection_errors_[spec.id].insert(
                  "enclosing compound child is not a statement");
            break;
          }
          if (parent.get<Stmt>() == nullptr) {
            injection_errors_[spec.id].insert(
                "anchor is not contained by an executable statement");
            break;
          }
          current = parent;
        }
        if (enclosing_statement_anchor == nullptr)
          continue;
      } else if (parents[0].get<CompoundStmt>() == nullptr) {
        DynTypedNode current = DynTypedNode::create(*call);
        bool saw_variable_declaration = false;
        bool found_declaration_anchor = false;

        while (true) {
          auto current_parents = context.getParents(current);
          if (current_parents.size() != 1)
            break;
          const DynTypedNode &parent = current_parents[0];
          if (parent.get<CompoundStmt>() != nullptr) {
            found_declaration_anchor =
                declaration_anchor != nullptr && current.get<DeclStmt>() != nullptr;
            break;
          }
          if (const auto *statement = parent.get<DeclStmt>()) {
            if (!saw_variable_declaration || declaration_anchor != nullptr)
              break;
            declaration_anchor = statement;
            current = parent;
            continue;
          }
          if (parent.get<VarDecl>() != nullptr) {
            if (saw_variable_declaration || declaration_anchor != nullptr)
              break;
            saw_variable_declaration = true;
            current = parent;
            continue;
          }
          if (parent.get<Expr>() != nullptr) {
            current = parent;
            continue;
          }
          break;
        }
        if (!found_declaration_anchor || spec.position != "after") {
          injection_errors_[spec.id].insert(
              "anchor is neither a standalone expression statement nor an "
              "after-position variable initializer");
          continue;
        }
      }

      SourceLocation statement_begin = enclosing_statement_anchor != nullptr
          ? enclosing_statement_anchor->getBeginLoc()
          : declaration_anchor != nullptr ? declaration_anchor->getBeginLoc()
                                          : call->getBeginLoc();
      statement_begin = source_manager.getSpellingLoc(statement_begin);
      if (statement_begin.isInvalid() || statement_begin.isMacroID() ||
          source_manager.getFileID(statement_begin) != source_manager.getFileID(begin)) {
        injection_errors_[spec.id].insert("cannot locate anchor statement");
        continue;
      }

      FileID file_id = source_manager.getFileID(statement_begin);
      bool invalid_buffer = false;
      llvm::StringRef buffer = source_manager.getBufferData(file_id, &invalid_buffer);
      if (invalid_buffer) {
        injection_errors_[spec.id].insert("cannot read anchor source buffer");
        continue;
      }
      uint64_t begin_offset = source_manager.getFileOffset(statement_begin);
      uint64_t line_start = begin_offset;
      while (line_start > 0 && buffer[line_start - 1] != '\n' &&
             buffer[line_start - 1] != '\r')
        --line_start;
      llvm::StringRef indentation =
          buffer.slice(static_cast<size_t>(line_start), static_cast<size_t>(begin_offset));
      if (!std::all_of(indentation.begin(), indentation.end(),
                       [](char value) { return value == ' ' || value == '\t'; })) {
        injection_errors_[spec.id].insert(
            "anchor does not begin on an indentation-only prefix");
        continue;
      }

      uint64_t insertion_offset = begin_offset;
      std::string rendered_code = renderInjectedCode(spec.code, indentation);
      std::string replacement;
      if (spec.position == "before") {
        insertion_offset = line_start;
        replacement = rendered_code + "\n";
      } else {
        SourceLocation after_semicolon;
        if (enclosing_statement_anchor != nullptr) {
          SourceLocation statement_end = source_manager.getSpellingLoc(
              enclosing_statement_anchor->getEndLoc());
          after_semicolon = Lexer::findLocationAfterToken(
              statement_end, tok::semi, source_manager,
              context.getLangOpts(), false);
        } else if (declaration_anchor != nullptr) {
          SourceLocation statement_end =
              source_manager.getSpellingLoc(declaration_anchor->getEndLoc());
          llvm::StringRef end_token = Lexer::getSourceText(
              CharSourceRange::getTokenRange(statement_end, statement_end),
              source_manager, context.getLangOpts());
          if (end_token == ";")
            after_semicolon = Lexer::getLocForEndOfToken(
                statement_end, 0, source_manager, context.getLangOpts());
          else
            after_semicolon = Lexer::findLocationAfterToken(
                statement_end, tok::semi, source_manager,
                context.getLangOpts(), false);
        } else {
          after_semicolon = Lexer::findLocationAfterToken(
              call->getEndLoc(), tok::semi, source_manager,
              context.getLangOpts(), false);
        }
        if (after_semicolon.isInvalid() || after_semicolon.isMacroID() ||
            source_manager.getFileID(after_semicolon) != file_id) {
          injection_errors_[spec.id].insert(
              "cannot locate anchor statement terminator");
          continue;
        }
        insertion_offset = source_manager.getFileOffset(after_semicolon);
        uint64_t cursor = insertion_offset;
        bool trailing_tokens = false;
        while (cursor < buffer.size() && buffer[cursor] != '\n' &&
               buffer[cursor] != '\r') {
          if (buffer[cursor] != ' ' && buffer[cursor] != '\t') {
            injection_errors_[spec.id].insert(
                "trailing tokens make anchor ambiguous");
            trailing_tokens = true;
            break;
          }
          ++cursor;
        }
        if (trailing_tokens)
          continue;
        replacement = "\n" + rendered_code;
      }

      PresumedLoc presumed = source_manager.getPresumedLoc(begin);
      InjectionRecord record{spec.id,
                             spec.enclosing_function,
                             spec.anchor_callee,
                             spec.position,
                             rooted->kind,
                             rooted->path,
                             presumed.isValid() ? presumed.getLine() : 0,
                             presumed.isValid() ? presumed.getColumn() : 0,
                             insertion_offset,
                             std::move(replacement)};
      if (injection_keys_.insert(record.key()).second) {
        injections_.push_back(std::move(record));
        std::sort(injections_.begin(), injections_.end(),
                  [](const auto &left, const auto &right) {
                    return left.key() < right.key();
                  });
      }
    }
  }

  void collectEntryInjection(const FunctionDecl *function, ASTContext &context,
                             const SourceManager &source_manager) {
    if (!function->doesThisDeclarationHaveABody())
      return;
    const auto *body = dyn_cast<CompoundStmt>(function->getBody());
    if (body == nullptr)
      return;
    SourceLocation brace = source_manager.getSpellingLoc(body->getLBracLoc());
    if (brace.isInvalid() || brace.isMacroID())
      return;
    std::string absolute = locationPath(source_manager, brace);
    auto rooted = locateInRoots(absolute, source_root_, generated_root_);
    if (!rooted)
      return;

    for (const InjectionSpec &spec : manifest_.injections) {
      if (spec.position != "entry" ||
          function->getNameAsString() != spec.enclosing_function)
        continue;
      bool source_matches = spec.source_suffixes.empty() || std::any_of(
          spec.source_suffixes.begin(), spec.source_suffixes.end(),
          [&](const std::string &suffix) {
            return hasPathSuffix(rooted->path, suffix);
          });
      if (!source_matches)
        continue;

      unsigned token_length = Lexer::MeasureTokenLength(
          brace, source_manager, context.getLangOpts());
      if (token_length == 0) {
        injection_errors_[spec.id].insert("cannot locate function-entry brace");
        continue;
      }
      uint64_t insertion_offset =
          source_manager.getFileOffset(brace) + token_length;
      SourceLocation record_location = brace;
      std::string replacement =
          "\n" + renderInjectedCode(spec.code, "\t");

      auto statement = body->body_begin();
      if (statement != body->body_end() && isa<DeclStmt>(*statement)) {
        while (statement != body->body_end() && isa<DeclStmt>(*statement))
          ++statement;
        if (statement != body->body_end()) {
          SourceLocation anchor =
              source_manager.getExpansionLoc((*statement)->getBeginLoc());
          if (anchor.isInvalid() || anchor.isMacroID() ||
              source_manager.getFileID(anchor) != source_manager.getFileID(brace)) {
            injection_errors_[spec.id].insert(
                "cannot locate first executable function-entry statement");
            continue;
          }
          FileID file_id = source_manager.getFileID(anchor);
          bool invalid_buffer = false;
          llvm::StringRef buffer =
              source_manager.getBufferData(file_id, &invalid_buffer);
          if (invalid_buffer) {
            injection_errors_[spec.id].insert(
                "cannot read function-entry source buffer");
            continue;
          }
          uint64_t anchor_offset = source_manager.getFileOffset(anchor);
          uint64_t line_start = anchor_offset;
          while (line_start > 0 && buffer[line_start - 1] != '\n' &&
                 buffer[line_start - 1] != '\r')
            --line_start;
          llvm::StringRef indentation = buffer.slice(
              static_cast<size_t>(line_start),
              static_cast<size_t>(anchor_offset));
          if (!std::all_of(indentation.begin(), indentation.end(),
                           [](char value) {
                             return value == ' ' || value == '\t';
                           })) {
            injection_errors_[spec.id].insert(
                "first executable function-entry statement does not begin on "
                "an indentation-only prefix");
            continue;
          }
          insertion_offset = line_start;
          record_location = anchor;
          replacement = renderInjectedCode(spec.code, indentation) + "\n";
        }
      }

      PresumedLoc presumed = source_manager.getPresumedLoc(record_location);
      InjectionRecord record{
          spec.id,
          spec.enclosing_function,
          spec.anchor_callee,
          spec.position,
          rooted->kind,
          rooted->path,
          presumed.isValid() ? presumed.getLine() : 0,
          presumed.isValid() ? presumed.getColumn() : 0,
          insertion_offset,
          std::move(replacement)};
      if (injection_keys_.insert(record.key()).second) {
        injections_.push_back(std::move(record));
        std::sort(injections_.begin(), injections_.end(),
                  [](const auto &left, const auto &right) {
                    return left.key() < right.key();
                  });
      }
    }
  }

  llvm::json::Array injectionDiagnosticsJSON() const {
    llvm::json::Array diagnostics;
    for (const InjectionSpec &spec : manifest_.injections) {
      int64_t matches = std::count_if(
          injections_.begin(), injections_.end(),
          [&](const InjectionRecord &record) { return record.rule_id == spec.id; });
      llvm::json::Array errors;
      auto found = injection_errors_.find(spec.id);
      if (found != injection_errors_.end())
        for (const std::string &message : found->second)
          errors.push_back(message);
      diagnostics.push_back(llvm::json::Object{
          {"id", spec.id},
          {"expected_matches", spec.expected_matches},
          {"actual_matches", matches},
          {"status", matches == spec.expected_matches && errors.empty()
                         ? "ok"
                         : "incompatible"},
          {"errors", std::move(errors)}});
    }
    return diagnostics;
  }

  llvm::json::Array assumptionDiagnosticsJSON() const {
    llvm::json::Array diagnostics;
    for (const AssumptionSpec &spec : manifest_.assumptions) {
      llvm::json::Array matches;
      bool sources_allowed = true;
      int64_t match_count = 0;
      for (const AssumptionRecord &record : assumptions_) {
        if (record.rule_id != spec.id)
          continue;
        ++match_count;
        bool allowed = std::any_of(
            spec.allowed_source_suffixes.begin(),
            spec.allowed_source_suffixes.end(),
            [&](const std::string &suffix) {
              return hasPathSuffix(record.path, suffix);
            });
        sources_allowed &= allowed;
        matches.push_back(llvm::json::Object{
            {"enclosing_function", record.enclosing_function},
            {"path", record.path},
            {"line", static_cast<int64_t>(record.line)},
            {"column", static_cast<int64_t>(record.column)},
            {"allowed", allowed}});
      }
      bool compatible =
          match_count == spec.expected_matches && sources_allowed;
      diagnostics.push_back(llvm::json::Object{
          {"id", spec.id},
          {"callee", spec.callee},
          {"expected_matches", spec.expected_matches},
          {"actual_matches", match_count},
          {"status", compatible ? "ok" : "incompatible"},
          {"matches", std::move(matches)}});
    }
    return diagnostics;
  }

  llvm::json::Object summaryJSON(
      const std::optional<std::string> &source_kind = std::nullopt) const {
    std::map<std::string, int64_t> by_kind;
    std::map<std::string, int64_t> by_macro_kind;
    std::map<std::string, int64_t> by_symbol;
    int64_t macro_uses = 0;
    for (const UseRecord &record : uses_) {
      if (source_kind && record.source_kind != *source_kind)
        continue;
      ++by_kind[record.use_kind];
      ++by_symbol[record.symbol_id];
      ++by_macro_kind[record.macro_kind];
      if (record.macro)
        ++macro_uses;
    }
    llvm::json::Object kinds;
    for (const auto &[name, count] : by_kind)
      kinds[name] = count;
    llvm::json::Object symbols;
    for (const auto &[name, count] : by_symbol)
      symbols[name] = count;
    llvm::json::Object macro_kinds;
    for (const auto &[name, count] : by_macro_kind)
      macro_kinds[name] = count;
    return llvm::json::Object{
        {"declaration_count", static_cast<int64_t>(std::count_if(
                                  declarations_.begin(), declarations_.end(),
                                  [&](const DeclarationRecord &record) {
                                    return !source_kind ||
                                           record.source_kind == *source_kind;
                                  }))},
        {"use_count", static_cast<int64_t>(std::count_if(
                          uses_.begin(), uses_.end(),
                          [&](const UseRecord &record) {
                            return !source_kind ||
                                   record.source_kind == *source_kind;
                          }))},
        {"injection_count", static_cast<int64_t>(std::count_if(
                                injections_.begin(), injections_.end(),
                                [&](const InjectionRecord &record) {
                                  return !source_kind ||
                                         record.source_kind == *source_kind;
                                }))},
        {"macro_use_count", macro_uses},
        {"by_macro_kind", std::move(macro_kinds)},
        {"by_kind", std::move(kinds)},
        {"by_symbol", std::move(symbols)}};
  }

  void eraseNonTargetRecords(const std::string &symbol_id,
                             const std::optional<std::string> &target_usr) {
    declarations_.erase(
        std::remove_if(declarations_.begin(), declarations_.end(),
                       [&](const DeclarationRecord &record) {
                         return record.symbol_id == symbol_id &&
                                (!target_usr || record.usr != *target_usr);
                       }),
        declarations_.end());
    uses_.erase(
        std::remove_if(uses_.begin(), uses_.end(),
                       [&](const UseRecord &record) {
                         return record.symbol_id == symbol_id &&
                                (!target_usr || record.usr != *target_usr);
                       }),
        uses_.end());
  }

  void collectTypeMismatches() {
    for (const DeclarationRecord &record : declarations_) {
      if (!record.type_matches) {
        const SymbolSpec &spec = manifest_.by_name.at(record.name);
        type_mismatches_.insert("type contract changed for " + record.name +
                                ": expected " + spec.c_type + ", found " +
                                record.canonical_type);
      }
    }
  }

  void validateMacroBodyUses() {
    using Location =
        std::tuple<std::string, std::string, uint64_t, uint64_t>;
    std::map<Location, std::vector<const UseRecord *>> groups;
    std::set<Location> macro_locations;
    for (const UseRecord &record : uses_) {
      Location location{record.source_kind, record.path, record.offset,
                        record.length};
      groups[location].push_back(&record);
      if (record.macro_kind == "body")
        macro_locations.insert(std::move(location));
    }

    for (const auto &[location, records] : groups) {
      if (macro_locations.count(location) == 0)
        continue;
      const UseRecord &first = *records.front();
      const bool consistent = std::all_of(
          records.begin(), records.end(), [&](const UseRecord *record) {
            return record->symbol_id == first.symbol_id &&
                   record->usr == first.usr &&
                   record->spelling == first.spelling &&
                   record->replacement == first.replacement;
          });
      if (!consistent) {
        macro_body_errors_.insert(
            "macro-body token has inconsistent semantic bindings at " +
            first.path + ":" + std::to_string(first.line));
        continue;
      }
      if (first.spelling != first.name || first.length != first.name.size()) {
        macro_body_errors_.insert(
            "macro-body token is not a direct identifier spelling for " +
            first.name + " at " + first.path + ":" +
            std::to_string(first.line));
        continue;
      }
      fs::path path = rootPath(first.source_kind) / first.path;
      auto buffer = llvm::MemoryBuffer::getFile(path.string());
      if (!buffer || first.offset + first.length >
                         buffer.get()->getBufferSize() ||
          buffer.get()->getBuffer().substr(first.offset, first.length) !=
              first.spelling) {
        macro_body_errors_.insert(
            "macro-body source bytes changed at " + first.path + ":" +
            std::to_string(first.line));
      }
    }
  }

  fs::path rootPath(llvm::StringRef source_kind) const {
    if (source_kind == "source")
      return fs::path(source_root_);
    if (source_kind == "generated_build" && !generated_root_.empty())
      return fs::path(generated_root_);
    return {};
  }

  std::string fileSHA256(llvm::StringRef source_kind,
                         llvm::StringRef relative) const {
    fs::path path = rootPath(source_kind) / relative.str();
    auto buffer = llvm::MemoryBuffer::getFile(path.string());
    if (!buffer)
      return {};
    llvm::StringRef bytes = buffer.get()->getBuffer();
    auto digest = llvm::SHA256::hash(llvm::arrayRefFromStringRef(bytes));
    return llvm::toHex(llvm::ArrayRef<uint8_t>(digest), true);
  }

  llvm::json::Array planFilesJSON(
      const std::optional<std::string> &source_kind) const {
    struct PlannedReplacement {
      std::string rule_id;
      std::string kind;
      std::string use_kind;
      uint64_t offset = 0;
      uint64_t length = 0;
      std::string original;
      std::string replacement;
      unsigned line = 0;
      unsigned column = 0;
    };
    std::map<std::string, std::vector<PlannedReplacement>> files;
    std::set<std::tuple<std::string, std::string, uint64_t, uint64_t>>
        planned_symbol_locations;
    for (const UseRecord &record : uses_) {
      if (source_kind && record.source_kind != *source_kind)
        continue;
      if (!planned_symbol_locations
               .insert({record.source_kind, record.path, record.offset,
                        record.length})
               .second)
        continue;
      files[record.path].push_back(
          {record.symbol_id, "symbol",
           record.macro_kind == "body" ? "macro_body" : record.use_kind,
           record.offset, record.length, record.spelling, record.replacement,
           record.line, record.column});
    }
    for (const InjectionRecord &record : injections_) {
      if (source_kind && record.source_kind != *source_kind)
        continue;
      files[record.path].push_back(
          {record.rule_id, "injection", "injection_" + record.position,
           record.offset, 0, "", record.replacement, record.line, record.column});
    }
    llvm::json::Array result;
    for (auto &[path, records] : files) {
      std::sort(records.begin(), records.end(), [](const PlannedReplacement &left,
                                                   const PlannedReplacement &right) {
        return std::tie(left.offset, left.length, left.rule_id, left.replacement) <
               std::tie(right.offset, right.length, right.rule_id, right.replacement);
      });
      llvm::json::Array replacements;
      for (const PlannedReplacement &record : records) {
        replacements.push_back(llvm::json::Object{
            {"rule_id", record.rule_id},
            {"kind", record.kind},
            {"use_kind", record.use_kind},
            {"offset", static_cast<int64_t>(record.offset)},
            {"length", static_cast<int64_t>(record.length)},
            {"original", record.original},
            {"replacement", record.replacement},
            {"line", static_cast<int64_t>(record.line)},
            {"column", static_cast<int64_t>(record.column)}});
      }
      llvm::StringRef kind = source_kind ? llvm::StringRef(*source_kind)
                                         : llvm::StringRef("source");
      result.push_back(llvm::json::Object{{"path", path},
                                          {"sha256", fileSHA256(kind, path)},
                                          {"replacements", std::move(replacements)}});
    }
    return result;
  }

  Manifest manifest_;
  std::string source_root_;
  std::string generated_root_;
  std::vector<DeclarationRecord> declarations_;
  std::vector<UseRecord> uses_;
  std::vector<InjectionRecord> injections_;
  std::vector<AssumptionRecord> assumptions_;
  std::set<decltype(std::declval<DeclarationRecord>().key())> declaration_keys_;
  std::set<decltype(std::declval<UseRecord>().key())> use_keys_;
  std::set<decltype(std::declval<InjectionRecord>().key())> injection_keys_;
  std::set<decltype(std::declval<AssumptionRecord>().key())> assumption_keys_;
  std::map<std::string, std::set<std::string>> binding_usrs_;
  std::set<std::string> binding_errors_;
  std::set<std::string> macro_body_errors_;
  std::set<std::string> type_mismatches_;
  std::map<std::string, std::set<std::string>> injection_errors_;
};

class PostgammaAction : public ASTFrontendAction {
 public:
  explicit PostgammaAction(SemanticCollector &collector) : collector_(collector) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &,
                                                 llvm::StringRef) override {
    finder_.addMatcher(varDecl().bind("declaration"), &collector_);
    finder_.addMatcher(
        declRefExpr(to(varDecl().bind("referenced-variable"))).bind("reference"),
        &collector_);
    finder_.addMatcher(
        callExpr(callee(functionDecl().bind("anchor-callee")),
                 hasAncestor(functionDecl().bind("enclosing-function")))
            .bind("anchor-call"),
        &collector_);
    finder_.addMatcher(functionDecl(isDefinition()).bind("entry-function"),
                       &collector_);
    return finder_.newASTConsumer();
  }

 private:
  SemanticCollector &collector_;
  MatchFinder finder_;
};

class PostgammaActionFactory : public FrontendActionFactory {
 public:
  explicit PostgammaActionFactory(SemanticCollector &collector) : collector_(collector) {}

  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<PostgammaAction>(collector_);
  }

 private:
  SemanticCollector &collector_;
};

}  // namespace

int main(int argc, const char **argv) {
  auto options = CommonOptionsParser::create(argc, argv, PostgammaCategory);
  if (!options) {
    llvm::errs() << llvm::toString(options.takeError()) << "\n";
    return 2;
  }
  if (Mode != "scan" && Mode != "plan") {
    llvm::errs() << "postgamma-cc: --mode must be scan or plan\n";
    return 2;
  }
  auto manifest = loadManifest(ManifestPath);
  if (!manifest)
    return 2;
  std::string source_root = normalizeAbsolutePath(SourceRoot);
  if (source_root.empty()) {
    llvm::errs() << "postgamma-cc: invalid source root\n";
    return 2;
  }
  std::string generated_root;
  if (!GeneratedRoot.empty()) {
    generated_root = normalizeAbsolutePath(GeneratedRoot);
    if (generated_root.empty()) {
      llvm::errs() << "postgamma-cc: invalid generated root\n";
      return 2;
    }
  }
  if (Mode == "scan" && !GeneratedOutputPath.empty()) {
    llvm::errs() << "postgamma-cc: --generated-output is only valid in plan mode\n";
    return 2;
  }
  if (Mode != "scan" && ReportInjectionIncompatibility) {
    llvm::errs() << "postgamma-cc: --report-injection-incompatibility is only "
                    "valid in scan mode\n";
    return 2;
  }
  if (Mode == "plan" &&
      (generated_root.empty() != GeneratedOutputPath.empty())) {
    llvm::errs() << "postgamma-cc: --generated-root and --generated-output "
                    "must be used together in plan mode\n";
    return 2;
  }

  SemanticCollector collector(std::move(*manifest), source_root,
                              generated_root);
  ClangTool tool(options->getCompilations(), options->getSourcePathList());
  if (!ClangResourceDir.empty()) {
    CommandLineArguments resource_arguments{
        "-resource-dir=" + ClangResourceDir};
    tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
        resource_arguments,
        ArgumentInsertPosition::BEGIN));
  }
  if (SuppressCompilerWarnings) {
    CommandLineArguments warning_arguments{"-w"};
    tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
        warning_arguments, ArgumentInsertPosition::END));
  }
  PostgammaActionFactory factory(collector);
  int result = tool.run(&factory);
  if (result != 0)
    return result;
  collector.finalizeBindings();
  if (!collector.validate(ReportInjectionIncompatibility))
    return 3;
  if (Mode == "scan") {
    if (!collector.write(Mode, OutputPath))
      return 5;
  } else {
    if (!collector.write(Mode, OutputPath, std::string("source")))
      return 5;
    if (!generated_root.empty() &&
        !collector.write(Mode, GeneratedOutputPath,
                         std::string("generated_build")))
      return 5;
  }
  return 0;
}
