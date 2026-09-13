#include "../src/ir/Varargs.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
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
std::string replace(std::string source, llvm::StringRef before, llvm::StringRef after) {
  auto position = source.find(before.str());
  if (position == std::string::npos) llvm_unreachable("invalid fixture mutation");
  source.replace(position, before.size(), after.str());
  return source;
}
bool test(llvm::StringRef name, llvm::StringRef source, bool word64, unsigned expected) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(source, diagnostic, context);
  if (!module || llvm::verifyModule(*module, &llvm::errs())) {
    llvm::errs() << "invalid test fixture: " << name << '\n'; return false;
  }
  auto result = sela::detail::normalizeNativeVarargs(*module, word64);
  if (!result) { llvm::logAllUnhandledErrors(result.takeError(), llvm::errs()); return false; }
  if (result->scalarExtractions != expected || llvm::verifyModule(*module, &llvm::errs())) {
    llvm::errs() << name << ": got " << result->scalarExtractions << ", expected " << expected << '\n';
    return false;
  }
  return true;
}
}
int main() {
  bool passed = test("native register/overflow diamond", wide, true, 1);
  passed &= test("native stack cursor", narrow, false, 1);
  passed &= test("changed register stride", replace(wide, "add i32 %offset, 8", "add i32 %offset, 16"), true, 0);
  passed &= test("changed register bound", replace(wide, "ule i32 %offset, 40", "ule i32 %offset, 48"), true, 0);
  passed &= test("volatile register store", replace(wide, "store i32 %increment", "store volatile i32 %increment"), true, 0);
  passed &= test("extra register side effect", replace(wide, "  br label %join\noverflow:", "  call void @observe(i32 %offset)\n  br label %join\noverflow:"), true, 0);
  passed &= test("escaping selected pointer", replace(wide, "  call void @llvm.va_end", "  %extra = ptrtoint ptr %selected to i64\n  call void @llvm.va_end"), true, 0);
  passed &= test("changed stack stride", replace(narrow, "ptr %cursor, i32 4", "ptr %cursor, i32 8"), false, 0);
  passed &= test("volatile stack store", replace(narrow, "store ptr %next", "store volatile ptr %next"), false, 0);
  passed &= test("escaping cursor", replace(narrow, "  call void @llvm.va_end", "  store ptr %cursor, ptr @observed\n  call void @llvm.va_end"), false, 0);
  return passed ? 0 : 1;
}
