#include "../src/ir/ConditionalCFG.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

namespace {
bool check(bool value, llvm::StringRef message) {
  if (!value) llvm::errs() << "conditional CFG test: " << message << '\n';
  return value;
}
std::string text(const llvm::Module &module) {
  std::string result;
  llvm::raw_string_ostream stream(result);
  module.print(stream, nullptr);
  return result;
}
std::string replace(std::string value, llvm::StringRef before, llvm::StringRef after) {
  auto position = value.find(before.str());
  if (position == std::string::npos) llvm_unreachable("missing CFG fixture mutation");
  value.replace(position, before.size(), after.str());
  return value;
}
std::string replaceAll(std::string value, llvm::StringRef before, llvm::StringRef after) {
  size_t position = 0;
  while ((position = value.find(before.str(), position)) != std::string::npos) {
    value.replace(position, before.size(), after.str());
    position += after.size();
  }
  return value;
}
std::string fixture(bool extra, bool chain = false, unsigned label = 8) {
  std::string source =
      "declare void @observe(i32)\n"
      "define i32 @sample(i32 %value, ptr %storage) {\n"
      "entry:\n switch i32 %value, label %fallback [\n"
      "  i32 1, label %one\n";
  if (extra) source += "  i32 " + std::to_string(label) + ", label %extra\n";
  source +=
      "  i32 2, label %two\n ]\n"
      "one:\n store i32 10, ptr %storage\n br label %join\n";
  if (extra) {
    source += "extra:\n %adjusted = add i32 %value, 7\n";
    if (chain) source += " br label %continuation\ncontinuation:\n";
    source += " call void @observe(i32 %adjusted)\n"
              " store volatile i32 %adjusted, ptr %storage\n br label %join\n";
  }
  source +=
      "two:\n store i32 20, ptr %storage\n br label %join\n"
      "fallback:\n store i32 30, ptr %storage\n br label %join\n"
      "join:\n %result = load i32, ptr %storage\n ret i32 %result\n}\n";
  return source;
}

bool projection(const sela::detail::ConditionalCFG &result,
                const llvm::Function &function, bool left) {
  auto original = function.begin();
  bool passed = true;
  const auto &mapping = left ? result.leftBlocks : result.rightBlocks;
  for (unsigned index = 0; index < result.blocks.size(); ++index) {
    const auto &pair = result.blocks[index];
    const auto *block = left ? pair.left : pair.right;
    passed &= check(bool(block) == bool(pair.domain & (left ? 1 : 2)), "block domain agrees with original");
    if (!block) continue;
    passed &= check(original != function.end() && &*original == block,
                    "filtered block order is exactly native order");
    if (original == function.end()) return false;
    ++original;
    passed &= check(mapping.lookup(block) == index, "block map has exact shared index");
  }
  passed &= check(original == function.end(), "projection includes every native block");
  for (const auto &pair : result.switches) {
    auto *instruction = left ? pair.left : pair.right;
    auto *target = left ? result.blocks[pair.defaultSuccessor].left
                        : result.blocks[pair.defaultSuccessor].right;
    passed &= check(target == instruction->getDefaultDest(), "default successor preserved");
    unsigned ordinal = 0;
    for (const auto &entry : pair.cases) {
      auto *constant = left ? entry.left : entry.right;
      auto nativeIndex = left ? entry.leftIndex : entry.rightIndex;
      passed &= check(bool(constant) == bool(entry.domain & (left ? 1 : 2)), "case domain agrees with original");
      if (!constant) continue;
      passed &= check(nativeIndex && *nativeIndex == ordinal, "filtered case order is exactly native order");
      auto nativeCase = instruction->case_begin() + ordinal++;
      auto *destination = left ? result.blocks[entry.successor].left : result.blocks[entry.successor].right;
      passed &= check(constant == nativeCase->getCaseValue() &&
                          destination == nativeCase->getCaseSuccessor(),
                      "native case value and edge are preserved");
    }
    passed &= check(ordinal == instruction->getNumCases(), "projection includes every native case");
  }
  return passed;
}

bool test(llvm::StringRef name, const std::string &a, const std::string &b,
          bool accepted, unsigned privateBlocks = 0) {
  llvm::LLVMContext leftContext, rightContext;
  llvm::SMDiagnostic diagnostic;
  auto left = llvm::parseAssemblyString(a, diagnostic, leftContext);
  if (!left) { diagnostic.print("left CFG fixture", llvm::errs()); return false; }
  auto right = llvm::parseAssemblyString(b, diagnostic, rightContext);
  if (!right) { diagnostic.print("right CFG fixture", llvm::errs()); return false; }
  if (llvm::verifyModule(*left, &llvm::errs()) || llvm::verifyModule(*right, &llvm::errs())) return false;
  auto beforeLeft = text(*left), beforeRight = text(*right);
  auto *leftFunction = left->getFunction("sample"), *rightFunction = right->getFunction("sample");
  auto result = sela::detail::pairConditionalCFG(*leftFunction, *rightFunction);
  bool passed = check(bool(result) == accepted, name);
  if (!result) {
    if (accepted) llvm::logAllUnhandledErrors(result.takeError(), llvm::errs());
    else llvm::consumeError(result.takeError());
  } else {
    passed &= projection(*result, *leftFunction, true);
    passed &= projection(*result, *rightFunction, false);
    unsigned actualPrivate = 0;
    for (const auto &block : result->blocks) actualPrivate += block.domain != 3;
    passed &= check(actualPrivate == privateBlocks, "only proved arms are one-sided");
    passed &= check(result->conditional == bool(actualPrivate), "conditional flag agrees with graph");
  }
  passed &= check(text(*left) == beforeLeft && text(*right) == beforeRight,
                  "both inputs remain byte-for-byte unchanged");
  return passed;
}

bool nativePair(llvm::StringRef leftPath, llvm::StringRef rightPath) {
  llvm::LLVMContext leftContext, rightContext;
  llvm::SMDiagnostic diagnostic;
  auto left = llvm::parseIRFile(leftPath, diagnostic, leftContext);
  if (!left) { diagnostic.print("native CFG left", llvm::errs()); return false; }
  auto right = llvm::parseIRFile(rightPath, diagnostic, rightContext);
  if (!right) { diagnostic.print("native CFG right", llvm::errs()); return false; }
  unsigned differing = 0;
  for (auto &function : *left) {
    auto *other = right->getFunction(function.getName());
    if (function.empty() || !other || function.size() == other->size()) continue;
    auto result = sela::detail::pairConditionalCFG(function, *other);
    if (!result) {
      llvm::errs() << "native function " << function.getName() << ": ";
      llvm::logAllUnhandledErrors(result.takeError(), llvm::errs()); return false;
    }
    if (!projection(*result, function, true) || !projection(*result, *other, false)) return false;
    unsigned arms = 0;
    for (const auto &block : result->blocks) arms += block.domain != 3;
    llvm::outs() << function.getName() << ": " << arms << " conditional blocks\n";
    ++differing;
  }
  return check(differing == 2, "real capture has exactly two differing CFGs");
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 3) return nativePair(argv[1], argv[2]) ? 0 : 1;
  if (argc != 1) return 2;
  const auto plain = fixture(false), wide = fixture(true), chain = fixture(true, true);
  bool passed = test("identical graphs", plain, plain, true);
  passed &= test("wide straight arm", wide, plain, true, 1);
  passed &= test("narrow straight arm", plain, wide, true, 1);
  passed &= test("wide closed chain", chain, plain, true, 2);
  passed &= test("distinct arms in both domains", wide, fixture(true, true, 16), true, 3);
  passed &= test("case hits shared block", replace(wide, "label %extra\n", "label %two\n"), plain, false);
  passed &= test("one arm has two case edges", replace(wide, "i32 8, label %extra", "i32 8, label %extra\n i32 9, label %extra"), plain, false);
  passed &= test("private return is not a rejoin", replace(wide, "store volatile i32 %adjusted, ptr %storage\n br label %join", "store volatile i32 %adjusted, ptr %storage\n ret i32 %adjusted"), plain, false);
  passed &= test("private conditional branch", replace(wide, "store volatile i32 %adjusted, ptr %storage\n br label %join", "store volatile i32 %adjusted, ptr %storage\n br i1 true, label %join, label %extra"), plain, false);
  passed &= test("private unreachable is not a rejoin", replace(wide, "store volatile i32 %adjusted, ptr %storage\n br label %join", "store volatile i32 %adjusted, ptr %storage\n unreachable"), plain, false);
  passed &= test("private cycle", replace(chain, "store volatile i32 %adjusted, ptr %storage\n br label %join", "store volatile i32 %adjusted, ptr %storage\n br label %extra"), plain, false);
  passed &= test("common case order reversed", replace(plain, "i32 1, label %one\n  i32 2, label %two", "i32 2, label %two\n  i32 1, label %one"), plain, false);
  passed &= test("default correspondence conflicts", replace(wide, "label %fallback [", "label %one ["), plain, false);
  passed &= test("shared branch correspondence conflicts", replace(wide, "store i32 20, ptr %storage\n br label %join", "store i32 20, ptr %storage\n br label %fallback"), plain, false);
  passed &= test("unreachable unpaired block", replace(wide, "join:\n", "dead:\n ret i32 0\njoin:\n"), plain, false);
  passed &= test("block address escapes", "@address = global ptr blockaddress(@sample, %extra)\n" + wide, plain, false);
  auto phiLeft = replace(wide, "%result = load i32, ptr %storage", "%result = phi i32 [10, %one], [20, %two], [30, %fallback], [%adjusted, %extra]");
  auto phiRight = replace(plain, "%result = load i32, ptr %storage", "%result = phi i32 [10, %one], [20, %two], [30, %fallback]");
  passed &= test("conditional PHI rejected", phiLeft, phiRight, false);
  passed &= test("unchanged shared PHI retained", phiRight, phiRight, true);
  passed &= test("private PHI rejected", replace(wide, "extra:\n %adjusted", "extra:\n %local = phi i32 [%value, %entry]\n %adjusted"), plain, false);
  passed &= test("native label width differs", replaceAll(wide, "i32", "i64"), plain, false);
  passed &= test("unqualified large label width", replaceAll(plain, "i32", "i128"), replaceAll(plain, "i32", "i128"), false);
  passed &= test("wide labels retain exact bits", replaceAll(wide, "i32", "i64"), replaceAll(plain, "i32", "i64"), true, 1);
  auto renamed = plain;
  while (renamed.find("one") != std::string::npos) renamed = replace(renamed, "one", "renamed_arm");
  passed &= test("block names ignored", plain, renamed, true);
  auto reordered = replace(plain,
      "one:\n store i32 10, ptr %storage\n br label %join\ntwo:\n store i32 20, ptr %storage\n br label %join\n",
      "two:\n store i32 20, ptr %storage\n br label %join\none:\n store i32 10, ptr %storage\n br label %join\n");
  passed &= test("common block order reversed", plain, reordered, false);
  const std::string branch =
      "define i32 @sample(i1 %condition) {\nentry:\n br i1 %condition, label %a, label %b\na:\n ret i32 1\nb:\n ret i32 2\n}\n";
  passed &= test("branch orientation preserved", branch, branch, true);
  passed &= test("branch orientation reversal rejected", branch,
      replace(branch, "label %a, label %b", "label %b, label %a"), false);
  const std::string loop =
      "define i32 @sample(i1 %condition) {\nentry:\n br label %loop\nloop:\n br i1 %condition, label %loop, label %exit\nexit:\n ret i32 0\n}\n";
  passed &= test("shared loop preserved", loop, loop, true);
  const std::string duplicateTargets =
      "define i32 @sample(i32 %value) {\nentry:\n switch i32 %value, label %exit [i32 1, label %exit\n i32 2, label %exit]\nexit:\n ret i32 0\n}\n";
  passed &= test("shared duplicate switch targets preserved", duplicateTargets, duplicateTargets, true);
  return passed ? 0 : 1;
}
