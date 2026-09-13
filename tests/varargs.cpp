#include "../src/ir/Varargs.h"
#include "sela/Targets.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

namespace {
const char *wide = R"llvm(
%Va = type { i32, i32, ptr, ptr }
declare void @llvm.va_start(ptr)
declare void @llvm.va_end(ptr)
declare void @observe(i32)
define i32 @take(i32 %fixed, ...) {
entry:
  %state = alloca [1 x %Va], align 8
  %decay = getelementptr inbounds [1 x %Va], ptr %state, i64 0, i64 0
  call void @llvm.va_start(ptr %decay)
  %position = getelementptr inbounds %Va, ptr %decay, i32 0, i32 0
  %offset = load i32, ptr %position, align 8
  %available = icmp ule i32 %offset, 40
  br i1 %available, label %registers, label %overflow
registers:
  %saveaddr = getelementptr inbounds %Va, ptr %decay, i32 0, i32 3
  %save = load ptr, ptr %saveaddr, align 8
  %address = getelementptr i8, ptr %save, i32 %offset
  %increment = add i32 %offset, 8
  store i32 %increment, ptr %position, align 8
  br label %join
overflow:
  %cursoraddr = getelementptr inbounds %Va, ptr %decay, i32 0, i32 2
  %cursor = load ptr, ptr %cursoraddr, align 8
  %next = getelementptr i8, ptr %cursor, i32 8
  store ptr %next, ptr %cursoraddr, align 8
  br label %join
join:
  %selected = phi ptr [ %address, %registers ], [ %cursor, %overflow ]
  %result = load i32, ptr %selected, align 4
  call void @llvm.va_end(ptr %decay)
  ret i32 %result
}
)llvm";
const char *narrow = R"llvm(
declare void @llvm.va_start(ptr)
declare void @llvm.va_end(ptr)
@observed = global ptr null
define i32 @take(i32 %fixed, ...) {
entry:
  %state = alloca ptr, align 4
  call void @llvm.va_start(ptr %state)
  %cursor = load ptr, ptr %state, align 4
  %next = getelementptr inbounds i8, ptr %cursor, i32 4
  store ptr %next, ptr %state, align 4
  %result = load i32, ptr %cursor, align 4
  call void @llvm.va_end(ptr %state)
  ret i32 %result
}
)llvm";
const char *armAligned = R"llvm(
%Va = type { ptr }
declare void @llvm.va_start(ptr)
declare void @llvm.va_end(ptr)
declare ptr @llvm.ptrmask.p0.i32(ptr, i32)
@observed = global ptr null
define i64 @take(i32 %fixed, ...) {
entry:
  %state = alloca %Va, align 4
  call void @llvm.va_start(ptr %state)
  %cursor = load ptr, ptr %state, align 4
  %rounded = getelementptr inbounds i8, ptr %cursor, i32 7
  %aligned = call ptr @llvm.ptrmask.p0.i32(ptr %rounded, i32 -8)
  %next = getelementptr inbounds i8, ptr %aligned, i32 8
  store ptr %next, ptr %state, align 4
  %result = load i64, ptr %aligned, align 8
  call void @llvm.va_end(ptr %state)
  ret i64 %result
}
)llvm";
const char *armWide = R"llvm(
%Va = type { ptr, ptr, ptr, i32, i32 }
declare void @llvm.va_start(ptr)
declare void @llvm.va_end(ptr)
declare void @observe(i32)
define i32 @take(i32 %fixed, ...) {
entry:
  %state = alloca %Va, align 8
  call void @llvm.va_start(ptr %state)
  %position = getelementptr inbounds %Va, ptr %state, i32 0, i32 3
  %offset = load i32, ptr %position, align 8
  %empty = icmp sge i32 %offset, 0
  br i1 %empty, label %overflow, label %check
check:
  %nextoff = add i32 %offset, 8
  store i32 %nextoff, ptr %position, align 8
  %available = icmp sle i32 %nextoff, 0
  br i1 %available, label %registers, label %overflow
registers:
  %saveaddr = getelementptr inbounds %Va, ptr %state, i32 0, i32 1
  %save = load ptr, ptr %saveaddr, align 8
  %address = getelementptr inbounds i8, ptr %save, i32 %offset
  br label %join
overflow:
  %stackaddr = getelementptr inbounds %Va, ptr %state, i32 0, i32 0
  %cursor = load ptr, ptr %stackaddr, align 8
  %next = getelementptr inbounds i8, ptr %cursor, i64 8
  store ptr %next, ptr %stackaddr, align 8
  br label %join
join:
  %selected = phi ptr [ %address, %registers ], [ %cursor, %overflow ]
  %result = load i32, ptr %selected, align 8
  call void @llvm.va_end(ptr %state)
  ret i32 %result
}
)llvm";
const char *armForward = R"llvm(
%Va = type { ptr }
declare void @llvm.va_start(ptr)
declare void @llvm.va_end(ptr)
declare void @sink([1 x i32])
define void @forward(i32 %fixed, ...) {
  %state = alloca %Va, align 4
  call void @llvm.va_start(ptr %state)
  %field = getelementptr inbounds %Va, ptr %state, i32 0, i32 0
  %value = load [1 x i32], ptr %field, align 4
  call void @sink([1 x i32] %value)
  call void @llvm.va_end(ptr %state)
  ret void
}
)llvm";
const char *aarch64Forward = R"llvm(
%Va = type { ptr, ptr, ptr, i32, i32 }
declare void @llvm.va_start(ptr)
declare void @llvm.va_end(ptr)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
declare void @sink(ptr)
define void @forward(i32 %fixed, ...) {
  %state = alloca %Va, align 8
  %copy = alloca %Va, align 8
  call void @llvm.va_start(ptr %state)
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %copy, ptr align 8 %state, i64 32, i1 false)
  call void @sink(ptr %copy)
  call void @llvm.va_end(ptr %state)
  ret void
}
)llvm";
std::string replace(std::string source, llvm::StringRef before, llvm::StringRef after) {
  auto position = source.find(before.str());
  if (position == std::string::npos) llvm_unreachable("invalid fixture mutation");
  source.replace(position, before.size(), after.str());
  return source;
}
bool test(llvm::StringRef name, llvm::StringRef source, llvm::StringRef target, unsigned expected) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(source, diagnostic, context);
  if (module) module->setDataLayout(sela::targets::find(target)->layout);
  if (!module || llvm::verifyModule(*module, &llvm::errs())) {
    llvm::errs() << "invalid test fixture: " << name << '\n'; return false;
  }
  auto result = sela::detail::normalizeNativeVarargs(*module, target);
  if (!result) { llvm::logAllUnhandledErrors(result.takeError(), llvm::errs()); return false; }
  if (result->scalarExtractions != expected || llvm::verifyModule(*module, &llvm::errs())) {
    llvm::errs() << name << ": got " << result->scalarExtractions << ", expected " << expected << '\n';
    return false;
  }
  return true;
}
bool forwarding(llvm::StringRef name, llvm::StringRef source, llvm::StringRef target,
                unsigned expected, unsigned scaffolding) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(source, diagnostic, context);
  if (module) module->setDataLayout(sela::targets::find(target)->layout);
  if (!module || llvm::verifyModule(*module, &llvm::errs())) return false;
  auto result = sela::detail::normalizeNativeVarargs(*module, target);
  if (!result) { llvm::logAllUnhandledErrors(result.takeError(), llvm::errs()); return false; }
  unsigned arguments = 0;
  for (const auto &call : result->forwardedArguments) arguments += call.second.size();
  bool passed = arguments == expected && result->forwardingScaffolding.size() == scaffolding;
  if (target == "aarch64" && expected) {
    auto *function = module->getFunction("forward");
    auto *state = llvm::dyn_cast<llvm::AllocaInst>(&function->getEntryBlock().front());
    passed &= state && result->states.contains(state);
    for (const auto &entry : result->forwardedArguments)
      for (const auto &argument : entry.second)
        passed &= argument.second == state && entry.first->getArgOperand(argument.first) == state;
    for (const auto &block : *function)
      for (const auto &instruction : block)
        if (llvm::isa<llvm::AllocaInst>(instruction)) passed &= &instruction == state;
  }
  if (!passed) llvm::errs() << name << ": unexpected forwarding proof " << arguments << "/"
                            << result->forwardingScaffolding.size() << '\n';
  return passed && !llvm::verifyModule(*module, &llvm::errs());
}
bool incoming(llvm::StringRef name, llvm::StringRef source, llvm::StringRef target,
              unsigned localForwards, unsigned valueForwards, bool spillRemains = false) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(source, diagnostic, context);
  if (module) module->setDataLayout(sela::targets::find(target)->layout);
  if (!module || llvm::verifyModule(*module, &llvm::errs())) return false;
  auto result = sela::detail::normalizeNativeVarargs(*module, target);
  if (!result) { llvm::logAllUnhandledErrors(result.takeError(), llvm::errs()); return false; }
  unsigned local = 0, values = 0, spills = 0;
  for (const auto &call : result->forwardedArguments) local += call.second.size();
  for (const auto &call : result->forwardedValues) values += call.second.size();
  auto *relay = module->getFunction("sink");
  for (const auto &block : *relay)
    for (const auto &instruction : block)
      spills += llvm::isa<llvm::StoreInst>(instruction);
  const bool passed = local == localForwards && values == valueForwards &&
      result->incomingArguments.contains(relay->getArg(0)) &&
      result->forwardedParameters[relay].contains(0) && bool(spills) == spillRemains;
  if (!passed) llvm::errs() << name << ": incoming proof " << local << "/" << values
                            << " remaining stores=" << spills << '\n';
  return passed && !llvm::verifyModule(*module, &llvm::errs());
}
}
int main(int argc, char **argv) {
  if (argc == 4 || (argc == 5 && llvm::StringRef(argv[4]) == "--dump")) {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseIRFile(argv[2], diagnostic, context);
    if (!module || llvm::verifyModule(*module, &llvm::errs())) return 2;
    auto result = sela::detail::normalizeNativeVarargs(*module, argv[1]);
    if (!result) { llvm::logAllUnhandledErrors(result.takeError(), llvm::errs()); return 1; }
    unsigned forwards = 0;
    for (const auto &call : result->forwardedArguments) forwards += call.second.size();
    unsigned incoming = 0;
    for (const auto &call : result->forwardedValues) incoming += call.second.size();
    if (argc == 5) module->print(llvm::outs(), nullptr);
    else llvm::outs() << argv[1] << ": extractions=" << result->scalarExtractions << " forwards=" << forwards
                      << " incoming=" << incoming << '\n';
    return result->scalarExtractions == std::stoul(argv[3]) && !llvm::verifyModule(*module, &llvm::errs()) ? 0 : 1;
  }
  bool passed = test("native register/overflow diamond", wide, "x86_64", 1);
  passed &= test("native stack cursor", narrow, "i686", 1);
  passed &= test("changed register stride", replace(wide, "add i32 %offset, 8", "add i32 %offset, 16"), "x86_64", 0);
  passed &= test("changed register bound", replace(wide, "ule i32 %offset, 40", "ule i32 %offset, 48"), "x86_64", 0);
  passed &= test("volatile register store", replace(wide, "store i32 %increment", "store volatile i32 %increment"), "x86_64", 0);
  passed &= test("extra register side effect", replace(wide, "  br label %join\noverflow:", "  call void @observe(i32 %offset)\n  br label %join\noverflow:"), "x86_64", 0);
  passed &= test("escaping selected pointer", replace(wide, "  call void @llvm.va_end", "  %extra = ptrtoint ptr %selected to i64\n  call void @llvm.va_end"), "x86_64", 0);
  passed &= test("changed stack stride", replace(narrow, "ptr %cursor, i32 4", "ptr %cursor, i32 8"), "i686", 0);
  passed &= test("volatile stack store", replace(narrow, "store ptr %next", "store volatile ptr %next"), "i686", 0);
  passed &= test("escaping cursor", replace(narrow, "  call void @llvm.va_end", "  store ptr %cursor, ptr @observed\n  call void @llvm.va_end"), "i686", 0);
  passed &= test("ARM32 aligned cursor", armAligned, "armv7", 1);
  passed &= test("ARM32 incorrect rounding", replace(armAligned, "i32 7", "i32 3"), "armv7", 0);
  passed &= test("ARM32 incorrect mask", replace(armAligned, "i32 -8", "i32 -4"), "armv7", 0);
  passed &= test("ARM32 escaping aligned cursor", replace(armAligned, "  call void @llvm.va_end", "  store ptr %aligned, ptr @observed\n  call void @llvm.va_end"), "armv7", 0);
  passed &= test("AArch64 GP transition", armWide, "aarch64", 1);
  passed &= test("AArch64 wrong register stride", replace(armWide, "add i32 %offset, 8", "add i32 %offset, 16"), "aarch64", 0);
  passed &= test("AArch64 unsigned availability", replace(armWide, "icmp sle", "icmp ule"), "aarch64", 0);
  passed &= test("AArch64 wrong overflow stride", replace(armWide, "ptr %cursor, i64 8", "ptr %cursor, i64 16"), "aarch64", 0);
  passed &= test("AArch64 hidden side effect", replace(armWide, "  br label %join\noverflow:", "  call void @observe(i32 %offset)\n  br label %join\noverflow:"), "aarch64", 0);
  passed &= test("AArch64 escaping pointer", replace(armWide, "  call void @llvm.va_end", "  %extra = ptrtoint ptr %selected to i64\n  call void @llvm.va_end"), "aarch64", 0);
  passed &= forwarding("ARM32 ABI argument", armForward, "armv7", 1, 1);
  passed &= forwarding("ARM32 unproved alignment", replace(armForward, "ptr %field, align 4", "ptr %field, align 2"), "armv7", 0, 0);
  passed &= forwarding("AArch64 owned copy", aarch64Forward, "aarch64", 1, 0);
  passed &= forwarding("AArch64 truncated copy", replace(aarch64Forward, "i64 32", "i64 24"), "aarch64", 0, 0);
  passed &= forwarding("AArch64 volatile copy", replace(aarch64Forward, "i1 false", "i1 true"), "aarch64", 0, 0);
  const auto armRelay = replace(armForward, "declare void @sink([1 x i32])", R"llvm(
declare void @next([1 x i32])
define void @sink([1 x i32] %argument) {
  %spill = alloca %Va, align 4
  %write = getelementptr inbounds %Va, ptr %spill, i32 0, i32 0
  store [1 x i32] %argument, ptr %write, align 4
  %read = getelementptr inbounds %Va, ptr %spill, i32 0, i32 0
  %value = load [1 x i32], ptr %read, align 4
  call void @next([1 x i32] %value)
  ret void
}
)llvm");
  passed &= incoming("ARM32 defined cursor relay", armRelay, "armv7", 1, 1);
  passed &= incoming("ARM32 volatile incoming spill retained",
      replace(armRelay, "store [1 x i32] %argument", "store volatile [1 x i32] %argument"),
      "armv7", 1, 0, true);
  passed &= incoming("ARM32 overwritten incoming spill retained",
      replace(armRelay, "  %read =", "  store [1 x i32] zeroinitializer, ptr %write, align 4\n  %read ="),
      "armv7", 1, 0, true);
  passed &= forwarding("ARM32 mixed unproved actual rejects formal",
      std::string(armForward) + "\ndefine void @unproved() { call void @sink([1 x i32] zeroinitializer)\n ret void }\n",
      "armv7", 0, 0);
  const auto aarch64Relay = replace(aarch64Forward, "declare void @sink(ptr)", R"llvm(
declare void @next(ptr)
define void @sink(ptr %argument) {
  %spill = alloca ptr, align 8
  %copy = alloca %Va, align 8
  store ptr %argument, ptr %spill, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %copy, ptr align 8 %argument, i64 32, i1 false)
  call void @next(ptr %copy)
  ret void
}
)llvm");
  passed &= incoming("AArch64 defined cursor relay proves its implicit ABI copy", aarch64Relay, "aarch64", 1, 1);
  const auto pointerRelay = replace(aarch64Forward, "declare void @sink(ptr)", R"llvm(
declare void @next(ptr)
define void @sink(ptr %argument) {
  %spill = alloca ptr, align 8
  store ptr %argument, ptr %spill, align 8
  %value = load ptr, ptr %spill, align 8
  call void @next(ptr %value)
  ret void
}
)llvm");
  passed &= incoming("AArch64 unproved direct passthrough is not given copy semantics", pointerRelay, "aarch64", 1, 0);
  return passed ? 0 : 1;
}
