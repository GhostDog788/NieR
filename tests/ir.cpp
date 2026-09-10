#include "nier/IR/Compiler.h"
#include "nier/IR/Dialect.h"
#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <cstdlib>
#include <sys/stat.h>

namespace {
const char *validAggregateABI = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({
  ^entry(%output: !nier.ptr, %input: !nier.ptr):
    %zero = "nier.constant"() {value = 0 : i64} : () -> i32
    %one = "nier.constant"() {value = 1 : i64} : () -> i32
    %left = "nier.gep"(%input, %zero, %zero) {element = !nier.record<"r0", 0, [i32, i32]>, inbounds = true} : (!nier.ptr, i32, i32) -> !nier.ptr
    %right = "nier.gep"(%input, %zero, %one) {element = !nier.record<"r0", 0, [i32, i32]>, inbounds = true} : (!nier.ptr, i32, i32) -> !nier.ptr
    %first = "nier.load"(%left) {alignment = 4 : i64} : (!nier.ptr) -> i32
    %second = "nier.load"(%right) {alignment = 4 : i64} : (!nier.ptr) -> i32
    %out_left = "nier.gep"(%output, %zero, %zero) {element = !nier.record<"r0", 0, [i32, i32]>, inbounds = true} : (!nier.ptr, i32, i32) -> !nier.ptr
    %out_right = "nier.gep"(%output, %zero, %one) {element = !nier.record<"r0", 0, [i32, i32]>, inbounds = true} : (!nier.ptr, i32, i32) -> !nier.ptr
    "nier.store"(%first, %out_left) {alignment = 4 : i64} : (i32, !nier.ptr) -> ()
    "nier.store"(%second, %out_right) {alignment = 4 : i64} : (i32, !nier.ptr) -> ()
    "nier.return"() : () -> ()
  }) {id = "record_identity", type = (!nier.ptr, !nier.ptr) -> (),
      native_abi = (!nier.record<"r0", 0, [i32, i32]>) -> !nier.record<"r0", 0, [i32, i32]>,
      declaration = false, variadic = false, internal = false, dso_local = true, attributes = [[], [], [], []]} : () -> ()
  "nier.func"() ({
  ^entry(%output: !nier.ptr, %input: !nier.ptr):
    %callback = "nier.address"() {global = "record_identity"} : () -> !nier.ptr
    "nier.call_indirect"(%callback, %output, %input) {
      type = (!nier.ptr, !nier.ptr) -> (),
      native_abi = (!nier.record<"r0", 0, [i32, i32]>) -> !nier.record<"r0", 0, [i32, i32]>,
      variadic = false, attributes = [[], [], [], []], tail = 0 : i32} : (!nier.ptr, !nier.ptr, !nier.ptr) -> ()
    "nier.return"() : () -> ()
  }) {id = "record_callback", type = (!nier.ptr, !nier.ptr) -> (),
      native_abi = (!nier.record<"r0", 0, [i32, i32]>) -> !nier.record<"r0", 0, [i32, i32]>,
      declaration = false, variadic = false, internal = false, dso_local = true, attributes = [[], [], [], []]} : () -> ()
}
)mlir";
const char *validIndexDomain = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.global"() {id = "table", element = !nier.word_array<3, 2, i32>,
    initializer = {array = [{word64 = 1 : i64, word32 = 4 : i64}, {word64 = 2 : i64, word32 = 5 : i64}, 3 : i64], count = {word64 = 3 : i64, word32 = 2 : i64}},
    constant = true, declaration = false, linkage = "internal", dso_local = true,
    alignment = 4 : i64, unnamed = 0 : i32} : () -> ()
}
)mlir";
const char *validVarargs = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({}) {id = "llvm.va_start", type = (!nier.ptr) -> (), declaration = true, variadic = false, internal = false, dso_local = false, attributes = [[], [], []]} : () -> ()
  "nier.func"() ({}) {id = "llvm.va_end", type = (!nier.ptr) -> (), declaration = true, variadic = false, internal = false, dso_local = false, attributes = [[], [], []]} : () -> ()
  "nier.func"() ({
  ^entry(%fixed: i32):
    %state = "nier.alloca"() {element = !nier.va_list, alignment = "pointer_bytes"} : () -> !nier.ptr
    "nier.call"(%state) {callee = "llvm.va_start", attributes = [[], [], []], tail = 0 : i32} : (!nier.ptr) -> ()
    %value = "nier.va_arg"(%state) : (!nier.ptr) -> i32
    "nier.call"(%state) {callee = "llvm.va_end", attributes = [[], [], []], tail = 0 : i32} : (!nier.ptr) -> ()
    "nier.return"(%value) : (i32) -> ()
  }) {id = "take", type = (i32) -> i32, declaration = false, variadic = true, internal = false, dso_local = true, attributes = [[], [], []]} : () -> ()
}
)mlir";
const char *valid = R"mlir(
module attributes {nier.schema = 1 : i32, nier.module_flags = []} {
  "nier.func"() ({
    %0 = "nier.constant"() {value = 1 : i64} : () -> i32
    %1 = "nier.constant"() {value = 2 : i64} : () -> i32
    %2 = "nier.binary"(%0, %1) {opcode = "add", flags = 0 : i32} : (i32, i32) -> i32
    "nier.return"(%2) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *validCFG = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({
    %condition = "nier.constant"() {value = 1 : i64} : () -> i1
    "nier.cond_br"(%condition)[^yes, ^no] {true_count = 0 : i32} : (i1) -> ()
  ^yes:
    %left = "nier.constant"() {value = 42 : i64} : () -> i32
    "nier.br"(%left)[^join] : (i32) -> ()
  ^no:
    %right = "nier.constant"() {value = 7 : i64} : () -> i32
    "nier.br"(%right)[^join] : (i32) -> ()
  ^join(%result : i32):
    "nier.return"(%result) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *validLoop = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({
    %zero = "nier.constant"() {value = 0 : i64} : () -> i32
    "nier.br"(%zero)[^loop] : (i32) -> ()
  ^loop(%index : i32):
    %one = "nier.constant"() {value = 1 : i64} : () -> i32
    %next = "nier.binary"(%index, %one) {opcode = "add", flags = 0 : i32} : (i32, i32) -> i32
    %limit = "nier.constant"() {value = 5 : i64} : () -> i32
    %again = "nier.compare"(%next, %limit) {predicate = 40 : i32} : (i32, i32) -> i1
    "nier.cond_br"(%again, %next, %next)[^loop, ^done] {true_count = 1 : i32} : (i1, i32, i32) -> ()
  ^done(%result : i32):
    "nier.return"(%result) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *validFP = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({
    %a = "nier.constant"() {value = 2.500000e+00 : f64} : () -> f64
    %b = "nier.constant"() {value = 1.500000e+00 : f64} : () -> f64
    %sum = "nier.binary"(%a, %b) {opcode = "fadd", flags = 0 : i32} : (f64, f64) -> f64
    %result = "nier.cast"(%sum) {opcode = "fptosi"} : (f64) -> i32
    "nier.return"(%result) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *validArray = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({
    %storage = "nier.alloca"() {element = !nier.array<4, i32>, alignment = 16 : i64} : () -> !nier.ptr
    %zero = "nier.constant"() {value = 0 : i64} : () -> !nier.word
    %index = "nier.constant"() {value = 2 : i64} : () -> !nier.word
    %address = "nier.gep"(%storage, %zero, %index) {element = !nier.array<4, i32>, inbounds = true} : (!nier.ptr, !nier.word, !nier.word) -> !nier.ptr
    %value = "nier.constant"() {value = 42 : i64} : () -> i32
    "nier.store"(%value, %address) {alignment = 4 : i64} : (i32, !nier.ptr) -> ()
    %loaded = "nier.load"(%address) {alignment = 4 : i64} : (!nier.ptr) -> i32
    "nier.return"(%loaded) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *widthSpecific = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({
    %value = "nier.constant"() {value = 42 : i64} : () -> i32
    %word = "nier.cast"(%value) {opcode = "sext"} : (i32) -> !nier.word
    "nier.return"(%word) : (!nier.word) -> ()
  }) {id = "answer", type = () -> !nier.word, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *validStorage = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.global"() {id = "state", element = !nier.record<"r0", 0, [i8, !nier.word]>,
      initializer = [7 : i64, 42 : i64], constant = false, declaration = false,
      linkage = "external", dso_local = false, alignment = "pointer_bytes", unnamed = 0 : i32} : () -> ()
  "nier.global"() {id = "callback", element = !nier.ptr, initializer = {symbol = "answer"},
      constant = false, declaration = false, linkage = "external", dso_local = false,
      alignment = "pointer_bytes", unnamed = 0 : i32} : () -> ()
  "nier.func"() ({
    %value = "nier.constant"() {value = 42 : i64} : () -> i32
    "nier.return"(%value) : (i32) -> ()
  }) {id = "answer", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = false, attributes = [[], []]} : () -> ()
  "nier.func"() ({
    %address = "nier.address"() {global = "callback"} : () -> !nier.ptr
    %callback = "nier.load"(%address) {alignment = "pointer_bytes"} : (!nier.ptr) -> !nier.ptr
    %result = "nier.call_indirect"(%callback) {type = () -> i32, variadic = false,
        attributes = [[], []], tail = 0 : i32} : (!nier.ptr) -> i32
    "nier.return"(%result) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = false, attributes = [[], []]} : () -> ()
}
)mlir";

std::string replace(std::string source, const std::string &from,
                    const std::string &to) {
  auto position = source.find(from);
  if (position == std::string::npos) {
    llvm::errs() << "bad test replacement: " << from << '\n';
    std::exit(2);
  }
  source.replace(position, from.size(), to);
  return source;
}

bool check(llvm::StringRef directory, llvm::StringRef label,
           const std::string &text, bool expectSuccess, bool bytecode = true,
           bool retainLocations = false,
           llvm::ArrayRef<llvm::StringRef> targets = {"x86_64", "i686"}) {
  llvm::SmallString<256> path(directory);
  llvm::sys::path::append(path, label);
  std::error_code ec;
  {
    llvm::raw_fd_ostream output(path, ec, llvm::sys::fs::OF_None);
    if (ec) { llvm::errs() << ec.message() << '\n'; return false; }
    if (bytecode) {
      mlir::MLIRContext context;
      context.getOrLoadDialect<nier::ir::NIERDialect>();
      auto module = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
      if (module && !retainLocations)
        module->walk([&](mlir::Operation *operation) {
          operation->setLoc(mlir::UnknownLoc::get(&context));
          for (auto &region : operation->getRegions())
            for (auto &block : region)
              for (auto argument : block.getArguments())
                argument.setLoc(mlir::UnknownLoc::get(&context));
        });
      if (!module || mlir::failed(mlir::writeBytecodeToFile(module->getOperation(), output))) {
        llvm::errs() << "cannot construct test fixture " << label << '\n';
        return false;
      }
    } else {
      output << text;
    }
  }
  nier::ArtifactSummary summary;
  auto error = nier::inspectArtifact(path, summary, targets);
  bool success = !error;
  if (error) {
    std::string message = llvm::toString(std::move(error));
    if (expectSuccess) llvm::errs() << label << ": " << message << '\n';
  }
  llvm::sys::fs::remove(path);
  if (success != expectSuccess) {
    llvm::errs() << label << ": incorrect acceptance result\n";
    return false;
  }
  return true;
}
} // namespace

int main() {
  llvm::SmallString<256> directory;
  if (auto ec = llvm::sys::fs::createUniqueDirectory("nier-ir-tests", directory)) {
    llvm::errs() << ec.message() << '\n';
    return 1;
  }
  bool passed = true;
  passed &= check(directory, "valid.mlirbc", valid, true);
  passed &= check(directory, "independent-native-aggregate-abi.mlirbc", validAggregateABI, true);
  passed &= check(directory, "invalid-native-aggregate-body.mlirbc",
      replace(validAggregateABI, "type = (!nier.ptr, !nier.ptr) -> ()", "type = (!nier.ptr) -> ()"), false);
  passed &= check(directory, "invalid-native-aggregate-scalar-attributes.mlirbc",
      replace(validAggregateABI, "attributes = [[], [], [], []]", "attributes = [[], [], [], [{name = \"noundef\"}]]"), false);
  passed &= check(directory, "invalid-native-aggregate-tail-call.mlirbc",
      replace(validAggregateABI, "tail = 0", "tail = 1"), false);
  passed &= check(directory, "invalid-native-aggregate-signature-kind.mlirbc",
      replace(validAggregateABI, "native_abi = (!nier.record<\"r0\", 0, [i32, i32]>) -> !nier.record<\"r0\", 0, [i32, i32]>",
                                "native_abi = \"opaque native ABI\""), false);
  passed &= check(directory, "valid-available-externally.mlirbc",
                  replace(valid, "internal = false", "available_externally = true, internal = false"), true);
  passed &= check(directory, "conflicting-native-linkage.mlirbc",
                  replace(valid, "internal = false", "available_externally = true, internal = true"), false);
  passed &= check(directory, "invalid-native-linkage-kind.mlirbc",
                  replace(valid, "internal = false", "available_externally = 1 : i32, internal = false"), false);
  passed &= check(directory, "valid-native-byte-swap.mlirbc",
                  replace(valid, "    \"nier.return\"(%2)", "    %swapped = \"nier.bswap\"(%2) : (i32) -> i32\n    \"nier.return\"(%2)"), true);
  passed &= check(directory, "valid-native-index-domain.mlirbc", validIndexDomain, true);
  passed &= check(directory, "invalid-native-index-extent.mlirbc",
                  replace(validIndexDomain, "count = {word64 = 3", "count = {word64 = 4"), false);
  passed &= check(directory, "invalid-native-index-storage.mlirbc",
                  replace(validIndexDomain, "word_array<3, 2", "word_array<3, 1"), false);
  passed &= check(directory, "private-inactive-native-index-tail.mlirbc",
                  replace(validIndexDomain, ", 3 : i64], count", ", {private = \"hidden source\"}], count"), false, true, false, {"i686"});
  passed &= check(directory, "independent-native-varargs.mlirbc", validVarargs, true);
  passed &= check(directory, "independent-native-varargs-forwarding.mlirbc",
                  replace(validVarargs, "    %value =", "    %forward = \"nier.va_forward\"(%state) : (!nier.ptr) -> !nier.ptr\n    %value ="), true);
  passed &= check(directory, "invalid-native-varargs-forwarding-result.mlirbc",
                  replace(validVarargs, "    %value =", "    %forward = \"nier.va_forward\"(%state) : (!nier.ptr) -> i32\n    %value ="), false);
  passed &= check(directory, "unknown-native-vararg-semantics.mlirbc",
                  replace(validVarargs, "\"nier.va_arg\"(%state)", "\"nier.va_arg\"(%state) {future_abi = true}"), false);
  passed &= check(directory, "invalid-native-visibility.mlirbc",
                  replace(valid, "dso_local = true", "visibility = \"invented\", dso_local = true"), false);
  passed &= check(directory, "invalid-native-intrinsic.mlirbc",
                  replace(valid, "dso_local = true", "intrinsic = \"invented\", dso_local = true"), false);
  passed &= check(directory, "valid-frame-pointer-policy.mlirbc",
                  replace(valid, "nier.module_flags = []",
                          "nier.module_flags = [{name = \"frame-pointer\", behavior = 7 : i32, value = 2 : i32, profile = \"both\"}]"), true);
  passed &= check(directory, "invalid-frame-pointer-policy.mlirbc",
                  replace(valid, "nier.module_flags = []",
                          "nier.module_flags = [{name = \"frame-pointer\", behavior = 7 : i32, value = 3 : i32, profile = \"both\"}]"), false);
  passed &= check(directory, "valid-cfg.mlirbc", validCFG, true);
  passed &= check(directory, "valid-loop.mlirbc", validLoop, true);
  passed &= check(directory, "valid-floating.mlirbc", validFP, true);
  passed &= check(directory, "valid-native-array.mlirbc", validArray, true);
  passed &= check(directory, "valid-record-callback.mlirbc", validStorage, true);
  passed &= check(directory, "invalid-global-symbol.mlirbc",
                  replace(validStorage, "symbol = \"answer\"", "symbol = \"missing\""), false);
  passed &= check(directory, "invalid-global-initializer-size.mlirbc",
                  replace(validStorage, "[7 : i64, 42 : i64]", "[7 : i64]"), false);
  passed &= check(directory, "private-record-identity.mlirbc",
                  replace(validStorage, "record<\"r0\"", "record<\"PrivateSourceType\""), false);
  passed &= check(directory, "invalid-indirect-call-arity.mlirbc",
                  replace(validStorage, "type = () -> i32, variadic = false", "type = (i32) -> i32, variadic = false"), false);
  passed &= check(directory, "native-cast-or-identity.mlirbc",
                  replace(widthSpecific, "opcode = \"sext\"", "opcode = \"native_sext\""), true);
  passed &= check(directory, "invalid-native-cast-policy.mlirbc",
                  replace(widthSpecific, "opcode = \"sext\"", "opcode = \"native_bitcast\""), false);
  passed &= check(directory, "invalid-array-extent.mlirbc",
                  replace(validArray, "array<4, i32>", "array<4294967296, i32>"), false);
  passed &= check(directory, "declared-x64-domain.mlirbc", widthSpecific, true, true, false, {"x86_64"});
  passed &= check(directory, "invalid-declared-i686-domain.mlirbc", widthSpecific, false, true, false, {"i686"});
  passed &= check(directory, "unknown-semantic-target.mlirbc", valid, false, true, false, {"unknown"});
  passed &= check(directory, "empty-semantic-domain.mlirbc", valid, false, true, false, {});
  passed &= check(directory, "duplicate-semantic-target.mlirbc", valid, false, true, false, {"x86_64", "x86_64"});
  passed &= check(directory, "invalid-floating-integer-flags.mlirbc",
                  replace(validFP, "flags = 0", "flags = 3"), false);
  passed &= check(directory, "word-domain-overflow-flags.mlirbc",
                  replace(valid, "flags = 0 : i32", "flags = {word64 = 0 : i64, word32 = 2 : i64}"), true);
  passed &= check(directory, "invalid-inactive-overflow-flags.mlirbc",
                  replace(valid, "flags = 0 : i32", "flags = {word64 = 9 : i64, word32 = 2 : i64}"), false, true, false, {"i686"});
  passed &= check(directory, "invalid-floating-opcode.mlirbc",
                  replace(validFP, "opcode = \"fadd\"", "opcode = \"add\""), false);
  passed &= check(directory, "invalid-branch-argument-count.mlirbc",
                  replace(validCFG, "\"nier.br\"(%left)[^join] : (i32)",
                          "\"nier.br\"()[^join] : ()"), false);
  passed &= check(directory, "invalid-branch-condition-count.mlirbc",
                  replace(validCFG, "true_count = 0", "true_count = 9"), false);
  passed &= check(directory, "independent-no-compiler-flags.mlirbc",
                  replace(valid, ", nier.module_flags = []", ""), true);
  passed &= check(directory, "producer-provenance-is-not-core.mlirbc",
                  replace(valid, "nier.schema = 1", "nier.profiles = [\"x86_64\", \"i686\"], nier.schema = 1"), false);
  passed &= check(directory, "text-is-not-bytecode.mlirbc", valid, false, false);
  passed &= check(directory, "truncated.mlirbc", "ML", false, false);
  passed &= check(directory, "invalid-exact-add.mlirbc",
                  replace(valid, "flags = 0", "flags = 4"), false);
  passed &= check(directory, "invalid-wrap-div.mlirbc",
                  replace(replace(valid, "flags = 0", "flags = 2"),
                          "opcode = \"add\"", "opcode = \"udiv\""), false);
  passed &= check(directory, "valid-wrap-add.mlirbc",
                  replace(valid, "flags = 0", "flags = 3"), true);
  passed &= check(directory, "valid-exact-div.mlirbc",
                  replace(replace(valid, "flags = 0", "flags = 4"),
                          "opcode = \"add\"", "opcode = \"udiv\""), true);
  passed &= check(directory, "unknown-schema.mlirbc",
                  replace(valid, "nier.schema = 1", "nier.schema = 2"), false);
  passed &= check(directory, "unknown-module-field.mlirbc",
                  replace(valid, "nier.schema = 1", "nier.private_source = \"secret.c\", nier.schema = 1"), false);
  passed &= check(directory, "unknown-op-field.mlirbc",
                  replace(valid, "flags = 0", "future_semantics = true, flags = 0"), false);
  passed &= check(directory, "oversized-integer.mlirbc",
                  replace(valid, "value = 1 : i64", "value = 18446744073709551616 : i128"), false);
  passed &= check(directory, "unknown-function-attribute.mlirbc",
                  replace(valid, "attributes = [[], []]", "attributes = [[{name = \"invented\"}], []]"), false);
  passed &= check(directory, "private-locations.mlirbc", valid, false, true, true);
  std::string fifo = directory.str().str() + "/fifo";
  if (::mkfifo(fifo.c_str(), 0600)) passed = false;
  else {
    nier::ArtifactSummary summary;
    auto error = nier::inspectArtifact(fifo, summary);
    if (!error) { llvm::errs() << "FIFO input was accepted\n"; passed = false; }
    else llvm::consumeError(std::move(error));
    llvm::sys::fs::remove(fifo);
  }
  for (llvm::StringRef input : {llvm::StringRef("/dev/zero"), llvm::StringRef(directory)}) {
    nier::ArtifactSummary summary;
    auto error = nier::inspectArtifact(input, summary);
    if (!error) { llvm::errs() << "non-regular input was accepted\n"; passed = false; }
    else llvm::consumeError(std::move(error));
  }
  llvm::sys::fs::remove(directory);
  return passed ? 0 : 1;
}
