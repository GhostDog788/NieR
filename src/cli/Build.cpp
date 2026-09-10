#include "Build.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdlib>
#include <set>
#include <unistd.h>

namespace aot::driver {
namespace {
fs::path absolutePath(const fs::path &path) {
  return fs::weakly_canonical(fs::absolute(path));
}
bool inside(const fs::path &path, const fs::path &root) {
  auto relative = path.lexically_relative(root);
  return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
llvm::Expected<std::vector<std::string>> strings(const llvm::json::Object &object,
                                               llvm::StringRef key) {
  std::vector<std::string> result;
  auto value = object.get(key);
  if (!value) return result;
  auto array = value->getAsArray();
  if (!array) return fail("build." + key.str() + " must be an array of strings");
  for (auto &item : *array) {
    auto text = item.getAsString();
    if (!text || text->contains('\0')) return fail("invalid string in build." + key.str());
    result.push_back(text->str());
  }
  return result;
}
std::string normalize(std::string text, const fs::path &root) {
  std::string prefix = root.string();
  size_t position = 0;
  while ((position = text.find(prefix, position)) != std::string::npos) {
    text.replace(position, prefix.size(), "$PRIVATE");
    position += 8;
  }
  return text;
}
llvm::Error copySource(const fs::path &source, const fs::path &destination,
                       const fs::path &root) {
  fs::create_directories(destination);
  for (const auto &entry : fs::directory_iterator(source)) {
    auto name = entry.path().filename();
    if (name == ".git" || name == ".sdk") continue;
    auto target = destination / name;
    if (entry.is_symlink()) {
      auto resolved = fs::canonical(entry.path());
      if (!inside(resolved, root) || !fs::is_regular_file(resolved))
        return fail("build source directory/external symlinks need explicit integration: " + entry.path().string());
      fs::copy_file(resolved, target);
    } else if (entry.is_directory()) {
      if (auto error = copySource(entry.path(), target, root)) return error;
    } else if (entry.is_regular_file()) {
      fs::copy_file(entry.path(), target);
    } else return fail("unsupported special file in build source: " + entry.path().string());
  }
  return llvm::Error::success();
}
llvm::Error atomicJson(const fs::path &path, llvm::json::Value value) {
  fs::path temporary = path.string() + "." + std::to_string(getpid()) + ".tmp";
  if (auto error = write(temporary, jsonText(std::move(value)))) return error;
  fs::rename(temporary, path);
  return llvm::Error::success();
}
fs::path metadataPath(const fs::path &directory, const fs::path &output,
                      llvm::StringRef kind) {
  return directory / (digest(absolutePath(output).string()) + "." + kind.str() + ".json");
}
llvm::Expected<std::vector<std::string>> expandResponses(
    const std::vector<std::string> &arguments, unsigned depth = 0) {
  if (depth > 8) return fail("response-file nesting exceeds capture limit");
  std::vector<std::string> result;
  for (const auto &argument : arguments) {
    if (argument.empty() || argument.front() != '@') { result.push_back(argument); continue; }
    auto contents = read(argument.substr(1), 1024 * 1024);
    if (!contents) return contents.takeError();
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver saver(allocator);
    llvm::SmallVector<const char *, 32> tokens;
    llvm::cl::TokenizeGNUCommandLine(*contents, saver, tokens);
    std::vector<std::string> nested(tokens.begin(), tokens.end());
    auto expanded = expandResponses(nested, depth + 1);
    if (!expanded) return expanded.takeError();
    result.insert(result.end(), expanded->begin(), expanded->end());
    if (result.size() > 16384) return fail("too many compiler arguments");
  }
  return result;
}
struct Invocation {
  std::vector<std::string> compileFlags;
  std::vector<std::string> libraries;
  std::vector<fs::path> sources, objects;
  fs::path output;
  std::string optimization = "O0";
  bool compileOnly = false;
  bool query = false;
};
llvm::Expected<Invocation> parseInvocation(const std::vector<std::string> &args) {
  Invocation result;
  const std::set<std::string> valued = {"-I", "-isystem", "-iquote", "-idirafter",
    "-include", "-imacros", "-D", "-U", "-MF", "-MT", "-MQ", "-x"};
  const std::set<std::string> ordinaryFlags = {"-v", "-w", "-pipe", "-pedantic", "-pedantic-errors",
    "-ansi", "-MD", "-MMD", "-MP", "-MG", "-fPIC", "-fpic", "-fPIE", "-fpie",
    "-fno-builtin", "-fwrapv", "-fno-strict-aliasing", "-fstrict-aliasing",
    "-fno-omit-frame-pointer", "-fomit-frame-pointer", "-fcommon", "-fno-common",
    "-fstack-protector", "-fstack-protector-strong", "-fstack-protector-all", "-fno-stack-protector",
    "-fcolor-diagnostics", "-fno-color-diagnostics", "-fno-caret-diagnostics", "-Qunused-arguments"};
  for (size_t i = 0; i < args.size(); ++i) {
    llvm::StringRef arg(args[i]);
    if (arg == "-o") {
      if (++i == args.size()) return fail("compiler -o requires an output path");
      result.output = args[i];
    } else if (arg.starts_with("-o") && arg.size() > 2) result.output = arg.drop_front(2).str();
    else if (arg == "-c") result.compileOnly = true;
    else if (valued.count(arg.str())) {
      result.compileFlags.push_back(arg.str());
      if (++i == args.size()) return fail("missing compiler flag value");
      if (arg == "-x" && args[i] != "c") return fail("build capture currently accepts only C");
      result.compileFlags.push_back(args[i]);
    } else if (arg == "-E" || arg == "-M" || arg == "-MM" || arg == "-fsyntax-only" ||
               arg == "--version" || arg == "-dumpmachine" || arg == "-dumpversion" ||
               arg == "-###" || arg.starts_with("-print-")) {
      result.query = true;
      result.compileFlags.push_back(arg.str());
    } else if (arg == "-O" || arg == "-O0" || arg == "-O1" || arg == "-O2" ||
               arg == "-O3" || arg == "-Os" || arg == "-Oz") {
      result.optimization = arg == "-O" ? "O1" : arg.drop_front().str();
      result.compileFlags.push_back(arg.str());
    } else if (arg.starts_with("-l")) {
      std::string library = arg.drop_front(2).str();
      if (library != "m" && library != "c") return fail("unqualified native build library: " + library);
      if (library != "c") result.libraries.push_back(library);
    } else if (arg == "-shared" || arg == "-static" || arg == "-r" || arg == "-S" ||
               arg == "-pthread" || arg.starts_with("-Wl,") || arg.starts_with("-Wa,") ||
               arg.starts_with("-Wp,") || arg.starts_with("-L") ||
               arg.starts_with("-m") || arg.starts_with("-O") || arg.starts_with("--target") ||
               arg.starts_with("-target") || arg.starts_with("--sysroot") || arg.starts_with("-isysroot") ||
               arg.starts_with("-resource-dir") || arg.starts_with("-fpass-plugin") ||
               arg.starts_with("-flto") || arg.starts_with("-fuse-ld") || arg.starts_with("-fplugin") ||
               arg.starts_with("-X") || arg.starts_with("-B") || arg.starts_with("-nostd") ||
               arg.starts_with("-nodefault") || arg.starts_with("--gcc")) {
      return fail("unqualified compiler/link option in existing-build checkpoint: " + arg.str());
    } else if (ordinaryFlags.count(arg.str()) || arg.starts_with("-D") || arg.starts_with("-U") ||
               arg.starts_with("-I") || arg.starts_with("-W") || arg.starts_with("-g") ||
               arg.starts_with("-std=") || arg.starts_with("-fvisibility=") ||
               arg.starts_with("-fno-builtin-") || arg.starts_with("-fdiagnostics-color=") ||
               arg.starts_with("-ferror-limit=") || arg.starts_with("-fmessage-length="))
      result.compileFlags.push_back(arg.str());
    else if (arg.starts_with("-"))
      return fail("unknown compiler/link flag cannot be silently replayed: " + arg.str());
    else {
      fs::path input(arg.str());
      if (input.extension() == ".c") result.sources.push_back(absolutePath(input));
      else if (input.extension() == ".o") result.objects.push_back(absolutePath(input));
      else return fail("existing-build checkpoint needs direct .c/.o inputs; archives, shared libraries, assembly and other inputs are unqualified: " + input.string());
    }
  }
  if (result.sources.empty() && result.objects.empty()) result.query = true;
  return result;
}
llvm::Expected<std::vector<std::string>> nativeLink(
    const Sdk &sdk, llvm::StringRef profile, const std::vector<fs::path> &objects,
    const fs::path &output, const std::vector<std::string> &libraries) {
  fs::path target = sdk.sysroot(profile);
  bool x64 = profile == "x86_64";
  fs::path lib = target / "usr/lib" / (x64 ? "x86_64-linux-gnu" : "i386-linux-gnu");
  fs::path loader = lib / (x64 ? "ld-linux-x86-64.so.2" : "ld-linux.so.2");
  fs::path runtime = sdk.root / "host/usr/lib/llvm-18/lib/clang/18/lib/linux";
  std::string suffix = x64 ? "x86_64" : "i386";
  std::vector<std::string> args{sdk.tool("ld.lld").string(), "--sysroot=" + target.string(),
    "-m", x64 ? "elf_x86_64" : "elf_i386", "-pie", "--eh-frame-hdr", "--hash-style=gnu",
    "--dynamic-linker=" + loader.string(), "--enable-new-dtags", "-rpath", lib.string(),
    "-z", "nodefaultlib", "-z", "relro", "-z", "now", "-o", output.string(),
    (lib / "Scrt1.o").string(), (lib / "crti.o").string(),
    (runtime / ("clang_rt.crtbegin-" + suffix + ".o")).string()};
  for (auto &object : objects) args.push_back(object.string());
  args.push_back("-L" + lib.string());
  for (auto &library : libraries) args.push_back("-l" + library);
  args.insert(args.end(), {(runtime / ("libclang_rt.builtins-" + suffix + ".a")).string(),
    "-lc", (runtime / ("clang_rt.crtend-" + suffix + ".o")).string(), (lib / "crtn.o").string()});
  return args;
}
llvm::Error compilerWrapper(const std::vector<std::string> &arguments,
                             const Sdk &sdk, const std::string &profile,
                             const fs::path &lane, const fs::path &metadata) {
  auto expanded = expandResponses(arguments);
  if (!expanded) return expanded.takeError();
  if (const char *extra = std::getenv("AOT_BUILD_EXTRA_FLAGS")) {
    auto value = llvm::json::parse(extra);
    if (!value) return value.takeError();
    auto array = value->getAsArray();
    if (!array) return fail("invalid private build flags");
    for (auto &item : *array) {
      auto flag = item.getAsString();
      if (!flag || flag->contains('\0')) return fail("invalid private compiler flag");
      expanded->push_back(flag->str());
    }
  }
  auto invocation = parseInvocation(*expanded);
  if (!invocation) return invocation.takeError();
  auto allowedOutput = [&](const fs::path &path) {
    auto output = absolutePath(path);
    return output == "/dev/null" || inside(output, lane);
  };
  if (!invocation->output.empty() && !allowedOutput(invocation->output))
    return fail("compiler output escaped its private profile tree");
  for (size_t i = 0; i + 1 < invocation->compileFlags.size(); ++i)
    if (invocation->compileFlags[i] == "-MF" && !allowedOutput(invocation->compileFlags[i + 1]))
      return fail("dependency output escaped its private profile tree");
  if (invocation->query) {
    std::vector<std::string> command{sdk.tool("clang").string()};
    auto target = sdk.compileFlags(profile);
    command.insert(command.end(), target.begin(), target.end());
    command.insert(command.end(), expanded->begin(), expanded->end());
    return run(command);
  }
  if (invocation->sources.size() > 1)
    return fail("capture currently requires one C source per compiler invocation");
  if (invocation->compileOnly && invocation->sources.size() != 1)
    return fail("compile-only capture requires exactly one C source");
  fs::path output = invocation->output;
  if (output.empty()) output = invocation->compileOnly ? invocation->sources.front().stem().string() + ".o" : "a.out";
  output = absolutePath(output);
  if (!inside(output, lane)) return fail("build output escaped its private profile tree: " + output.string());
  std::vector<fs::path> objects = invocation->objects;
  if (!invocation->sources.empty()) {
    fs::path source = invocation->sources.front();
    // CMake's ABI/type probes may compile its own SDK-owned source files. They
    // are read-only inputs and still produce all outputs inside the lane.
    if (!inside(source, lane) && !inside(source, absolutePath(sdk.root)))
      return fail("C input outside private source/build tree or SDK: " + source.string());
    fs::path object = invocation->compileOnly ? output : metadata / ("combined-" + std::to_string(getpid()) + ".o");
    fs::path bitcode = metadata / (digest(object.string()) + "." + std::to_string(getpid()) + ".bc");
    std::vector<std::string> command{sdk.tool("clang").string()};
    auto target = sdk.compileFlags(profile);
    command.insert(command.end(), target.begin(), target.end());
    command.insert(command.end(), invocation->compileFlags.begin(), invocation->compileFlags.end());
    command.insert(command.end(), {"-fPIC", "-g", "-fstandalone-debug",
      std::string("-fpass-plugin=") + AOT_CAPTURE_PLUGIN, "-c", source.string(), "-o", object.string()});
    if (auto error = run(command, {}, {{"AOT_CAPTURE_PATH", bitcode.string()}})) return error;
    if (!fs::is_regular_file(bitcode)) return fail("stock compiler did not run the capture plugin");
    auto native = read(object);
    if (!native) return native.takeError();
    llvm::json::Array flags;
    for (auto &flag : invocation->compileFlags) flags.push_back(normalize(flag, lane));
    std::string key = invocation->compileOnly ? normalize(object.string(), lane) : normalize(output.string(), lane) + ":source";
    if (auto error = atomicJson(metadataPath(metadata, object, "compile"), llvm::json::Object{
      {"source", normalize(source.string(), lane)}, {"key", key}, {"flags", std::move(flags)},
      {"optimization", invocation->optimization}, {"native_sha256", digest(*native)},
      {"capture", bitcode.string()}})) return error;
    // Combined source/object invocations have positional link semantics. Their
    // source must be the only link input until positional replacement is modeled.
    if (!objects.empty()) return fail("mixed source/object link invocation is not yet qualified");
    objects.push_back(object);
  }
  if (invocation->compileOnly) return llvm::Error::success();
  for (const auto &object : objects)
    if (!inside(object, lane)) return fail("link input outside private build tree: " + object.string());
  auto command = nativeLink(sdk, profile, objects, output, invocation->libraries);
  if (!command) return command.takeError();
  if (std::find(invocation->compileFlags.begin(), invocation->compileFlags.end(), "-v") != invocation->compileFlags.end()) {
    for (const auto &argument : *command) llvm::errs() << ' ' << '"' << argument << '"';
    llvm::errs() << '\n';
  }
  if (auto error = run(*command)) return error;
  llvm::json::Array inputs, libraries;
  for (auto &object : objects) inputs.push_back(object.string());
  for (auto &library : invocation->libraries) libraries.push_back(library);
  auto native = read(output);
  if (!native) return native.takeError();
  return atomicJson(metadataPath(metadata, output, "link"), llvm::json::Object{
    {"objects", std::move(inputs)}, {"libraries", std::move(libraries)}, {"native_sha256", digest(*native)}});
}
struct SelectedUnit {
  fs::path capture;
  std::string key, source, flags, optimization;
};
struct SelectedBuild {
  std::vector<SelectedUnit> units;
  std::vector<std::string> libraries;
};
llvm::Expected<SelectedBuild> selectOutput(const fs::path &metadata, const fs::path &output) {
  auto link = readJson(metadataPath(metadata, output, "link"));
  if (!link) return fail("selected output has no captured native link graph: " + output.string() + ": " + llvm::toString(link.takeError()));
  auto record = link->getAsObject();
  if (!record || !record->getString("native_sha256")) return fail("invalid private link record");
  auto bytes = read(output);
  if (!bytes) return bytes.takeError();
  if (digest(*bytes) != *record->getString("native_sha256")) return fail("selected output changed after recorded link");
  auto objects = strings(*record, "objects");
  if (!objects) return objects.takeError();
  auto libraries = strings(*record, "libraries");
  if (!libraries) return libraries.takeError();
  if (objects->empty()) return fail("selected native link has no captured objects");
  SelectedBuild result;
  result.libraries = std::move(*libraries);
  for (auto &path : *objects) {
    auto unit = readJson(metadataPath(metadata, path, "compile"));
    if (!unit) return fail("linked object lacks a capture; prebuilt/cache-hit objects cannot be silently published: " + path + ": " + llvm::toString(unit.takeError()));
    auto item = unit->getAsObject();
    if (!item || !item->getString("native_sha256") || !item->getString("capture") ||
        !item->getString("key") || !item->getString("source") || !item->getArray("flags") ||
        !item->getString("optimization")) return fail("invalid private compile record");
    auto native = read(path);
    if (!native) return native.takeError();
    if (digest(*native) != *item->getString("native_sha256")) return fail("linked object changed after capture: " + path);
    SelectedUnit selected{item->getString("capture")->str(), item->getString("key")->str(),
      item->getString("source")->str(), jsonText(*item->get("flags")), item->getString("optimization")->str()};
    if (!fs::is_regular_file(selected.capture)) return fail("private LLVM capture missing");
    result.units.push_back(std::move(selected));
  }
  return result;
}
} // namespace

int wrapperMain(int argc, char **argv) {
  try {
    const char *sdkRoot = std::getenv("AOT_BUILD_SDK");
    const char *profile = std::getenv("AOT_BUILD_PROFILE");
    const char *lane = std::getenv("AOT_BUILD_LANE");
    const char *metadata = std::getenv("AOT_BUILD_METADATA");
    if (!sdkRoot || !profile || !lane || !metadata ||
        (std::string(profile) != "x86_64" && std::string(profile) != "i686")) {
      llvm::errs() << "aot wrapper: missing private build environment\n";
      return 1;
    }
    Sdk sdk{sdkRoot};
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string name = fs::path(argv[0]).filename();
    llvm::Error error = llvm::Error::success();
    if (name == "aot-ar" || name == "aot-ranlib") {
      auto expanded = expandResponses(args);
      if (!expanded) error = expanded.takeError();
      else {
        args = std::move(*expanded);
        bool query = args.size() == 1 && (args[0] == "--version" || args[0] == "--help");
        if (!query) {
          size_t firstPath = 0;
          if (name == "aot-ar") {
            if (args.size() < 2) error = fail("archive creation requires an operation and output");
            else {
              llvm::StringRef operation(args[0]);
              operation.consume_front("-");
              if (operation.empty() || operation.find_first_not_of("qrcsDU") != llvm::StringRef::npos)
                error = fail("unqualified archiver operation (MRI/custom archive modes are not supported)");
              firstPath = 1;
            }
          }
          if (!error) for (size_t i = firstPath; i < args.size(); ++i) {
            if (name == "aot-ranlib" && (args[i] == "-D" || args[i] == "-U")) continue;
            if (args[i].empty() || args[i][0] == '-' || !inside(absolutePath(args[i]), lane)) {
              error = fail("archiver paths must stay in the private build tree");
              break;
            }
          }
        }
        if (!error) {
          args.insert(args.begin(), sdk.tool(name == "aot-ar" ? "llvm-ar" : "llvm-ranlib").string());
          error = run(args); // Native configure/build archive operations remain real.
        }
      }
    } else if (name == "aot-clang") error = compilerWrapper(args, sdk, profile, lane, metadata);
    else error = fail("direct linker invocation is not qualified; link through aot-clang");
    if (error) { llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "aot wrapper: "); return 1; }
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "aot wrapper: " << error.what() << '\n';
    return 1;
  }
}

llvm::Expected<CapturedBuild> captureBuild(
    const llvm::json::Object &build, const Sdk &sdk,
    const fs::path &recipeDirectory, const fs::path &scratch,
    const std::vector<std::string> &cflags) {
  for (auto &entry : build)
    if (entry.first != "system" && entry.first != "source_dir" && entry.first != "output" &&
        entry.first != "configure_args" && entry.first != "targets")
      return fail("unsupported build recipe field: " + entry.first.str());
  auto system = build.getString("system");
  auto outputName = build.getString("output");
  if (!system || (*system != "cmake" && *system != "make")) return fail("build.system must be cmake or make");
  if (!outputName || outputName->empty() || outputName->contains('\0')) return fail("build.output must select one executable");
  fs::path relativeOutput(outputName->str());
  if (relativeOutput.is_absolute() || relativeOutput.lexically_normal().empty() ||
      *relativeOutput.lexically_normal().begin() == "..") return fail("build.output must stay inside the private build tree");
  auto sourceDir = build.getString("source_dir");
  if (build.get("source_dir") && (!sourceDir || sourceDir->contains('\0')))
    return fail("build.source_dir must be a string without NUL");
  fs::path source = absolutePath(recipeDirectory / (sourceDir ? sourceDir->str() : "."));
  if (!fs::is_directory(source)) return fail("build.source_dir is not a directory");
  if (inside(absolutePath(scratch), source)) return fail("private scratch cannot be inside copied build source");
  auto configureArgs = strings(build, "configure_args");
  if (!configureArgs) return configureArgs.takeError();
  auto targets = strings(build, "targets");
  if (!targets) return targets.takeError();
  for (const auto &target : *targets)
    if (target.empty() || target.front() == '-') return fail("build.targets entries must be target names, not options");
  std::vector<SelectedBuild> selected;
  auto executable = fs::read_symlink("/proc/self/exe");
  for (const char *profile : {"x86_64", "i686"}) {
    fs::path lane = absolutePath(scratch) / (std::string("build-") + profile);
    if (fs::exists(lane)) return fail("private profile build directory already exists");
    fs::create_directories(lane / "metadata");
    fs::create_directories(lane / "wrappers");
    if (auto error = copySource(source, lane / "source", source)) return error;
    for (const char *tool : {"aot-clang", "aot-ar", "aot-ranlib", "aot-ld"})
      fs::create_symlink(executable, lane / "wrappers" / tool);
    llvm::json::Array extraFlags;
    for (const auto &flag : cflags) extraFlags.push_back(flag);
    auto wrapper = [&](const char *name) { return (lane / "wrappers" / name).string(); };
    std::string inheritedPath = std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin";
    std::string inheritedLibraries = std::getenv("LD_LIBRARY_PATH") ? std::getenv("LD_LIBRARY_PATH") : "";
    std::map<std::string, std::string> environment{
      {"AOT_BUILD_SDK", sdk.root.string()}, {"AOT_BUILD_PROFILE", profile},
      {"AOT_BUILD_LANE", lane.string()}, {"AOT_BUILD_METADATA", (lane / "metadata").string()},
      {"CC", wrapper("aot-clang")}, {"AR", wrapper("aot-ar")},
      {"RANLIB", wrapper("aot-ranlib")}, {"LD", wrapper("aot-ld")},
      {"AOT_BUILD_EXTRA_FLAGS", jsonText(std::move(extraFlags))}, {"CCACHE_DISABLE", "1"},
      {"PATH", (sdk.root / "host/usr/bin").string() + ":" + inheritedPath},
      {"LD_LIBRARY_PATH", (sdk.root / "host/usr/lib/llvm-18/lib").string() + ":" +
        (sdk.root / "host/usr/lib/x86_64-linux-gnu").string() +
        (inheritedLibraries.empty() ? "" : ":" + inheritedLibraries)}};
    fs::path outputDirectory;
    if (*system == "cmake") {
      for (const auto &argument : *configureArgs) {
        llvm::StringRef arg(argument);
        if (arg.starts_with("-S") || arg.starts_with("-B") || arg.starts_with("-G") || arg.starts_with("--preset") ||
            arg.starts_with("--toolchain") || arg.starts_with("-DCMAKE_C_COMPILER") ||
            arg.starts_with("-DCMAKE_TOOLCHAIN_FILE") || arg.starts_with("-DCMAKE_AR") ||
            arg.starts_with("-DCMAKE_RANLIB") || arg.starts_with("-DCMAKE_MAKE_PROGRAM"))
          return fail("configure option overrides the private capture toolchain: " + argument);
      }
      outputDirectory = lane / "build";
      std::vector<std::string> command{(sdk.root / "host/usr/bin/cmake").string(),
        "-S", (lane / "source").string(), "-B", outputDirectory.string(), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=" + wrapper("aot-clang"),
        "-DCMAKE_AR=" + wrapper("aot-ar"), "-DCMAKE_RANLIB=" + wrapper("aot-ranlib"),
        "-DCMAKE_MAKE_PROGRAM=" + (sdk.root / "host/usr/bin/ninja").string()};
      command.insert(command.end(), configureArgs->begin(), configureArgs->end());
      if (auto error = run(command, lane, environment)) return error;
      command = {(sdk.root / "host/usr/bin/cmake").string(), "--build", outputDirectory.string(), "--parallel", "2"};
      if (!targets->empty()) { command.push_back("--target"); command.insert(command.end(), targets->begin(), targets->end()); }
      if (auto error = run(command, lane, environment)) return error;
    } else {
      outputDirectory = lane / "source";
      fs::path configure = outputDirectory / "configure";
      if (fs::is_regular_file(configure)) {
        std::vector<std::string> command{configure.string()};
        command.insert(command.end(), configureArgs->begin(), configureArgs->end());
        if (auto error = run(command, outputDirectory, environment)) return error;
      } else if (!configureArgs->empty()) return fail("Make configure_args supplied but no configure script exists");
      std::vector<std::string> command{"make", "-B", "-j2", "CC=" + wrapper("aot-clang"),
        "AR=" + wrapper("aot-ar"), "RANLIB=" + wrapper("aot-ranlib"), "LD=" + wrapper("aot-ld")};
      command.insert(command.end(), targets->begin(), targets->end());
      if (auto error = run(command, outputDirectory, environment)) return error;
    }
    auto result = selectOutput(lane / "metadata", absolutePath(outputDirectory / relativeOutput));
    if (!result) return result.takeError();
    selected.push_back(std::move(*result));
  }
  if (selected[0].libraries != selected[1].libraries || selected[0].units.size() != selected[1].units.size())
    return fail("profile-dependent link graph requires an unimplemented publication contract");
  CapturedBuild result;
  result.libraries = selected[0].libraries;
  for (size_t i = 0; i < selected[0].units.size(); ++i) {
    const auto &left = selected[0].units[i], &right = selected[1].units[i];
    if (left.key != right.key || left.source != right.source || left.flags != right.flags || left.optimization != right.optimization)
      return fail("profile source/compile-flag/object-order divergence cannot be silently paired");
    result.units.push_back({left.capture, right.capture, left.optimization});
  }
  return result;
}
} // namespace aot::driver
