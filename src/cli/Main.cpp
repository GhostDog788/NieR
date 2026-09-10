#include "Package.h"
#include "Build.h"
#include "aot/IR/Compiler.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <iostream>
#include <set>

using namespace aot::driver;

namespace {
struct Options {
  std::string command;
  fs::path input, recipe, output;
  Sdk sdk;
  bool keepWork = false;
};
void usage() {
  llvm::outs() << "On-target AOT — experimental C checkpoint, not broad-C MVP completion\n"
    "  aot publish --recipe BUILD.json -o APP.aotpkg [--sdk DIR] [--keep-work]\n"
    "  aot inspect APP.aotpkg\n"
    "  aot compile APP.aotpkg --output-dir DIR [--sdk DIR] [--keep-work]\n"
    "  aot lower APP.aotpkg --output-dir DIR [--profile x86_64|i686]\n"
    "Native output is executed directly, without an aot run command.\n";
}
std::string lowerProfile = "x86_64";
llvm::Expected<Options> parseOptions(int argc, char **argv) {
  if (argc < 2) return fail("expected a command; use aot --help");
  Options options;
  options.command = argv[1];
  const char *envSdk = std::getenv("AOT_SDK_ROOT");
  options.sdk.root = fs::absolute(envSdk ? envSdk : AOT_DEFAULT_SDK);
  for (int i = 2; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--keep-work") { options.keepWork = true; continue; }
    if (argument == "--sdk" || argument == "--recipe" || argument == "-o" || argument == "--output-dir" || argument == "--profile") {
      if (++i == argc) return fail("missing value after " + argument);
      if (argument == "--sdk") options.sdk.root = fs::absolute(argv[i]);
      else if (argument == "--recipe") options.recipe = fs::absolute(argv[i]);
      else if (argument == "--profile") lowerProfile = argv[i];
      else options.output = fs::absolute(argv[i]);
    } else if (!argument.empty() && argument[0] != '-' && options.input.empty()) options.input = fs::absolute(argument);
    else return fail("unexpected argument: " + argument);
  }
  return options;
}
llvm::Expected<std::vector<std::string>> stringArray(const llvm::json::Object &object, llvm::StringRef key, bool required = false) {
  std::vector<std::string> result;
  auto *value = object.get(key);
  if (!value && !required) return result;
  auto *array = value ? value->getAsArray() : nullptr;
  if (!array) return fail("recipe field must be an array: " + key.str());
  for (auto &element : *array) {
    auto string = element.getAsString();
    if (!string || string->contains('\0')) return fail("invalid string in " + key.str());
    result.push_back(string->str());
  }
  return result;
}
llvm::Error publish(const Options &options) {
  if (options.recipe.empty() || options.output.empty()) return fail("publish requires --recipe and -o");
  if (auto error = options.sdk.validate(true)) return error;
  auto recipe = readJson(options.recipe);
  if (!recipe) return recipe.takeError();
  auto *object = recipe->getAsObject();
  if (!object) return fail("build recipe must be an object");
  for (auto &entry : *object)
    if (entry.first != "name" && entry.first != "sources" && entry.first != "build" && entry.first != "cflags" && entry.first != "libraries")
      return fail("unsupported recipe field in this checkpoint: " + entry.first.str());
  auto name = object->getString("name");
  if (!name || !validName(*name)) return fail("recipe needs a simple application name");
  bool existingBuild = object->get("build") != nullptr;
  if (existingBuild && object->get("sources")) return fail("choose sources or build, not both");
  auto sources = stringArray(*object, "sources", !existingBuild);
  if (!sources) return sources.takeError();
  auto cflags = stringArray(*object, "cflags");
  if (!cflags) return cflags.takeError();
  auto libraries = stringArray(*object, "libraries");
  if (!libraries) return libraries.takeError();
  if ((!existingBuild && sources->empty()) || sources->size() > 128) return fail("recipe requires 1..128 translation units");
  std::string optimization = "O2";
  for (size_t i = 0; i < cflags->size(); ++i) {
    llvm::StringRef flag((*cflags)[i]);
    if (flag.starts_with("-Wl,") || flag.starts_with("-Wa,") || flag.starts_with("-Wp,"))
      return fail("unqualified pass-through compiler flag: " + flag.str());
    if (flag == "-O0" || flag == "-O1" || flag == "-O2" || flag == "-O3" || flag == "-Os" || flag == "-Oz") optimization = flag.drop_front().str();
    else if (flag.starts_with("-D") || flag.starts_with("-U") || flag.starts_with("-I") || flag.starts_with("-W") ||
             flag == "-std=c11" || flag == "-std=gnu11" || flag == "-std=c17" || flag == "-std=gnu17" || flag == "-std=c99" || flag == "-std=gnu99" ||
             flag == "-fno-builtin" || flag == "-fwrapv" || flag == "-fno-strict-aliasing" || flag == "-fno-omit-frame-pointer") {
      if (flag == "-I" || flag == "-D" || flag == "-U") { if (++i == cflags->size()) return fail("missing flag argument"); }
    } else return fail("unqualified compiler flag: " + flag.str());
  }
  for (auto &library : *libraries) if (library != "m") return fail("unqualified managed library: " + library);
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  scratch->keep = options.keepWork;
  if (options.keepWork) llvm::outs() << "Private publisher workspace: " << scratch->path.string() << '\n';
  PackageFiles files;
  llvm::json::Array modules;
  std::vector<CapturedUnit> units;
  if (existingBuild) {
    auto *build = object->getObject("build");
    if (!build) return fail("build must be an object");
    if (!libraries->empty()) return fail("existing builds declare libraries through their native link commands");
    auto captured = captureBuild(*build, options.sdk, options.recipe.parent_path(), scratch->path, *cflags);
    if (!captured) return captured.takeError();
    units = std::move(captured->units);
    *libraries = std::move(captured->libraries);
  }
  for (size_t i = 0; i < sources->size(); ++i) {
    fs::path source = fs::absolute(options.recipe.parent_path() / (*sources)[i]);
    if (!fs::is_regular_file(source) || source.extension() != ".c") return fail("checkpoint requires an existing .c translation unit: " + source.string());
    fs::path capture64, capture32;
    for (const char *profile : {"x86_64", "i686"}) {
      fs::path directory = scratch->path / profile;
      fs::create_directories(directory);
      fs::path bitcode = directory / (std::to_string(i) + ".bc");
      if (std::string(profile) == "x86_64") capture64 = bitcode; else capture32 = bitcode;
      std::vector<std::string> args{options.sdk.tool("clang").string()};
      auto targetFlags = options.sdk.compileFlags(profile);
      args.insert(args.end(), targetFlags.begin(), targetFlags.end());
      args.insert(args.end(), {"-O2", "-fPIC"});
      args.insert(args.end(), cflags->begin(), cflags->end());
      args.insert(args.end(), {"-g", "-fstandalone-debug", std::string("-fpass-plugin=") + AOT_CAPTURE_PLUGIN,
          "-c", source.string(), "-o", (directory / (std::to_string(i) + ".o")).string()});
      if (auto error = run(args, options.recipe.parent_path(), {{"AOT_CAPTURE_PATH", bitcode.string()}})) return error;
      if (!fs::is_regular_file(bitcode)) return fail("stock compiler did not execute the qualified capture hook");
    }
    units.push_back({capture64, capture32, optimization});
  }
  for (size_t i = 0; i < units.size(); ++i) {
    fs::path merged = scratch->path / (std::to_string(i) + ".mlirbc");
    aot::ArtifactSummary summary;
    if (auto error = aot::mergeProfiles(units[i].x64Path.string(), units[i].i686Path.string(), merged.string(), &summary)) return error;
    auto bytes = read(merged);
    if (!bytes) return bytes.takeError();
    std::string path = "modules/" + std::to_string(i) + ".mlirbc";
    modules.push_back(llvm::json::Object{{"path", path}, {"sha256", digest(*bytes)}, {"optimization", units[i].optimization}});
    files.emplace(path, std::move(*bytes));
    llvm::outs() << "Merged translation unit " << i << ": " << summary.functions << " functions, " << summary.operations << " operations\n";
  }
  llvm::json::Array publicLibraries;
  for (auto &library : *libraries) publicLibraries.push_back(library);
  files["manifest.json"] = jsonText(llvm::json::Object{{"format_version", 1}, {"contract", Contract}, {"name", *name},
    {"profiles", llvm::json::Array{"x86_64", "i686"}}, {"runtime", "glibc-2.39-0ubuntu8.8"},
    {"libraries", std::move(publicLibraries)}, {"modules", std::move(modules)}});
  auto checked = validatePackage(files);
  if (!checked) return checked.takeError();
  if (auto error = writePackage(options.output, files)) return error;
  llvm::outs() << "Published " << options.output.string() << '\n';
  return llvm::Error::success();
}
llvm::Error inspect(const Options &options) {
  if (options.input.empty()) return fail("inspect requires a package");
  auto files = readPackage(options.input);
  if (!files) return files.takeError();
  auto manifest = validatePackage(*files);
  if (!manifest) return manifest.takeError();
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  auto *object = manifest->getAsObject();
  llvm::outs() << "Artifact: " << *object->getString("name") << "\nContract: " << Contract
    << "\nCapture profiles: x86_64, i686\nDevice execution qualified: x86_64\n";
  for (auto &module : *object->getArray("modules")) {
    std::string name = module.getAsObject()->getString("path")->str();
    fs::path input = scratch->path / "module.mlirbc";
    if (auto error = write(input, files->at(name))) return error;
    aot::ArtifactSummary summary;
    if (auto error = aot::inspectArtifact(input.string(), summary)) return error;
    llvm::outs() << name << ": " << summary.functions << " functions, " << summary.globals << " globals, "
      << summary.operations << " operations, " << summary.symbolicTypes << " symbolic types, " << summary.symbolicConstants << " symbolic constants\n";
  }
  return llvm::Error::success();
}
llvm::Error compile(const Options &options, bool lowerOnly) {
  if (options.input.empty() || options.output.empty()) return fail("compile/lower requires a package and --output-dir");
  if (lowerProfile != "x86_64" && lowerProfile != "i686") return fail("unknown profile");
  if (!lowerOnly && lowerProfile != "x86_64") return fail("device execution is qualified only for x86_64");
  if (!lowerOnly) if (auto error = options.sdk.validate()) return error;
  auto files = readPackage(options.input);
  if (!files) return files.takeError();
  auto manifest = validatePackage(*files);
  if (!manifest) return manifest.takeError();
  if (fs::exists(options.output)) return fail("output directory already exists; will not overwrite native outputs");
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  scratch->keep = options.keepWork;
  if (options.keepWork) llvm::outs() << "Device compiler workspace: " << scratch->path.string() << '\n';
  auto *object = manifest->getAsObject();
  std::vector<fs::path> objects;
  std::vector<std::pair<std::string, fs::path>> outputs;
  size_t index = 0;
  for (auto &module : *object->getArray("modules")) {
    auto *record = module.getAsObject();
    std::string member = record->getString("path")->str();
    std::string stem = std::to_string(index++);
    fs::path bytecode = scratch->path / (stem + ".mlirbc");
    fs::path nativeIR = scratch->path / (stem + ".ll");
    if (auto error = write(bytecode, files->at(member))) return error;
    if (auto error = aot::lowerArtifact(bytecode.string(), lowerProfile, nativeIR.string())) return error;
    if (lowerOnly) { outputs.emplace_back(stem + ".ll", nativeIR); continue; }
    fs::path optimized = scratch->path / (stem + ".opt.bc");
    std::string level = record->getString("optimization")->str();
    if (auto error = run({options.sdk.tool("opt").string(), "-passes=default<" + level + ">", "-verify-each", nativeIR.string(), "-o", optimized.string()})) return error;
    fs::path nativeObject = scratch->path / (stem + ".o");
    std::string backendLevel = level == "O0" ? "0" : level == "O1" ? "1" : level == "O3" ? "3" : "2";
    if (auto error = run({options.sdk.tool("llc").string(), "-O=" + backendLevel, "-filetype=obj", "-relocation-model=pic", optimized.string(), "-o", nativeObject.string()})) return error;
    objects.push_back(nativeObject);
  }
  if (!lowerOnly) {
    std::vector<std::string> libraries;
    for (auto &library : *object->getArray("libraries")) libraries.push_back(library.getAsString()->str());
    fs::path executable = scratch->path / object->getString("name")->str();
    auto command = options.sdk.linkCommand("x86_64", objects, executable, libraries);
    if (!command) return command.takeError();
    if (auto error = run(*command)) return error;
    outputs.emplace_back("bin/" + object->getString("name")->str(), executable);
  }
  fs::create_directories(options.output);
  for (auto &output : outputs) {
    fs::path destination = options.output / output.first;
    fs::create_directories(destination.parent_path());
    fs::copy_file(output.second, destination);
    llvm::outs() << (lowerOnly ? "Lowered " : "Native executable: ") << destination.string() << '\n';
  }
  return llvm::Error::success();
}
}

int main(int argc, char **argv) {
  try {
    std::string invocation = fs::path(argv[0]).filename().string();
    if (invocation == "aot-clang" || invocation == "aot-ar" || invocation == "aot-ranlib" || invocation == "aot-ld")
      return wrapperMain(argc, argv);
    if (argc == 2 && std::string(argv[1]) == "--help") { usage(); return 0; }
    auto options = parseOptions(argc, argv);
    if (!options) { llvm::logAllUnhandledErrors(options.takeError(), llvm::errs(), "aot: "); return 1; }
    llvm::Error error = llvm::Error::success();
    if (options->command == "publish") error = publish(*options);
    else if (options->command == "inspect") error = inspect(*options);
    else if (options->command == "compile") error = compile(*options, false);
    else if (options->command == "lower") error = compile(*options, true);
    else error = fail("unknown command: " + options->command);
    if (error) { llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "aot: "); return 1; }
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "aot: " << error.what() << '\n';
    return 1;
  }
}
