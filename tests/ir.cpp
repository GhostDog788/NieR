#include "sela/IR/Compiler.h"
#include "sela/IR/Dialect.h"
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
module attributes {sela.schema = 1 : i32} {
  "sela.func"() ({
  ^entry(%output: !sela.ptr, %input: !sela.ptr):
    %zero = "sela.constant"() {value = 0 : i64} : () -> i32
    %one = "sela.constant"() {value = 1 : i64} : () -> i32
    %left = "sela.gep"(%input, %zero, %zero) {element = !sela.record<"r0", 0, [i32, i32]>, inbounds = true} : (!sela.ptr, i32, i32) -> !sela.ptr
    %right = "sela.gep"(%input, %zero, %one) {element = !sela.record<"r0", 0, [i32, i32]>, inbounds = true} : (!sela.ptr, i32, i32) -> !sela.ptr
    %first = "sela.load"(%left) {alignment = 4 : i64} : (!sela.ptr) -> i32
    %second = "sela.load"(%right) {alignment = 4 : i64} : (!sela.ptr) -> i32
    %out_left = "sela.gep"(%output, %zero, %zero) {element = !sela.record<"r0", 0, [i32, i32]>, inbounds = true} : (!sela.ptr, i32, i32) -> !sela.ptr
    %out_right = "sela.gep"(%output, %zero, %one) {element = !sela.record<"r0", 0, [i32, i32]>, inbounds = true} : (!sela.ptr, i32, i32) -> !sela.ptr
    "sela.store"(%first, %out_left) {alignment = 4 : i64} : (i32, !sela.ptr) -> ()
    "sela.store"(%second, %out_right) {alignment = 4 : i64} : (i32, !sela.ptr) -> ()
    "sela.return"() : () -> ()
  }) {id = "record_identity", type = (!sela.ptr, !sela.ptr) -> (),
      native_abi = (!sela.record<"r0", 0, [i32, i32]>) -> !sela.record<"r0", 0, [i32, i32]>,
      declaration = false, variadic = false, internal = false, dso_local = true, attributes = [[], [], [], []]} : () -> ()
  "sela.func"() ({
  ^entry(%output: !sela.ptr, %input: !sela.ptr):
    %callback = "sela.address"() {global = "record_identity"} : () -> !sela.ptr
    "sela.call_indirect"(%callback, %output, %input) {
      type = (!sela.ptr, !sela.ptr) -> (),
      native_abi = (!sela.record<"r0", 0, [i32, i32]>) -> !sela.record<"r0", 0, [i32, i32]>,
      variadic = false, attributes = [[], [], [], []], tail = 0 : i32} : (!sela.ptr, !sela.ptr, !sela.ptr) -> ()
    "sela.return"() : () -> ()
  }) {id = "record_callback", type = (!sela.ptr, !sela.ptr) -> (),
      native_abi = (!sela.record<"r0", 0, [i32, i32]>) -> !sela.record<"r0", 0, [i32, i32]>,
      declaration = false, variadic = false, internal = false, dso_local = true, attributes = [[], [], [], []]} : () -> ()
}
)mlir";
const char *validIndexDomain = R"mlir(
module attributes {sela.schema = 1 : i32} {
  "sela.global"() {id = "table", element = !sela.word_array<3, 2, i32>,
    initializer = {array = [{word64 = 1 : i64, word32 = 4 : i64}, {word64 = 2 : i64, word32 = 5 : i64}, 3 : i64], count = {word64 = 3 : i64, word32 = 2 : i64}},
    constant = true, declaration = false, linkage = "internal", dso_local = true,
    alignment = 4 : i64, unnamed = 0 : i32} : () -> ()
}
)mlir";
const char *validVarargs = R"mlir(
module attributes {sela.schema = 1 : i32} {
  "sela.func"() ({}) {id = "llvm.va_start", type = (!sela.ptr) -> (), declaration = true, variadic = false, internal = false, dso_local = false, attributes = [[], [], []]} : () -> ()
  "sela.func"() ({}) {id = "llvm.va_end", type = (!sela.ptr) -> (), declaration = true, variadic = false, internal = false, dso_local = false, attributes = [[], [], []]} : () -> ()
  "sela.func"() ({
  ^entry(%fixed: i32):
    %state = "sela.alloca"() {element = !sela.va_list, alignment = "pointer_bytes"} : () -> !sela.ptr
    "sela.call"(%state) {callee = "llvm.va_start", attributes = [[], [], []], tail = 0 : i32} : (!sela.ptr) -> ()
    %value = "sela.va_arg"(%state) : (!sela.ptr) -> i32
    "sela.call"(%state) {callee = "llvm.va_end", attributes = [[], [], []], tail = 0 : i32} : (!sela.ptr) -> ()
    "sela.return"(%value) : (i32) -> ()
  }) {id = "take", type = (i32) -> i32, declaration = false, variadic = true, internal = false, dso_local = true, attributes = [[], [], []]} : () -> ()
}
)mlir";
const char *valid = R"mlir(
module attributes {sela.schema = 1 : i32, sela.module_flags = []} {
  "sela.func"() ({
    %0 = "sela.constant"() {value = 1 : i64} : () -> i32
    %1 = "sela.constant"() {value = 2 : i64} : () -> i32
    %2 = "sela.binary"(%0, %1) {opcode = "add", flags = 0 : i32} : (i32, i32) -> i32
    "sela.return"(%2) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *validCFG = R"mlir(
module attributes {sela.schema = 1 : i32} {
  "sela.func"() ({
    %condition = "sela.constant"() {value = 1 : i64} : () -> i1
    "sela.cond_br"(%condition)[^yes, ^no] {true_count = 0 : i32} : (i1) -> ()
  ^yes:
    %left = "sela.constant"() {value = 42 : i64} : () -> i32
    "sela.br"(%left)[^join] : (i32) -> ()
  ^no:
    %right = "sela.constant"() {value = 7 : i64} : () -> i32
    "sela.br"(%right)[^join] : (i32) -> ()
  ^join(%result : i32):
    "sela.return"(%result) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *validLoop = R"mlir(
module attributes {sela.schema = 1 : i32} {
  "sela.func"() ({
    %zero = "sela.constant"() {value = 0 : i64} : () -> i32
    "sela.br"(%zero)[^loop] : (i32) -> ()
  ^loop(%index : i32):
    %one = "sela.constant"() {value = 1 : i64} : () -> i32
    %next = "sela.binary"(%index, %one) {opcode = "add", flags = 0 : i32} : (i32, i32) -> i32
    %limit = "sela.constant"() {value = 5 : i64} : () -> i32
    %again = "sela.compare"(%next, %limit) {predicate = 40 : i32} : (i32, i32) -> i1
    "sela.cond_br"(%again, %next, %next)[^loop, ^done] {true_count = 1 : i32} : (i1, i32, i32) -> ()
  ^done(%result : i32):
    "sela.return"(%result) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *validFP = R"mlir(
module attributes {sela.schema = 1 : i32} {
  "sela.func"() ({
    %a = "sela.constant"() {value = 2.500000e+00 : f64} : () -> f64
    %b = "sela.constant"() {value = 1.500000e+00 : f64} : () -> f64
    %sum = "sela.binary"(%a, %b) {opcode = "fadd", flags = 0 : i32} : (f64, f64) -> f64
    %result = "sela.cast"(%sum) {opcode = "fptosi"} : (f64) -> i32
    "sela.return"(%result) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *validArray = R"mlir(
module attributes {sela.schema = 1 : i32} {
  "sela.func"() ({
    %storage = "sela.alloca"() {element = !sela.array<4, i32>, alignment = 16 : i64} : () -> !sela.ptr
    %zero = "sela.constant"() {value = 0 : i64} : () -> !sela.word
    %index = "sela.constant"() {value = 2 : i64} : () -> !sela.word
    %address = "sela.gep"(%storage, %zero, %index) {element = !sela.array<4, i32>, inbounds = true} : (!sela.ptr, !sela.word, !sela.word) -> !sela.ptr
    %value = "sela.constant"() {value = 42 : i64} : () -> i32
    "sela.store"(%value, %address) {alignment = 4 : i64} : (i32, !sela.ptr) -> ()
    %loaded = "sela.load"(%address) {alignment = 4 : i64} : (!sela.ptr) -> i32
    "sela.return"(%loaded) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *widthSpecific = R"mlir(
module attributes {sela.schema = 1 : i32} {
  "sela.func"() ({
    %value = "sela.constant"() {value = 42 : i64} : () -> i32
    %word = "sela.cast"(%value) {opcode = "sext"} : (i32) -> !sela.word
    "sela.return"(%word) : (!sela.word) -> ()
  }) {id = "answer", type = () -> !sela.word, declaration = false, variadic = false,
      internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";

const char *validStorage = R"mlir(
module attributes {sela.schema = 1 : i32} {
  "sela.global"() {id = "state", element = !sela.record<"r0", 0, [i8, !sela.word]>,
      initializer = [7 : i64, 42 : i64], constant = false, declaration = false,
      linkage = "external", dso_local = false, alignment = "pointer_bytes", unnamed = 0 : i32} : () -> ()
  "sela.global"() {id = "callback", element = !sela.ptr, initializer = {symbol = "answer"},
      constant = false, declaration = false, linkage = "external", dso_local = false,
      alignment = "pointer_bytes", unnamed = 0 : i32} : () -> ()
  "sela.func"() ({
    %value = "sela.constant"() {value = 42 : i64} : () -> i32
    "sela.return"(%value) : (i32) -> ()
  }) {id = "answer", type = () -> i32, declaration = false, variadic = false,
      internal = false, dso_local = false, attributes = [[], []]} : () -> ()
  "sela.func"() ({
    %address = "sela.address"() {global = "callback"} : () -> !sela.ptr
    %callback = "sela.load"(%address) {alignment = "pointer_bytes"} : (!sela.ptr) -> !sela.ptr
    %result = "sela.call_indirect"(%callback) {type = () -> i32, variadic = false,
        attributes = [[], []], tail = 0 : i32} : (!sela.ptr) -> i32
    "sela.return"(%result) : (i32) -> ()
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
           llvm::ArrayRef<llvm::StringRef> targets = sela::supportedNativeTargets(),
           bool structuralOnly = false, bool allowUnregisteredFixture = false) {
  llvm::SmallString<256> path(directory);
  llvm::sys::path::append(path, label);
  std::error_code ec;
  {
    llvm::raw_fd_ostream output(path, ec, llvm::sys::fs::OF_None);
    if (ec) { llvm::errs() << ec.message() << '\n'; return false; }
    if (bytecode) {
      mlir::MLIRContext context;
      context.getOrLoadDialect<sela::ir::SelaDialect>();
      context.allowUnregisteredDialects(allowUnregisteredFixture);
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
  sela::ArtifactSummary summary;
  auto error = structuralOnly ? sela::inspectArtifactStructure(path, summary)
                              : sela::inspectArtifact(path, summary, targets);
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
  if (auto ec = llvm::sys::fs::createUniqueDirectory("sela-ir-tests", directory)) {
    llvm::errs() << ec.message() << '\n';
    return 1;
  }
  bool passed = true;
  passed &= !sela::supportedNativeTargets().empty();
  // Construct historical wire spelling only in a deliberately unregistered
  // fixture context. The production reader must not register a legacy dialect
  // or silently accept the old schema key under the new product contract.
  const std::string predecessor{char(0x6e), char(0x69), char(0x65), char(0x72)};
  std::string predecessorIR = valid;
  for (size_t offset = 0; (offset = predecessorIR.find("sela.", offset)) != std::string::npos;
       offset += predecessor.size() + 1)
    predecessorIR.replace(offset, 4, predecessor);
  passed &= check(directory, "predecessor-dialect.selabc", predecessorIR, false,
                  true, false, sela::supportedNativeTargets(), false, true);
  passed &= check(directory, "predecessor-schema.selabc",
                  replace(valid, "sela.schema", predecessor + ".schema"), false);
  for (llvm::StringRef target : {"x86_64", "i686"}) {
    bool available = llvm::is_contained(sela::supportedNativeTargets(), target);
    passed &= check(directory, ("native-capability-" + target + ".selabc").str(),
                    valid, available, true, false, {target});
  }
  // Structural inspection has no implicit native target: this remains
  // inspectable even in the i686-only library, where its sext is not legal.
  passed &= check(directory, "structural-width-specific.selabc", widthSpecific,
                  true, true, false, {}, true);
  passed &= check(directory, "structural-invalid-word-flags.selabc",
                  replace(valid, "flags = 0 : i32", "flags = {word64 = 9 : i64, word32 = 0 : i64}"),
                  false, true, false, {}, true);
  passed &= check(directory, "structural-missing-word-domain.selabc",
                  replace(valid, "flags = 0 : i32", "flags = {word32 = 0 : i64}"),
                  false, true, false, {}, true);
  passed &= check(directory, "structural-foreign-array-bound.selabc",
                  replace(validIndexDomain, "word_array<3, 2, i32>", "word_array<4294967296, 2, i32>"),
                  false, true, false, {}, true);
  passed &= check(directory, "structural-foreign-initializer-extent.selabc",
                  replace(validIndexDomain, "word_array<3, 2", "word_array<3, 1"),
                  false, true, false, {}, true);
  passed &= check(directory, "typed-global-unspecified-alignment.selabc",
                  replace(validIndexDomain, "alignment = 4", "alignment = 0"), true);
  passed &= check(directory, "structural-unknown-constant.selabc",
                  replace(valid, "value = 1 : i64", "value = \"unknown_semantics\""),
                  false, true, false, {}, true);
  passed &= check(directory, "structural-unknown-loop-option.selabc",
                  replace(validLoop, "true_count = 1", "loop = [\"llvm.loop.unknown\"], loop_id = \"l0\", true_count = 1"),
                  false, true, false, {}, true);
  passed &= check(directory, "valid.mlirbc", valid, true);
  passed &= check(directory, "independent-native-aggregate-abi.mlirbc", validAggregateABI, true);
  passed &= check(directory, "invalid-native-aggregate-body.mlirbc",
      replace(validAggregateABI, "type = (!sela.ptr, !sela.ptr) -> ()", "type = (!sela.ptr) -> ()"), false);
  passed &= check(directory, "invalid-native-aggregate-scalar-attributes.mlirbc",
      replace(validAggregateABI, "attributes = [[], [], [], []]", "attributes = [[], [], [], [{name = \"noundef\"}]]"), false);
  passed &= check(directory, "invalid-native-aggregate-tail-call.mlirbc",
      replace(validAggregateABI, "tail = 0", "tail = 1"), false);
  passed &= check(directory, "invalid-native-aggregate-signature-kind.mlirbc",
      replace(validAggregateABI, "native_abi = (!sela.record<\"r0\", 0, [i32, i32]>) -> !sela.record<\"r0\", 0, [i32, i32]>",
                                "native_abi = \"opaque native ABI\""), false);
  passed &= check(directory, "valid-available-externally.mlirbc",
                  replace(valid, "internal = false", "available_externally = true, internal = false"), true);
  passed &= check(directory, "conflicting-native-linkage.mlirbc",
                  replace(valid, "internal = false", "available_externally = true, internal = true"), false);
  passed &= check(directory, "invalid-native-linkage-kind.mlirbc",
                  replace(valid, "internal = false", "available_externally = 1 : i32, internal = false"), false);
  passed &= check(directory, "valid-native-byte-swap.mlirbc",
                  replace(valid, "    \"sela.return\"(%2)", "    %swapped = \"sela.bswap\"(%2) : (i32) -> i32\n    \"sela.return\"(%2)"), true);
  passed &= check(directory, "valid-native-index-domain.mlirbc", validIndexDomain, true);
  passed &= check(directory, "invalid-native-index-extent.mlirbc",
                  replace(validIndexDomain, "count = {word64 = 3", "count = {word64 = 4"), false);
  passed &= check(directory, "invalid-native-index-storage.mlirbc",
                  replace(validIndexDomain, "word_array<3, 2", "word_array<3, 1"), false);
  passed &= check(directory, "private-inactive-native-index-tail.mlirbc",
                  replace(validIndexDomain, ", 3 : i64], count", ", {private = \"hidden source\"}], count"), false, true, false, {"i686"});
  passed &= check(directory, "independent-native-varargs.mlirbc", validVarargs, true);
  passed &= check(directory, "independent-native-varargs-forwarding.mlirbc",
                  replace(validVarargs, "    %value =", "    %forward = \"sela.va_forward\"(%state) : (!sela.ptr) -> !sela.ptr\n    %value ="), true);
  passed &= check(directory, "invalid-native-varargs-forwarding-result.mlirbc",
                  replace(validVarargs, "    %value =", "    %forward = \"sela.va_forward\"(%state) : (!sela.ptr) -> i32\n    %value ="), false);
  passed &= check(directory, "unknown-native-vararg-semantics.mlirbc",
                  replace(validVarargs, "\"sela.va_arg\"(%state)", "\"sela.va_arg\"(%state) {future_abi = true}"), false);
  passed &= check(directory, "invalid-native-visibility.mlirbc",
                  replace(valid, "dso_local = true", "visibility = \"invented\", dso_local = true"), false);
  passed &= check(directory, "invalid-native-intrinsic.mlirbc",
                  replace(valid, "dso_local = true", "intrinsic = \"invented\", dso_local = true"), false);
  passed &= check(directory, "valid-frame-pointer-policy.mlirbc",
                  replace(valid, "sela.module_flags = []",
                          "sela.module_flags = [{name = \"frame-pointer\", behavior = 7 : i32, value = 2 : i32, profile = \"both\"}]"), true);
  passed &= check(directory, "invalid-frame-pointer-policy.mlirbc",
                  replace(valid, "sela.module_flags = []",
                          "sela.module_flags = [{name = \"frame-pointer\", behavior = 7 : i32, value = 3 : i32, profile = \"both\"}]"), false);
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
  passed &= check(directory, "declared-x64-domain.mlirbc", widthSpecific, llvm::is_contained(sela::supportedNativeTargets(), llvm::StringRef("x86_64")), true, false, {"x86_64"});
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
                  replace(validCFG, "\"sela.br\"(%left)[^join] : (i32)",
                          "\"sela.br\"()[^join] : ()"), false);
  passed &= check(directory, "invalid-branch-condition-count.mlirbc",
                  replace(validCFG, "true_count = 0", "true_count = 9"), false);
  passed &= check(directory, "independent-no-compiler-flags.mlirbc",
                  replace(valid, ", sela.module_flags = []", ""), true);
  passed &= check(directory, "producer-provenance-is-not-core.mlirbc",
                  replace(valid, "sela.schema = 1", "sela.profiles = [\"x86_64\", \"i686\"], sela.schema = 1"), false);
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
                  replace(valid, "sela.schema = 1", "sela.schema = 2"), false);
  passed &= check(directory, "unknown-module-field.mlirbc",
                  replace(valid, "sela.schema = 1", "sela.private_source = \"secret.c\", sela.schema = 1"), false);
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
    sela::ArtifactSummary summary;
    auto error = sela::inspectArtifactStructure(fifo, summary);
    if (!error) { llvm::errs() << "FIFO input was accepted\n"; passed = false; }
    else llvm::consumeError(std::move(error));
    llvm::sys::fs::remove(fifo);
  }
  for (llvm::StringRef input : {llvm::StringRef("/dev/zero"), llvm::StringRef(directory)}) {
    sela::ArtifactSummary summary;
    auto error = sela::inspectArtifactStructure(input, summary);
    if (!error) { llvm::errs() << "non-regular input was accepted\n"; passed = false; }
    else llvm::consumeError(std::move(error));
  }
  llvm::sys::fs::remove(directory);
  return passed ? 0 : 1;
}
