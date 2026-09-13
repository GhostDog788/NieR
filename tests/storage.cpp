#include "sela/Producer/LLVM.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

namespace {
std::string replaceAll(std::string text, const std::string &from, const std::string &to) {
  size_t offset = 0;
  while ((offset = text.find(from, offset)) != std::string::npos) {
    text.replace(offset, from.size(), to);
    offset += to.size();
  }
  return text;
}
const char *source = R"llvm(
%PrivateState = type { i8, WORD, ptr, [3 x i32] }
%PrivateJump = type { [REGCOUNT x WORD], i32, [MASKCOUNT x WORD] }
@state = global %PrivateState { i8 7, WORD 17, ptr @compare, [3 x i32] [i32 3, i32 1, i32 2] }, align ALIGN
@forward = global ptr @number, align ALIGN
@number = hidden global i32 19, align 4
@slot = global ptr @increment, align ALIGN
@element = global ptr getelementptr inbounds (%PrivateState, ptr @state, i32 0, i32 1), align ALIGN
@zero = common global %PrivateJump zeroinitializer, align ALIGN
@domain = internal constant [DOMAINCOUNT x i32] DOMAINVALUES, align 4

declare void @qsort(ptr, WORD, WORD, ptr)
declare i32 @_setjmp(ptr) returns_twice
declare void @longjmp(ptr, i32) noreturn
declare void @llvm.memcpy.p0.p0.WORD(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, WORD, i1 immarg)
declare void @llvm.memset.p0.WORD(ptr nocapture writeonly, i8, WORD, i1 immarg)
declare void @llvm.memmove.p0.p0.WORD(ptr nocapture writeonly, ptr nocapture readonly, WORD, i1 immarg)

define i32 @compare(ptr %left, ptr %right) {
entry:
  %a = load i32, ptr %left, align 4
  %b = load i32, ptr %right, align 4
  %difference = sub i32 %a, %b
  ret i32 %difference
}
define i32 @increment(i32 %value) {
entry:
  %next = add i32 %value, 1
  ret i32 %next
}
define i32 @indirect(i32 %value) {
entry:
  %callback = load ptr, ptr @slot, align ALIGN
  %result = call i32 %callback(i32 %value)
  ret i32 %result
}
define void @callback_sort() {
entry:
  call void @qsort(ptr getelementptr inbounds (%PrivateState, ptr @state, i32 0, i32 3, i32 0), WORD 3, WORD 4, ptr @compare)
  ret void
}
define WORD @word_field() {
entry:
  %value = load WORD, ptr getelementptr inbounds (%PrivateState, ptr @state, i32 0, i32 1), align ALIGN
  ret WORD %value
}
define i32 @jump() {
entry:
  %environment = alloca %PrivateJump, align ALIGN
  %value = call i32 @_setjmp(ptr %environment)
  %first = icmp eq i32 %value, 0
  br i1 %first, label %again, label %done
again:
  call void @longjmp(ptr %environment, i32 42)
  unreachable
done:
  ret i32 %value
}
define WORD @storage_size() {
entry:
  ret WORD JSIZE
}
define i32 @native_width() {
entry:
  ret i32 ALIGN
}
define i32 @promotion() {
entry:
  %value = call i32 @native_width()
  PROMOTION
  %equal = icmp eq WORD COMPARED, ALIGN
  %result = zext i1 %equal to i32
  ret i32 %result
}
define void @copy_storage() {
entry:
  %buffer = alloca %PrivateState, align ALIGN
  call void @llvm.memcpy.p0.p0.WORD(ptr align ALIGN %buffer, ptr align ALIGN @state, WORD COPYSIZE, i1 false)
  call void @llvm.memmove.p0.p0.WORD(ptr align ALIGN @state, ptr align ALIGN %buffer, WORD COPYSIZE, i1 false)
  call void @llvm.memset.p0.WORD(ptr align ALIGN %buffer, i8 0, WORD COPYSIZE, i1 false)
  store volatile i8 17, ptr %buffer, align 1
  %observable = load volatile i8, ptr %buffer, align 1
  ret void
}
)llvm";

std::string native(bool wide) {
  std::string text = source;
  text = replaceAll(text, "WORD", wide ? "i64" : "i32");
  text = replaceAll(text, "REGCOUNT", wide ? "8" : "6");
  text = replaceAll(text, "MASKCOUNT", wide ? "16" : "32");
  text = replaceAll(text, "ALIGN", wide ? "8" : "4");
  text = replaceAll(text, "JSIZE", wide ? "200" : "156");
  text = replaceAll(text, "COPYSIZE", wide ? "40" : "24");
  text = replaceAll(text, "DOMAINCOUNT", wide ? "3" : "2");
  text = replaceAll(text, "DOMAINVALUES", wide ? "[i32 1, i32 2, i32 3]" : "[i32 4, i32 5]");
  text = replaceAll(text, "PROMOTION", wide ? "%wide = zext i32 %value to i64" : "");
  text = replaceAll(text, "COMPARED", wide ? "%wide" : "%value");
  return std::string("target triple = \"") + (wide ? "x86_64-unknown-linux-gnu" : "i686-unknown-linux-gnu") + "\"\n" +
      "target datalayout = \"" + (wide ?
      "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128" :
      "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128") + "\"\n" + text;
}
bool write(const std::string &path, const std::string &text) {
  std::error_code error;
  llvm::raw_fd_ostream stream(path, error, llvm::sys::fs::OF_None);
  if (error) return false;
  stream << text;
  stream.flush();
  return !stream.has_error();
}
}

int main() {
  llvm::SmallString<256> directory;
  if (auto error = llvm::sys::fs::createUniqueDirectory("sela-storage-tests", directory)) {
    llvm::errs() << error.message() << '\n'; return 1;
  }
  std::string base = directory.str().str();
  std::string left = base + "/wide.ll", right = base + "/narrow.ll", artifact = base + "/module.mlirbc";
  bool passed = write(left, native(true)) && write(right, native(false));
  if (passed) {
    sela::ArtifactSummary summary;
    if (auto error = sela::mergeProfiles(left, right, artifact, &summary)) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "storage producer: ");
      passed = false;
    }
  }
  if (!passed) llvm::errs() << "Private failing fixture retained at " << base << '\n';
  else {
    for (const auto &path : {left, right, artifact}) llvm::sys::fs::remove(path);
    llvm::sys::fs::remove(directory);
  }
  return passed ? 0 : 1;
}
