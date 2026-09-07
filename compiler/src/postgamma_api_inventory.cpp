/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

//===- postgamma_api_inventory.cpp - public C declaration inventory -------===//
//
// Derive the PostGamma public declaration and structure-layout catalog from
// Clang's AST.  Human-reviewed ownership and behavioral policy lives in a
// separate manifest and must align exactly with this mechanical inventory.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace clang;
using namespace clang::tooling;

namespace {

#ifndef POSTGAMMA_CLANG_RESOURCE_DIR
#define POSTGAMMA_CLANG_RESOURCE_DIR ""
#endif

llvm::cl::OptionCategory InventoryCategory(
    "PostGamma public API inventory options");
llvm::cl::opt<std::string> SourceRoot(
    "source-root", llvm::cl::desc("PostGamma project source root"),
    llvm::cl::value_desc("path"), llvm::cl::Required,
    llvm::cl::cat(InventoryCategory));
llvm::cl::list<std::string> PublicHeaders(
    "public-header", llvm::cl::desc("Public header to inventory"),
    llvm::cl::value_desc("path"), llvm::cl::OneOrMore,
    llvm::cl::cat(InventoryCategory));
llvm::cl::opt<std::string> OutputPath(
    "output", llvm::cl::desc("Deterministic JSON output"),
    llvm::cl::value_desc("path"), llvm::cl::Required,
    llvm::cl::cat(InventoryCategory));
llvm::cl::opt<std::string> ClangResourceDir(
    "clang-resource-dir", llvm::cl::desc("Clang builtin-header resource directory"),
    llvm::cl::value_desc("path"), llvm::cl::init(POSTGAMMA_CLANG_RESOURCE_DIR),
    llvm::cl::cat(InventoryCategory));

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

std::string canonicalType(QualType type, const LangOptions &language) {
  PrintingPolicy policy(language);
  policy.AnonymousTagLocations = false;
  policy.SuppressScope = false;
  return type.getCanonicalType().getAsString(policy);
}

struct FunctionRecord {
  std::string name;
  std::string header;
  unsigned line = 0;
  std::string return_type;
  std::string function_type;
  std::vector<std::string> parameters;
  bool variadic = false;
};

struct FieldRecord {
  std::string name;
  std::string type;
  uint64_t offset = 0;
};

struct RecordRecord {
  std::string name;
  std::string header;
  unsigned line = 0;
  bool complete = false;
  uint64_t size = 0;
  uint64_t alignment = 0;
  std::vector<FieldRecord> fields;
};

struct TypedefRecord {
  std::string name;
  std::string header;
  unsigned line = 0;
  std::string underlying_type;
};

class ApiCollector;

class ApiVisitor : public RecursiveASTVisitor<ApiVisitor> {
 public:
  ApiVisitor(ASTContext &context, ApiCollector &collector)
      : context_(context), collector_(collector) {}

  bool VisitFunctionDecl(FunctionDecl *declaration);
  bool VisitRecordDecl(RecordDecl *declaration);
  bool VisitTypedefNameDecl(TypedefNameDecl *declaration);

 private:
  ASTContext &context_;
  ApiCollector &collector_;
};

class ApiCollector {
 public:
  ApiCollector(std::string source_root, std::set<std::string> headers)
      : source_root_(std::move(source_root)), headers_(std::move(headers)) {}

  std::optional<std::pair<std::string, unsigned>> locate(
      const SourceManager &source_manager, SourceLocation location) const {
    if (location.isInvalid())
      return std::nullopt;
    SourceLocation spelling = source_manager.getSpellingLoc(location);
    std::string absolute = normalizeAbsolutePath(
        source_manager.getFilename(spelling));
    if (absolute.empty() || headers_.find(absolute) == headers_.end())
      return std::nullopt;
    auto relative = relativeToRoot(absolute, source_root_);
    PresumedLoc presumed = source_manager.getPresumedLoc(spelling);
    if (!relative || !presumed.isValid())
      return std::nullopt;
    return std::make_pair(*relative, presumed.getLine());
  }

  void addFunction(FunctionRecord record) {
    auto [found, inserted] = functions_.emplace(record.name, std::move(record));
    if (!inserted)
      conflicts_.insert(found->first);
  }

