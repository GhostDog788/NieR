#include "nier/Artifact/Artifact.h"
#include "nier/IR/Compiler.h"
#include "nier/IR/Dialect.h"
#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

using namespace nier::driver;
namespace {
constexpr const char *ConditionalModule = R"mlir(
module attributes {nier.schema = 1 : i32} {
  "nier.func"() ({
    %selector = "nier.constant"() {value = 0 : i64} : () -> i32
    "nier.switch"(%selector)[^ordinary, ^guarded] {cases = [8 : i64],
      case_domains = [1 : i32], argument_counts = array<i32: 0, 0>} : (i32) -> ()
  ^ordinary:
    "nier.br"()[^done] : () -> ()
  ^guarded:
    %one = "nier.constant"() {value = 1 : i64} : () -> i32
    %two = "nier.constant"() {value = 2 : i64} : () -> i32
    %sum = "nier.binary"(%one, %two) {opcode = "add", flags = 0 : i32} : (i32, i32) -> i32
    "nier.br"()[^done] : () -> ()
  ^done:
    %zero = "nier.constant"() {value = 0 : i64} : () -> i32
    "nier.return"(%zero) : (i32) -> ()
  }) {id = "main", type = () -> i32, declaration = false, variadic = false,
    internal = false, dso_local = true, attributes = [[], []],
    block_domains = [3 : i32, 3 : i32, 1 : i32, 3 : i32]} : () -> ()
}
)mlir";

llvm::Expected<std::string> malformedDomain(bool word64) {
  mlir::MLIRContext context;
  context.getOrLoadDialect<nier::ir::NIERDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(ConditionalModule, &context);
  if (!module) return fail("cannot construct conditional consumer fixture");
  mlir::Builder builder(&context);
  mlir::Operation *binary = nullptr;
  const unsigned domain = word64 ? 1 : 2;
  module->walk([&](mlir::Operation *operation) {
    operation->setLoc(builder.getUnknownLoc());
    if (operation->getName().getStringRef() == "nier.func")
      operation->setAttr("block_domains", builder.getArrayAttr({builder.getI32IntegerAttr(3),
          builder.getI32IntegerAttr(3), builder.getI32IntegerAttr(domain), builder.getI32IntegerAttr(3)}));
    if (operation->getName().getStringRef() == "nier.switch")
      operation->setAttr("case_domains", builder.getArrayAttr({builder.getI32IntegerAttr(domain)}));
    if (operation->getName().getStringRef() == "nier.binary") binary = operation;
  });
  // First prove the complete pristine fixture on both native implementations.
  // Only then introduce the one intentional public-schema fault.
  if (auto error = nier::verifyModule(*module, {"x86_64", "i686"})) return std::move(error);
  if (!binary) return fail("conditional fixture has no mutation anchor");
  binary->setAttr("flags", builder.getDictionaryAttr({
      builder.getNamedAttr("word64", builder.getI64IntegerAttr(word64 ? 8 : 0)),
      builder.getNamedAttr("word32", builder.getI64IntegerAttr(word64 ? 0 : 8))}));
  if (mlir::failed(mlir::verify(*module))) return fail("negative fixture is not structurally readable MLIR");
  auto expected = nier::verifyModuleStructure(*module);
  if (!expected) return fail("negative word-domain fixture was unexpectedly admitted");
  if (llvm::toString(std::move(expected)).find("invalid arithmetic flags") == std::string::npos)
    return fail("negative fixture failed for an unrelated reason");
  // Tests intentionally serialize invalid public semantics. Production writers
  // continue to require explicit, successful native validation.
  std::string bytes;
  llvm::raw_string_ostream stream(bytes);
  if (mlir::failed(mlir::writeBytecodeToFile(*module, stream)))
    return fail("cannot serialize controlled negative fixture");
  stream.flush();
  return bytes;
}

llvm::Error generate(const fs::path &input, const fs::path &output) {
  const std::vector<std::string> names = {"foreign-only-x86_64.nier", "foreign-only-i686.nier",
      "malformed-inactive-word64.nier", "malformed-inactive-word32.nier"};
  for (const auto &name : names)
    if (fs::exists(output / name) || fs::is_symlink(output / name))
      return fail("fixture output already exists: " + (output / name).string());
  auto files = readPackage(input);
  if (!files) return files.takeError();
  auto manifest = validatePackage(*files);
  if (!manifest) return manifest.takeError();
  auto &object = *manifest->getAsObject();
  if (object.getString("kind") != "executable") return fail("expected executable Hello artifact");
  auto plan = readCompilationPlan(object);
  if (!plan) return plan.takeError();
  if (!plan->count("x86_64") || !plan->count("i686"))
    return fail("Hello fixture must admit both native targets");
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  std::vector<ArtifactModule> modules;
  for (auto &entry : *object.getArray("modules")) {
    auto member = entry.getAsObject()->getString("path")->str();
    auto temporary = scratch->path / "checked.nierbc";
    if (auto error = write(temporary, files->at(member))) return error;
    nier::ArtifactSummary summary;
    if (auto error = nier::inspectArtifact(temporary.string(), summary, {"x86_64", "i686"})) return error;
    modules.push_back({files->at(member)});
  }
  std::vector<std::string> libraries, options;
  for (auto &entry : *object.getArray("libraries")) libraries.push_back(entry.getAsString()->str());
  for (auto &entry : *object.getArray("link_options")) options.push_back(entry.getAsString()->str());
  llvm::StringRef versionScript;
  if (object.get("version_script")) versionScript = files->at("link/version.script");
  fs::create_directories(output);
  for (const auto &target : {std::string("x86_64"), std::string("i686")}) {
    CompilationPlan selected{{target, plan->at(target)}};
    auto artifact = createArtifact("executable", modules, libraries, options, {target}, versionScript, selected);
    if (!artifact) return artifact.takeError();
    if (auto error = writePackage(output / ("foreign-only-" + target + ".nier"), *artifact)) return error;
  }
  for (bool word64 : {true, false}) {
    auto bytes = malformedDomain(word64);
    if (!bytes) return bytes.takeError();
    auto artifact = createArtifact("executable", {{std::move(*bytes), "O0"}});
    if (!artifact) return artifact.takeError();
    auto name = word64 ? "malformed-inactive-word64.nier" : "malformed-inactive-word32.nier";
    if (auto error = writePackage(output / name, *artifact)) return error;
  }
  return llvm::Error::success();
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  try {
    if (auto error = generate(argv[1], argv[2])) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "consumer artifact fixtures: ");
      return 1;
    }
  } catch (const std::exception &error) {
    llvm::errs() << "consumer artifact fixtures: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
