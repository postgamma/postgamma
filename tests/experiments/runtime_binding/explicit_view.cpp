// Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0
// Diagnostic only: generate private implementations with an explicit view.
// This measures a binding-preserving call contract; it does not certify it.
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Attr.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace clang;
struct Edit { unsigned offset, length; std::string text; std::string file; };
struct Call { unsigned offset; std::string name; };
struct Function {
  unsigned begin, name, body, end;
  std::string symbol, arguments;
  bool local, returns_void;
  std::vector<Call> calls;
  unsigned indirect = 0;
  bool binding_boundary = false;
  std::set<std::string> body_identity;
  std::string file, source;
  bool header = false;
  bool inline_clone = false;
  unsigned prototype = 0;
  std::set<std::string> direct_calls;
  bool changes_binding = false;
};
static std::string Output, Report, Input;
static bool Valid = true;
static bool ValidatedEntry = false;
static bool CloneSafeOnly = false;
static bool BorrowedScope = false;
static bool InlineHeaders = false;

static std::string scopeMacros() {
  return "\n#pragma push_macro(\"POSTGAMMA_BACKEND_STATE_VALUE\")\n"
    "#pragma push_macro(\"POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE\")\n"
    "#undef POSTGAMMA_BACKEND_STATE_VALUE\n"
    "#undef POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE\n"
    "#define POSTGAMMA_BACKEND_STATE_VALUE(slot, original) "
    "POSTGAMMA_BACKEND_STATE_SCOPE_VALUE(postgamma_view, slot, original)\n"
    "#define POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE(slot, original, bridge) "
    "POSTGAMMA_BACKEND_STATE_SCOPE_RELOCATED_VALUE(postgamma_view, slot, original, bridge)\n";
}

static std::string restoreMacros() {
  return "\n#pragma pop_macro(\"POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE\")\n"
    "#pragma pop_macro(\"POSTGAMMA_BACKEND_STATE_VALUE\")\n";
}