  void addRecord(RecordRecord record) {
    auto found = records_.find(record.name);
    if (found == records_.end()) {
      records_.emplace(record.name, std::move(record));
      return;
    }
    if (!found->second.complete && record.complete) {
      found->second = std::move(record);
      return;
    }
    if (found->second.complete == record.complete)
      return;
    conflicts_.insert(found->first);
  }

  void addTypedef(TypedefRecord record) {
    auto [found, inserted] = typedefs_.emplace(record.name, std::move(record));
    if (!inserted && found->second.underlying_type != record.underlying_type)
      conflicts_.insert(found->first);
  }

  bool validate() const {
    if (!conflicts_.empty()) {
      llvm::errs() << "postgamma-api-inventory: conflicting declarations:";
      for (const std::string &name : conflicts_)
        llvm::errs() << " " << name;
      llvm::errs() << "\n";
      return false;
    }
    if (functions_.empty() || records_.empty() || typedefs_.empty()) {
      llvm::errs() << "postgamma-api-inventory: public inventory is empty\n";
      return false;
    }
    for (const std::string &header : headers_) {
      bool found = false;
      auto relative = relativeToRoot(header, source_root_);
      for (const auto &[name, function] : functions_) {
        (void) name;
        if (relative && function.header == *relative) {
          found = true;
          break;
        }
      }
      if (!found) {
        llvm::errs() << "postgamma-api-inventory: header has no public function: "
                     << header << "\n";
        return false;
      }
    }
    return true;
  }

  bool write(llvm::StringRef output) const {
    llvm::json::Array functions;
    for (const auto &[name, function] : functions_) {
      llvm::json::Array parameters;
      for (const std::string &parameter : function.parameters)
        parameters.push_back(parameter);
      functions.push_back(llvm::json::Object{
          {"name", name},
          {"header", function.header},
          {"line", static_cast<int64_t>(function.line)},
          {"return_type", function.return_type},
          {"function_type", function.function_type},
          {"parameters", std::move(parameters)},
          {"variadic", function.variadic}});
    }

    llvm::json::Array records;
    int64_t complete_record_count = 0;
    for (const auto &[name, record] : records_) {
      llvm::json::Array fields;
      for (const FieldRecord &field : record.fields) {
        fields.push_back(llvm::json::Object{
            {"name", field.name},
            {"canonical_type", field.type},
            {"offset", static_cast<int64_t>(field.offset)}});
      }
      if (record.complete)
        complete_record_count++;
      records.push_back(llvm::json::Object{
          {"name", name},
          {"header", record.header},
          {"line", static_cast<int64_t>(record.line)},
          {"complete", record.complete},
          {"size", static_cast<int64_t>(record.size)},
          {"alignment", static_cast<int64_t>(record.alignment)},
          {"fields", std::move(fields)}});
    }

    llvm::json::Array typedefs;
    for (const auto &[name, type] : typedefs_) {
      typedefs.push_back(llvm::json::Object{
          {"name", name},
          {"header", type.header},
          {"line", static_cast<int64_t>(type.line)},
          {"underlying_type", type.underlying_type}});
    }

    llvm::json::Array headers;
    for (const std::string &header : headers_) {
      auto relative = relativeToRoot(header, source_root_);
      if (relative)
        headers.push_back(*relative);
    }
    llvm::json::Object document{
        {"schema_version", 1},
        {"kind", "postgamma.public-api-ast-catalog"},
        {"headers", std::move(headers)},
        {"functions", std::move(functions)},
        {"records", std::move(records)},
        {"typedefs", std::move(typedefs)},
        {"summary", llvm::json::Object{
             {"function_count", static_cast<int64_t>(functions_.size())},
             {"record_count", static_cast<int64_t>(records_.size())},
             {"complete_record_count", complete_record_count},
             {"typedef_count", static_cast<int64_t>(typedefs_.size())}}}};

    std::error_code error;
    llvm::raw_fd_ostream stream(output, error);
    if (error) {
      llvm::errs() << "postgamma-api-inventory: cannot write " << output
                   << ": " << error.message() << "\n";
      return false;
    }
    stream << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(document)));
    return true;
  }

 private:
  std::string source_root_;
  std::set<std::string> headers_;
  std::map<std::string, FunctionRecord> functions_;
  std::map<std::string, RecordRecord> records_;
  std::map<std::string, TypedefRecord> typedefs_;
  std::set<std::string> conflicts_;
};

