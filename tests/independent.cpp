#include "sela/Artifact/Artifact.h"
#include "sela/IR/Compiler.h"
#include "sela/IR/Dialect.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

using namespace sela::driver;
namespace {
const char *mainModule = R"mlir(
module attributes {sela.schema = 1 : i32, sela.targets = ["x86_64", "i686", "armv7", "aarch64"]} {
  "sela.func"() ({}) {id = "native_bytes", type = () -> !sela.word,
    declaration = true, variadic = false, internal = false, dso_local = false,
    attributes = [[], []]} : () -> ()
  "sela.func"() ({}) {id = "fixed_eight", type = () -> i32,
    declaration = true, variadic = false, internal = false, dso_local = false,
    attributes = [[], []]} : () -> ()
  "sela.func"() ({
    %width = "sela.call"() {callee = "native_bytes", tail = 0 : i32,
      attributes = [[], []]} : () -> !sela.word
    %expected = "sela.constant"() {value = "pointer_bytes"} : () -> !sela.word
    %matches = "sela.compare"(%width, %expected) {predicate = 32 : i32} : (!sela.word, !sela.word) -> i1
    "sela.cond_br"(%matches)[^yes, ^no] {true_count = 0 : i32} : (i1) -> ()
  ^yes:
    %fixed = "sela.call"() {callee = "fixed_eight", tail = 0 : i32,
      attributes = [[], []]} : () -> i32
    %eight = "sela.constant"() {value = 8 : i64} : () -> i32
    %result = "sela.binary"(%fixed, %eight) {opcode = "sub", flags = 0 : i32} : (i32, i32) -> i32
    "sela.return"(%result) : (i32) -> ()
  ^no:
    %failure = "sela.constant"() {value = 1 : i64} : () -> i32
    "sela.return"(%failure) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
    internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";
const char *helperModule = R"mlir(
module attributes {sela.schema = 1 : i32, sela.targets = ["x86_64", "i686", "armv7", "aarch64"]} {
  "sela.func"() ({
    %width = "sela.constant"() {value = "pointer_bytes"} : () -> !sela.word
    "sela.return"(%width) : (!sela.word) -> ()
  }) {id = "native_bytes", type = () -> !sela.word, declaration = false,
    variadic = false, internal = false, dso_local = true, attributes = [[], []]} : () -> ()
  "sela.func"() ({
    %eight = "sela.constant"() {value = 8 : i64} : () -> i32
    "sela.return"(%eight) : (i32) -> ()
  }) {id = "fixed_eight", type = () -> i32, declaration = false,
    variadic = false, internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";
llvm::Error produce(const fs::path &output) {
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  std::vector<ArtifactModule> modules;
  for (auto text : {mainModule, helperModule}) {
    mlir::MLIRContext context;
    context.getOrLoadDialect<sela::ir::SelaDialect>();
    auto module = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
    if (!module) return fail("cannot construct independent Sela fixture");
    module->walk([&](mlir::Operation *operation) {
      operation->setLoc(mlir::UnknownLoc::get(&context));
      for (auto &region : operation->getRegions())
        for (auto &block : region)
          for (auto argument : block.getArguments()) argument.setLoc(mlir::UnknownLoc::get(&context));
    });
    auto path = scratch->path / "module.selabc";
    if (auto error = sela::writeModule(*module, path.string(), sela::supportedNativeTargets())) return error;
    auto bytes = read(path);
    if (!bytes) return bytes.takeError();
    modules.push_back({std::move(*bytes), "O2"});
  }
  auto artifact = createArtifact("executable", modules);
  if (!artifact) return artifact.takeError();
  return writePackage(output, *artifact);
}
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  if (auto error = produce(argv[1])) {
    llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "independent producer: "); return 1;
  }
  return 0;
}
