#include "sela/Support.h"
#include "sela/Targets.h"
#include <cstdint>

namespace sela::driver {
namespace {
bool supportsProfile(llvm::StringRef profile) {
#ifdef SELA_DEVICE_TARGET
  return profile == SELA_DEVICE_TARGET;
#else
  return sela::targets::find(profile) != nullptr;
#endif
}
const sela::targets::TargetInfo &hostTarget() {
#ifdef SELA_DEVICE_TARGET
  return *sela::targets::find(SELA_DEVICE_TARGET);
#elif defined(__x86_64__)
  return *sela::targets::find("x86_64");
#elif defined(__i386__)
  return *sela::targets::find("i686");
#elif defined(__aarch64__)
  return *sela::targets::find("aarch64");
#elif defined(__arm__)
  return *sela::targets::find("armv7");
#else
#error "Unsupported compiler host architecture"
#endif
}
}
fs::path Sdk::tool(llvm::StringRef name) const { return root / "host/usr/lib/llvm-18/bin" / name.str(); }
fs::path Sdk::sysroot(llvm::StringRef profile) const {
  if (!supportsProfile(profile)) return {};
  return root / "sysroots" / sela::targets::find(profile)->sysrootTriple.str();
}
std::map<std::string, std::string> Sdk::toolEnvironment() const {
  // Only compiler subprocesses receive this setting. It must not leak into
  // application execution or rely on a publisher's development shell.
  return {{"LD_LIBRARY_PATH", (root / "host/usr/lib/llvm-18/lib").string() + ":" +
                               (root / "host/usr/lib" / hostTarget().multiarch.str()).string()}};
}
llvm::Error Sdk::validate(bool publisher, llvm::ArrayRef<std::string> selectedTargets) const {
  auto receipt = read(root / "sdk-lock.sha256", 128);
  if (!receipt) return fail("SDK bootstrap receipt missing: " + llvm::toString(receipt.takeError()));
  if (llvm::StringRef(*receipt).trim() != SELA_SDK_LOCK_SHA256)
    return fail("SDK package lock does not match this experimental compiler contract");
#ifdef SELA_REQUIRE_CONSUMER_RECEIPT
  auto completion = readJson(root / "consumer-sdk.json");
  if (!completion) return fail("consumer SDK build is incomplete: " + llvm::toString(completion.takeError()));
  auto *record = completion->getAsObject();
  if (!record || record->getString("sdk_identity") != SELA_SDK_LOCK_SHA256 ||
      record->getString("profile") != SELA_DEVICE_TARGET)
    return fail("consumer SDK completion receipt does not match this compiler");
#endif
  for (const char *toolName : {"opt", "llc", "ld.lld", "llvm-ar"}) {
    if (fs::is_regular_file(tool(toolName))) continue;
#ifdef SELA_REQUIRE_CONSUMER_RECEIPT
    return fail("consumer SDK tool missing: " + tool(toolName).string() +
                "; rebuild this target's SDK or reinstall its complete compiler bundle");
#else
    return fail("SDK tool missing: " + tool(toolName).string() + "; run scripts/bootstrap-sdk.sh");
#endif
  }
  if (publisher) {
#ifdef SELA_DEVICE_TARGET
    return fail("this device SDK does not provide publication tools");
#else
    if (!fs::is_regular_file(tool("clang"))) return fail("publisher SDK is missing stock Clang");
    auto selected = publicationTargets(selectedTargets);
    if (!selected) return selected.takeError();
    for (const auto &target : *selected)
      if (!fs::is_regular_file(sysroot(target) / "usr/include/stdio.h"))
        return fail("publisher SDK headers missing for " + target + "; run scripts/bootstrap-sdk.sh");
#endif
  }
  return llvm::Error::success();
}
std::vector<std::string> Sdk::compileFlags(llvm::StringRef profile) const {
  if (!supportsProfile(profile)) return {};
  const auto &target = *sela::targets::find(profile);
  std::vector<std::string> flags{"--target=" + target.triple.str(),
      "--sysroot=" + sysroot(profile).string(),
      "-resource-dir=" + (root / "host/usr/lib/llvm-18/lib/clang/18").string()};
  for (auto flag : sela::targets::clangArgs(target)) flags.push_back(flag.str());
  for (auto flag : sela::targets::publicationArgs(target)) flags.push_back(flag.str());
  return flags;
}
llvm::Expected<std::vector<std::string>> Sdk::linkCommand(
    llvm::StringRef profile, const std::vector<fs::path> &objects,
    const fs::path &output, const std::vector<std::string> &libraries,
    bool shared, const std::vector<std::string> &linkOptions,
    const std::vector<fs::path> &libraryDirectories) const {
  if (!supportsProfile(profile)) return fail("native linking target is unavailable in this compiler: " + profile.str());
  const auto &info = *sela::targets::find(profile);
  const std::string libraryTriple = info.multiarch.str();
  fs::path target = sysroot(profile);
  fs::path lib = target / "usr/lib" / libraryTriple;
  fs::path runtime = target / "lib" / libraryTriple;
  fs::path loader = runtime / info.loader.str();
  const std::string runtimeArch = info.compilerRTArch.str();
  fs::path builtins = root / "host/usr/lib/llvm-18/lib/clang/18/lib/linux" / ("libclang_rt.builtins-" + runtimeArch + ".a");
  fs::path crtbegin = builtins.parent_path() / ("clang_rt.crtbegin-" + runtimeArch + ".o");
  fs::path crtend = builtins.parent_path() / ("clang_rt.crtend-" + runtimeArch + ".o");
  for (auto &path : {lib / "Scrt1.o", lib / "crti.o", lib / "crtn.o", loader, lib / "libc.so", builtins, crtbegin, crtend})
    if (!fs::is_regular_file(path)) return fail("SDK link input missing: " + path.string());
  std::string runpath;
  for (auto &directory : libraryDirectories) {
    if (!fs::is_directory(directory) || directory.string().find(':') != std::string::npos)
      return fail("invalid managed native library directory: " + directory.string());
    runpath += fs::absolute(directory).string() + ":";
  }
  runpath += runtime.string() + ":" + lib.string();
  std::vector<std::string> args = {tool("ld.lld").string(), "-m", info.lldEmulation.str(), "--sysroot=" + target.string(), shared ? "-shared" : "-pie", "--strip-all", "--build-id", "--eh-frame-hdr", "--hash-style=gnu",
      "--dynamic-linker=" + loader.string(), "--enable-new-dtags", "-rpath", runpath, "-z", "nodefaultlib", "-z", "relro", "-z", "now",
      "-o", output.string()};
  if (!shared) args.push_back((lib / "Scrt1.o").string());
  args.insert(args.end(), {(lib / "crti.o").string(), crtbegin.string()});
  for (auto &object : objects) args.push_back(object.string());
  for (auto &directory : libraryDirectories) args.push_back("-L" + fs::absolute(directory).string());
  args.push_back("-L" + lib.string());
  args.push_back("-L" + runtime.string());
  // Native capture records the resolved DT_NEEDED list, including libraries
  // used only for constructors. Do not re-run as-needed selection on it.
  args.push_back("--no-as-needed");
  for (auto &library : libraries) {
    if (library.empty() || library.find('/') != std::string::npos || library.find('\0') != std::string::npos)
      return fail("invalid managed native library name");
    std::vector<fs::path> search = libraryDirectories;
    search.insert(search.end(), {lib, runtime});
    bool exact = library.front() == ':';
    std::string filename = exact ? library.substr(1) : "lib" + library + ".so";
    bool found = false;
    for (auto &directory : search) {
      found |= fs::is_regular_file(directory / filename);
      if (!exact) found |= fs::is_regular_file(directory / ("lib" + library + ".a"));
    }
    if (!found) return fail("missing declared managed native dependency: " + filename);
    args.push_back("-l" + library);
  }
  args.insert(args.end(), linkOptions.begin(), linkOptions.end());
  args.insert(args.end(), {builtins.string(), "-lc", crtend.string(), (lib / "crtn.o").string()});
  return args;
}
}