class Consumer final : public ASTConsumer, public RecursiveASTVisitor<Consumer> {
  CompilerInstance &CI;
  std::vector<Function> Functions;
  Function *Current = nullptr;
  FileID CurrentFile;
  std::vector<Edit> &Macros;
public:
  Consumer(CompilerInstance &ci, std::vector<Edit> &macros): CI(ci), Macros(macros) {}
  bool TraverseFunctionDecl(FunctionDecl *decl) {
    auto &sm = CI.getSourceManager();
    auto *body = dyn_cast_or_null<CompoundStmt>(decl->getBody());
    bool header = !sm.isWrittenInMainFile(decl->getLocation());
    if (!decl->isThisDeclarationADefinition() || !body ||
        (header && (!InlineHeaders || !decl->isInlineSpecified() ||
                    decl->getStorageClass() != SC_Static || sm.isInSystemHeader(decl->getLocation()))) ||
        decl->getLocation().isMacroID() || decl->getBeginLoc().isMacroID())
      return true;
    if (decl->isVariadic() || decl->isNoReturn()) return true;
    Function f{sm.getFileOffset(decl->getBeginLoc()),
               sm.getFileOffset(decl->getLocation()),
               sm.getFileOffset(body->getLBracLoc()),
               sm.getFileOffset(body->getRBracLoc()) + 1,
               decl->getNameAsString(), "", decl->getStorageClass() == SC_Static,
               decl->getReturnType()->isVoidType(), {}, 0};
    CurrentFile = sm.getFileID(decl->getLocation());
    f.file = sm.getFilename(decl->getLocation()).str();
    f.header = header;
    f.inline_clone = decl->isInlineSpecified() && decl->getStorageClass() == SC_Static;
    if (header || f.inline_clone) f.source = sm.getBufferData(CurrentFile).str();
    auto prototype = decl->getCanonicalDecl()->getBeginLoc();
    f.prototype = prototype.isFileID() && sm.getFileID(prototype) == CurrentFile ?
      sm.getFileOffset(prototype) : f.begin;
    // Adding a leading parameter must not change positional attributes such
    // as nonnull(1), alloc_size(1), or format(...). Unknown attributes stay on
    // the original entry. These allowed attributes do not number parameters.
    if (header || f.inline_clone)
      for (const auto *attribute : decl->attrs())
        if (!isa<AlwaysInlineAttr, GNUInlineAttr, UnusedAttr, ArtificialAttr>(attribute))
          f.body_identity.insert("function attribute requires original signature");
    for (const auto *param : decl->parameters()) {
      if (param->getName().empty()) return true;
      if (!f.arguments.empty()) f.arguments += ", ";
      f.arguments += param->getNameAsString();
    }
    Current = &f;
    TraverseStmt(body);
    Current = nullptr;
    Functions.push_back(std::move(f));
    return true;
  }
  bool VisitVarDecl(VarDecl *decl) {
    if (Current && decl->isStaticLocal()) Current->body_identity.insert("static local");
    return true;
  }
  bool VisitStringLiteral(StringLiteral *) {
    if (Current) Current->body_identity.insert("string literal");
    return true;
  }
  bool VisitCompoundLiteralExpr(CompoundLiteralExpr *) {
    if (Current) Current->body_identity.insert("compound literal");
    return true;
  }
  bool VisitPredefinedExpr(PredefinedExpr *) {
    if (Current) Current->body_identity.insert("function identity");
    return true;
  }
  bool VisitSourceLocExpr(SourceLocExpr *) {
    if (Current) Current->body_identity.insert("source location identity");
    return true;
  }
  bool VisitIntegerLiteral(IntegerLiteral *literal) {
    if (Current && literal->getLocation().isMacroID()) {
      auto name = Lexer::getImmediateMacroName(literal->getLocation(),
        CI.getSourceManager(), CI.getLangOpts());
      if (name == "__LINE__" || name == "__COUNTER__" || name == "__INCLUDE_LEVEL__")
        Current->body_identity.insert("preprocessor location identity");
    }
    return true;
  }
  bool VisitAddrLabelExpr(AddrLabelExpr *) {
    if (Current) Current->body_identity.insert("label address");
    return true;
  }
  bool VisitCallExpr(CallExpr *call) {
    if (!Current) return true;
    auto &sm = CI.getSourceManager();
    const auto *callee = call->getDirectCallee();
    if (!callee) { ++Current->indirect; return true; }
    const auto name = callee->getNameAsString();
    Current->direct_calls.insert(name);
    if (name == "__builtin_return_address" || name == "__builtin_frame_address")
      Current->body_identity.insert("caller identity");
    if (name == "postgamma_execution_context_bind" ||
        name == "postgamma_execution_context_restore" ||
        name == "setjmp" || name == "_setjmp" || name == "__sigsetjmp" ||
        name == "longjmp" || name == "siglongjmp")
      Current->binding_boundary = Current->changes_binding = true;
    auto *ref = dyn_cast<DeclRefExpr>(call->getCallee()->IgnoreParenImpCasts());
    if (ref && ref->getLocation().isFileID() && sm.getFileID(ref->getLocation()) == CurrentFile)
      Current->calls.push_back({sm.getFileOffset(ref->getLocation()), callee->getNameAsString()});
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr *reference) {
    if (!Current || reference->getLocation().isMacroID()) return true;
    if (const auto *variable = dyn_cast<VarDecl>(reference->getDecl()))
      if (variable->getTLSKind() != VarDecl::TLS_None)
        Current->binding_boundary = true;
    const auto name = reference->getDecl()->getNameAsString();
    if (name == "PostgammaCurrentPreparedRoleState" ||
        name == "PostgammaCurrentExecutionContext" ||
        name == "PostgammaCurrentBackendStateAddresses")
      Current->binding_boundary = true;
    return true;
  }
  bool bindingVariable(const Expr *expression) {
    const auto *reference = dyn_cast<DeclRefExpr>(expression->IgnoreParenImpCasts());
    if (!reference) return false;
    if (const auto *variable = dyn_cast<VarDecl>(reference->getDecl()))
      if (variable->getTLSKind() != VarDecl::TLS_None) return true;
    const auto name = reference->getDecl()->getNameAsString();
    return name == "PostgammaCurrentPreparedRoleState" ||
      name.rfind("PostgammaPreparedRoleState_", 0) == 0 ||
      name == "PostgammaCurrentExecutionContext" ||
      name == "PostgammaCurrentBackendStateAddresses";
  }
  bool VisitBinaryOperator(BinaryOperator *operation) {
    if (Current && operation->isAssignmentOp() && bindingVariable(operation->getLHS()))
      Current->binding_boundary = Current->changes_binding = true;
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *operation) {
    if (Current && (operation->getOpcode() == UO_AddrOf || operation->isIncrementDecrementOp()) &&
        bindingVariable(operation->getSubExpr()))
      Current->binding_boundary = Current->changes_binding = true;
    return true;
  }
  void HandleTranslationUnit(ASTContext &ctx) override {
    TraverseDecl(ctx.getTranslationUnitDecl());
    if (CI.getDiagnostics().hasErrorOccurred()) { Valid = false; return; }
    // A wrapper around a known binding/nonlocal-exit boundary is a boundary
    // too. A raw TLS reader retains its own original body, but does not mutate
    // its caller's binding. Do not infer restoration merely because a mutating
    // primitive is one call further down the local graph. Unknown external/indirect edges remain
    // explicitly unqualified in the diagnostic report.
    std::set<std::string> bindingFunctions;
    for (const auto &f : Functions)
      if (f.changes_binding) bindingFunctions.insert(f.symbol);
    bool boundaryChanged;
    do {
      boundaryChanged = false;
      for (auto &f : Functions) {
        if (f.changes_binding) continue;
        for (const auto &name : f.direct_calls)
          if (bindingFunctions.count(name)) {
            f.binding_boundary = f.changes_binding = true;
            bindingFunctions.insert(f.symbol);
            boundaryChanged = true;
            break;
          }
      }
    } while (boundaryChanged);
    auto source = CI.getSourceManager().getBufferData(CI.getSourceManager().getMainFileID()).str();
    if (BorrowedScope) {
      // Preserve the function, ABI, static locals and predefined identities.
      // Binding/nonlocal-exit functions are boundaries, not borrowed scopes.
      std::vector<Edit> edits;
      llvm::json::Array facts, boundaries;
      auto accesses = [&](const Function &f) {
        return std::any_of(Macros.begin(), Macros.end(), [&](const Edit &m) {
          return m.file == f.file && m.offset > f.body && m.offset < f.end;
        });
      };
      // Only inline header bodies with no observable object identity may be
      // copied. The ABI/address-taken original always remains available.
      std::map<std::string, const Function *> headers;
      llvm::json::Array lateHeaders;
      auto firstMain = std::find_if(Functions.begin(), Functions.end(),
        [](const Function &f) { return !f.header; });
      // Some generated .c includes introduce headers after function bodies.
      // Their types/macros are not visible at the early helper insertion point.
      // Leave those helpers on their original path rather than moving includes.
      for (auto it = firstMain; it != Functions.end(); ++it)
        if (it->header) lateHeaders.push_back(it->symbol);
      bool changed;
      do {
        changed = false;
        for (auto it = Functions.begin(); it != Functions.end(); ++it) {
          const auto &f = *it;
          if ((!f.header && (!InlineHeaders || !f.inline_clone)) ||
              (f.header && it >= firstMain) || f.binding_boundary ||
              !f.body_identity.empty() || headers.count(f.symbol)) continue;
          bool selected = accesses(f);
          for (const auto &c : f.calls) selected |= headers.count(c.name) != 0;
          if (selected) { headers[f.symbol] = &f; changed = true; }
        }
      } while (changed);
      auto replaceCalls = [&](const Function &f, std::vector<Edit> &target) {
        for (const auto &c : f.calls) {
          if (!headers.count(c.name)) continue;
          const auto &text = f.header ? f.source : source;
          auto paren = text.find('(', c.offset + c.name.size());
          auto first = text.find_first_not_of(" \t\r\n", paren + 1);
          if (paren == std::string::npos || paren >= f.end || first == std::string::npos) {
            Valid = false; return;
          }
          target.push_back({c.offset, unsigned(c.name.size()), "postgamma_view_inline_" + c.name});
          target.push_back({unsigned(paren + 1), 0, text[first] == ')' ? "postgamma_view" : "postgamma_view, "});
        }
      };
      auto apply = [&](std::string &text, std::vector<Edit> &target, unsigned origin) {
        std::sort(target.begin(), target.end(), [](const Edit &a, const Edit &b) {
          return a.offset > b.offset;
        });
        unsigned boundary = text.size() + origin;
        for (const auto &edit : target) {
          if (edit.offset < origin || edit.offset + edit.length > boundary) { Valid = false; return; }
          text.replace(edit.offset - origin, edit.length, edit.text); boundary = edit.offset;
        }
      };
      std::string prototypes, implementations;
      std::map<std::string, std::string> mainImplementations;
      llvm::json::Array headerFacts;
      for (const auto &[name, fp] : headers) {
        const auto &f = *fp;
        std::string signature = f.source.substr(f.begin, f.body - f.begin);
        auto pos = f.name - f.begin;
        if (signature.substr(pos, name.size()) != name) { Valid = false; return; }
        signature.replace(pos, name.size(), "postgamma_view_inline_" + name);
        auto open = signature.find('(', pos), close = signature.rfind(')');
        if (open == std::string::npos || close == std::string::npos) { Valid = false; return; }
        if (f.arguments.empty()) signature.replace(open + 1, close - open - 1, "unsigned char *postgamma_view");
        else signature.insert(open + 1, "unsigned char *postgamma_view, ");
        if (f.header) prototypes += signature + ";\n";
        else edits.push_back({f.prototype, 0, signature + ";\n"});
        std::string body = f.source.substr(f.body, f.end - f.body);
        std::vector<Edit> bodyEdits;
        replaceCalls(f, bodyEdits);
        bodyEdits.push_back({f.body + 1, 0, scopeMacros()});
        apply(body, bodyEdits, f.body);
        auto implementation = signature + body + restoreMacros() + "\n";
        if (f.header) implementations += implementation;
        else mainImplementations[name] = implementation;
        llvm::json::Array external;
        for (const auto &name : f.direct_calls)
          if (!headers.count(name)) external.push_back(name);
        headerFacts.push_back(llvm::json::Object{{"function", name}, {"file", f.file},
          {"origin", f.header ? "header" : "main_file_inline"},
          {"indirect_calls", f.indirect}, {"external_calls", std::move(external)}});
      }
      unsigned firstSelected = source.size();
      for (const auto &f : Functions) {
        if (f.header) continue;
        if (f.binding_boundary) { boundaries.push_back(f.symbol); continue; }
        bool selected = accesses(f);
        for (const auto &c : f.calls) selected |= headers.count(c.name) != 0;
        if (!selected) continue;
        firstSelected = std::min(firstSelected, f.begin);
        if (source.substr(f.body, f.end - f.body).find("postgamma_view") != std::string::npos) {
          llvm::errs() << "borrow name collision in " << f.symbol << "\n";
          Valid = false; return;
        }
        edits.push_back({f.body + 1, 0,
          "\n\tunsigned char *const postgamma_view = PostgammaCurrentPreparedRoleState;\n" + scopeMacros()});
        edits.push_back({f.end, 0, restoreMacros() + mainImplementations[f.symbol]});
        replaceCalls(f, edits);
        llvm::json::Array calls;
        for (const auto &name : f.direct_calls) calls.push_back(name);
        facts.push_back(llvm::json::Object{{"function", f.symbol},
          {"indirect_calls", f.indirect}, {"external_calls", std::move(calls)}});
      }
      if (firstSelected < source.size()) edits.push_back({firstSelected, 0, prototypes + implementations});
      apply(source, edits, 0);
      if (!Valid) return;
      std::error_code error;
      llvm::raw_fd_ostream output(Output, error);
      if (error) { Valid = false; return; }
      output << source;
      llvm::raw_fd_ostream report(Report, error);
      if (error) { Valid = false; return; }
      report << llvm::formatv("{0:2}\n", llvm::json::Value(llvm::json::Object{
        {"status", "diagnostic_only"}, {"input", Input}, {"borrowed_scope", true},
        {"contract", "Returning callees must restore bindings; unknown calls remain unqualified"},
        {"excluded_binding_boundaries", std::move(boundaries)},
        {"inline_headers", InlineHeaders}, {"header_functions", std::move(headerFacts)},
        {"excluded_late_headers", std::move(lateHeaders)},
        {"functions", std::move(facts)}}));
      return;
    }
    auto eligible = [](const Function &f) {
      return !f.binding_boundary && (!CloneSafeOnly || f.body_identity.empty());
    };
    std::map<std::string, Function *> selected;
    for (auto &f : Functions)
      for (const auto &m : Macros)
        if (eligible(f) && m.offset > f.body && m.offset < f.end) selected[f.symbol] = &f;
    bool changed;
    do {
      changed = false;
      for (auto &f : Functions) {
        if (!eligible(f) || selected.count(f.symbol)) continue;
        for (const auto &c : f.calls)
          if (selected.count(c.name)) { selected[f.symbol] = &f; changed = true; break; }
      }
    } while (changed);
    std::vector<Edit> edits;
    std::string prototypes;
    llvm::json::Array facts;
    llvm::json::Array boundaries;
    llvm::json::Array identities;
    for (const auto &f : Functions)
      if (f.binding_boundary) boundaries.push_back(f.symbol);
    if (CloneSafeOnly)
      for (const auto &f : Functions)
        if (!f.body_identity.empty()) {
          llvm::json::Array reasons;
          for (const auto &reason : f.body_identity) reasons.push_back(reason);
          identities.push_back(llvm::json::Object{{"function", f.symbol},
                                                 {"reasons", std::move(reasons)}});
        }
    for (const auto &[name, fp] : selected) {
      auto &f = *fp;
      std::string header = source.substr(f.begin, f.body - f.begin);
      auto pos = f.name - f.begin;
      if (header.substr(pos, name.size()) != name) { Valid = false; return; }
      std::string impl = header;
      impl.replace(pos, name.size(), "postgamma_view_impl_" + name);
      auto open = impl.find('(', pos);
      if (open == std::string::npos) { Valid = false; return; }
      auto close = impl.rfind(')');
      if (close == std::string::npos) { Valid = false; return; }
      std::string parameter = "unsigned char *postgamma_view __attribute__((unused))";
      if (f.arguments.empty()) impl.replace(open + 1, close - open - 1, parameter);
      else impl.insert(open + 1, parameter + ", ");
      if (!f.local) impl.insert(0, "static ");
      if (ValidatedEntry) impl.insert(0, "__attribute__((nonnull(1))) ");
      prototypes += impl + ";\n";
      std::string wrapper = (f.local ? "__attribute__((unused)) " : "") + header + "{\n\t";
      if (ValidatedEntry) {
        std::string fallback = header;
        fallback.replace(pos, name.size(), "postgamma_view_unbound_" + name);
        if (!f.local) fallback.insert(0, "static ");
        fallback.insert(0, "__attribute__((cold)) ");
        prototypes += fallback + ";\n";
        wrapper += "unsigned char *postgamma_view = PostgammaCurrentPreparedRoleState;\n\n\t";
        wrapper += "if (postgamma_view == NULL)\n\t{\n\t\t";
        if (!f.returns_void) wrapper += "return ";
        wrapper += "postgamma_view_unbound_" + name + "(" + f.arguments + ");\n";
        if (f.returns_void) wrapper += "\t\treturn;\n";
        wrapper += "\t}\n\t";
        // Keep the unbound body and all its call sites unchanged. In particular,
        // no access-free path may acquire a new binding precondition.
        edits.push_back({f.end, 0, "\n\n" + fallback +
                         source.substr(f.body, f.end - f.body) + "\n"});
      }
      if (!f.returns_void) wrapper += "return ";
      wrapper += "postgamma_view_impl_" + name + "(" +
                 (ValidatedEntry ? "postgamma_view" : "PostgammaCurrentPreparedRoleState");
      if (!f.arguments.empty()) wrapper += ", " + f.arguments;
      wrapper += ");\n}\n\n" + impl;
      edits.push_back({f.begin, f.body - f.begin, wrapper});
      for (const auto &m : Macros)
        if (m.offset > f.body && m.offset < f.end) edits.push_back(m);
      std::set<unsigned> calls;
      llvm::json::Array external;
      for (const auto &c : f.calls) {
        if (!selected.count(c.name)) { external.push_back(c.name); continue; }
        if (!calls.insert(c.offset).second) continue;
        auto end = c.offset + c.name.size();
        auto paren = source.find('(', end);
        if (paren == std::string::npos || paren >= f.end) { Valid = false; return; }
        auto first = source.find_first_not_of(" \t\r\n", paren + 1);
        edits.push_back({c.offset, unsigned(c.name.size()), "postgamma_view_impl_" + c.name});
        edits.push_back({unsigned(paren + 1), 0, source[first] == ')' ? "postgamma_view" : "postgamma_view, "});
      }
      facts.push_back(llvm::json::Object{{"function", name}, {"indirect_calls", f.indirect},
                      {"external_calls", std::move(external)}});
    }
    if (!selected.empty()) {
      unsigned first = source.size();
      for (const auto &[name, f] : selected) first = std::min(first, f->begin);
      edits.push_back({first, 0, prototypes + "\n"});
    }
    std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) {
      return a.offset > b.offset || (a.offset == b.offset && a.length > b.length);
    });
    unsigned boundary = source.size();
    for (const auto &e : edits) {
      if (e.offset + e.length > boundary) { Valid = false; return; }
      source.replace(e.offset, e.length, e.text); boundary = e.offset;
    }
    std::error_code error;
    llvm::raw_fd_ostream file(Output, error);
    if (error) { Valid = false; return; }
    file << source;
    llvm::raw_fd_ostream report(Report, error);
    if (error) { Valid = false; return; }
    report << llvm::formatv("{0:2}\n", llvm::json::Value(llvm::json::Object{
      {"status", "diagnostic_only"}, {"input", Input},
      {"validated_entry", ValidatedEntry},
      {"clone_safe_only", CloneSafeOnly},
      {"contract", "Borrowed views require binding restoration by returning callees; not callgraph-certified"},
      {"excluded_binding_boundaries", std::move(boundaries)},
      {"excluded_body_identity", std::move(identities)},
      {"functions", std::move(facts)}}));
  }
};

