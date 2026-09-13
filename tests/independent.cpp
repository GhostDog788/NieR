#include "nier/Artifact/Artifact.h"
#include "nier/IR/Compiler.h"
#include "nier/IR/Dialect.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

using namespace nier::driver;
namespace {
const char *mainModule = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({}) {id = "native_bytes", type = () -> !nier.word,
    declaration = true, variadic = false, internal = false, dso_local = false,
    attributes = [[], []]} : () -> ()
  "nier.func"() ({}) {id = "fixed_eight", type = () -> i32,
    declaration = true, variadic = false, internal = false, dso_local = false,
    attributes = [[], []]} : () -> ()
  "nier.func"() ({
    %width = "nier.call"() {callee = "native_bytes", tail = 0 : i32,
      attributes = [[], []]} : () -> !nier.word
    %expected = "nier.constant"() {value = "pointer_bytes"} : () -> !nier.word
    %matches = "nier.compare"(%width, %expected) {predicate = 32 : i32} : (!nier.word, !nier.word) -> i1
    "nier.cond_br"(%matches)[^yes, ^no] {true_count = 0 : i32} : (i1) -> ()
  ^yes:
    %fixed = "nier.call"() {callee = "fixed_eight", tail = 0 : i32,
      attributes = [[], []]} : () -> i32
    %eight = "nier.constant"() {value = 8 : i64} : () -> i32
    %result = "nier.binary"(%fixed, %eight) {opcode = "sub", flags = 0 : i32} : (i32, i32) -> i32
    "nier.return"(%result) : (i32) -> ()
  ^no:
    %failure = "nier.constant"() {value = 1 : i64} : () -> i32
    "nier.return"(%failure) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
    internal = false, dso_local = true, attributes = [[], []]} : () -> ()
}
)mlir";
const char *helperModule = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({
    %width = "nier.constant"() {value = "pointer_bytes"} : () -> !nier.word
    "nier.return"(%width) : (!nier.word) -> ()
  }) {id = "native_bytes", type = () -> !nier.word, declaration = false,
    variadic = false, internal = false, dso_local = true, attributes = [[], []]} : () -> ()
  "nier.func"() ({
    %eight = "nier.constant"() {value = 8 : i64} : () -> i32
    "nier.return"(%eight) : (i32) -> ()
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
    context.getOrLoadDialect<nier::ir::NIERDialect>();
    auto module = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
    if (!module) return fail("cannot construct independent NieR fixture");
    module->walk([&](mlir::Operation *operation) {
      operation->setLoc(mlir::UnknownLoc::get(&context));
      for (auto &region : operation->getRegions())
        for (auto &block : region)
          for (auto argument : block.getArguments()) argument.setLoc(mlir::UnknownLoc::get(&context));
    });
    auto path = scratch->path / "module.nierbc";
    if (auto error = nier::writeModule(*module, path.string(), nier::supportedNativeTargets())) return error;
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