bool ApiVisitor::VisitFunctionDecl(FunctionDecl *declaration) {
  if (declaration == nullptr || !declaration->getIdentifier() ||
      !declaration->getName().starts_with("pgm_"))
    return true;
  auto location = collector_.locate(context_.getSourceManager(),
                                    declaration->getLocation());
  if (!location)
    return true;
  FunctionRecord record;
  record.name = declaration->getNameAsString();
  record.header = location->first;
  record.line = location->second;
  record.return_type = canonicalType(declaration->getReturnType(),
                                     context_.getLangOpts());
  record.function_type = canonicalType(declaration->getType(),
                                       context_.getLangOpts());
  for (const ParmVarDecl *parameter : declaration->parameters())
    record.parameters.push_back(
        canonicalType(parameter->getType(), context_.getLangOpts()));
  record.variadic = declaration->isVariadic();
  collector_.addFunction(std::move(record));
  return true;
}

bool ApiVisitor::VisitRecordDecl(RecordDecl *declaration) {
  if (declaration == nullptr || !declaration->getIdentifier() ||
      !declaration->getName().starts_with("pgm_"))
    return true;
  auto location = collector_.locate(context_.getSourceManager(),
                                    declaration->getLocation());
  if (!location)
    return true;
  RecordRecord record;
  record.name = declaration->getNameAsString();
  record.header = location->first;
  record.line = location->second;
  record.complete = declaration->isCompleteDefinition();
  if (record.complete) {
    const ASTRecordLayout &layout = context_.getASTRecordLayout(declaration);
    record.size = layout.getSize().getQuantity();
    record.alignment = layout.getAlignment().getQuantity();
    unsigned index = 0;
    for (const FieldDecl *field : declaration->fields()) {
      record.fields.push_back(FieldRecord{
          field->getNameAsString(),
          canonicalType(field->getType(), context_.getLangOpts()),
          layout.getFieldOffset(index) / 8});
      index++;
    }
  }
  collector_.addRecord(std::move(record));
  return true;
}

bool ApiVisitor::VisitTypedefNameDecl(TypedefNameDecl *declaration) {
  if (declaration == nullptr || !declaration->getIdentifier() ||
      !declaration->getName().starts_with("pgm_"))
    return true;
  auto location = collector_.locate(context_.getSourceManager(),
                                    declaration->getLocation());
  if (!location)
    return true;
  collector_.addTypedef(TypedefRecord{
      declaration->getNameAsString(), location->first, location->second,
      canonicalType(declaration->getUnderlyingType(), context_.getLangOpts())});
  return true;
}

class InventoryConsumer : public ASTConsumer {
 public:
  InventoryConsumer(ASTContext &context, ApiCollector &collector)
      : visitor_(context, collector) {}

  void HandleTranslationUnit(ASTContext &context) override {
    visitor_.TraverseDecl(context.getTranslationUnitDecl());
  }

 private:
  ApiVisitor visitor_;
};

class InventoryAction : public ASTFrontendAction {
 public:
  explicit InventoryAction(ApiCollector &collector) : collector_(collector) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &compiler,
                                                 llvm::StringRef) override {
    return std::make_unique<InventoryConsumer>(compiler.getASTContext(),
                                               collector_);
  }

 private:
  ApiCollector &collector_;
};

class InventoryActionFactory : public FrontendActionFactory {
 public:
  explicit InventoryActionFactory(ApiCollector &collector)
      : collector_(collector) {}

  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<InventoryAction>(collector_);
  }

 private:
  ApiCollector &collector_;
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
    llvm::errs() << "postgamma-api-inventory: invalid source root\n";
    return 2;
  }
  std::set<std::string> headers;
  for (const std::string &value : PublicHeaders) {
    std::string header = normalizeAbsolutePath(value);
    if (header.empty() || !relativeToRoot(header, source_root)) {
      llvm::errs() << "postgamma-api-inventory: invalid public header: "
                   << value << "\n";
      return 2;
    }
    headers.insert(std::move(header));
  }

  ApiCollector collector(source_root, std::move(headers));
  ClangTool tool(options->getCompilations(), options->getSourcePathList());
  if (!ClangResourceDir.empty()) {
    CommandLineArguments resource_arguments{"-resource-dir=" +
                                             ClangResourceDir.getValue()};
    tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
        resource_arguments, ArgumentInsertPosition::BEGIN));
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