class Macros final : public PPCallbacks {
  SourceManager &SM;
  std::vector<Edit> &Edits;
public:
  Macros(SourceManager &sm, std::vector<Edit> &edits): SM(sm), Edits(edits) {}
  void MacroExpands(const Token &token, const MacroDefinition &, SourceRange range,
                    const MacroArgs *) override {
    auto location = BorrowedScope ? SM.getExpansionLoc(range.getBegin()) : range.getBegin();
    if (location.isMacroID() || (!SM.isWrittenInMainFile(location) && !InlineHeaders)) return;
    const auto *id = token.getIdentifierInfo();
    if (!id) return;
    std::string name = id->getName().str();
    if (name != "POSTGAMMA_BACKEND_STATE_VALUE" &&
        name != "POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE") return;
    auto offset = SM.getFileOffset(location);
    if (BorrowedScope) {
      Edits.push_back({offset, 0, "", SM.getFilename(location).str()});
      return;
    }
    auto source = SM.getBufferData(SM.getMainFileID());
    auto paren = source.find('(', offset + name.size());
    std::string replacement = name + "_IN";
    Edits.push_back({offset, unsigned(paren - offset + 1), replacement + "(postgamma_view, "});
  }
};
class Action final : public ASTFrontendAction {
  std::vector<Edit> Edits;
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &ci, llvm::StringRef) override {
    ci.getPreprocessor().addPPCallbacks(std::make_unique<Macros>(ci.getSourceManager(), Edits));
    return std::make_unique<Consumer>(ci, Edits);
  }
};
int main(int argc, char **argv) {
  if (argc < 5) return 2;
  Input = argv[1]; Output = argv[2]; Report = argv[3];
  auto buffer = llvm::MemoryBuffer::getFile(Input);
  if (!buffer) return 2;
  std::vector<std::string> args;
  for (int i = 4; i < argc; ++i)
    if (std::string(argv[i]) == "--validated-entry") ValidatedEntry = true;
    else if (std::string(argv[i]) == "--clone-safe-only") CloneSafeOnly = true;
    else if (std::string(argv[i]) == "--borrowed-scope") BorrowedScope = true;
    else if (std::string(argv[i]) == "--inline-headers") InlineHeaders = true;
    else args.emplace_back(argv[i]);
  CloneSafeOnly |= ValidatedEntry;
  if (BorrowedScope && (ValidatedEntry || CloneSafeOnly)) return 2;
  if (InlineHeaders && !BorrowedScope) return 2;
  return tooling::runToolOnCodeWithArgs(std::make_unique<Action>(),
      buffer.get()->getBuffer(), args, Input) && Valid ? 0 : 1;
}
