#include "nier/Support.h"
#include <cstdint>

namespace nier::driver {
namespace {
bool supportsProfile(llvm::StringRef profile) {
#ifdef NIER_DEVICE_TARGET
  return profile == NIER_DEVICE_TARGET;
#else
  return profile == "x86_64" || profile == "i686";
#endif
}
bool is64(llvm::StringRef profile) {
#ifdef NIER_DEVICE_WORD_BITS
  return NIER_DEVICE_WORD_BITS == 64;
#else
  return profile == "x86_64";
#endif
}
}
fs::path Sdk::tool(llvm::StringRef name) const { return root / "host/usr/lib/llvm-18/bin" / name.str(); }
fs::path Sdk::sysroot(llvm::StringRef profile) const {
  if (!supportsProfile(profile)) return {};
  return root / "sysroots" / (is64(profile) ? "x86_64-linux-gnu" : "i686-linux-gnu");
}
std::map<std::string, std::string> Sdk::toolEnvironment() const {
  // Only compiler subprocesses receive this setting. It must not leak into
  // application execution or rely on a publisher's development shell.
  return {{"LD_LIBRARY_PATH", (root / "host/usr/lib/llvm-18/lib").string() + ":" +
                               (root / "host/usr/lib" / (sizeof(void *) == 8 ? "x86_64-linux-gnu" : "i386-linux-gnu")).string()}};
}
llvm::Error Sdk::validate(bool publisher) const {
  auto receipt = read(root / "sdk-lock.sha256", 128);
  if (!receipt) return fail("SDK bootstrap receipt missing: " + llvm::toString(receipt.takeError()));
  if (llvm::StringRef(*receipt).trim() != NIER_SDK_LOCK_SHA256)
    return fail("SDK package lock does not match this experimental compiler contract");
#ifdef NIER_REQUIRE_CONSUMER_RECEIPT
  auto completion = readJson(root / "consumer-sdk.json");
  if (!completion) return fail("consumer SDK build is incomplete: " + llvm::toString(completion.takeError()));
  auto *record = completion->getAsObject();
  if (!record || record->getString("sdk_identity") != NIER_SDK_LOCK_SHA256 ||
      record->getString("profile") != NIER_DEVICE_TARGET)
    return fail("consumer SDK completion receipt does not match this compiler");
#endif
  for (const char *toolName : {"opt", "llc", "ld.lld", "llvm-ar"}) {
    if (fs::is_regular_file(tool(toolName))) continue;
#ifdef NIER_REQUIRE_CONSUMER_RECEIPT
    return fail("consumer SDK tool missing: " + tool(toolName).string() +
                "; rebuild this target's SDK or reinstall its complete compiler bundle");
#else
    return fail("SDK tool missing: " + tool(toolName).string() + "; run scripts/bootstrap-sdk.sh");
#endif
  }
  if (publisher) {
#ifdef NIER_DEVICE_TARGET
    return fail("this device SDK does not provide publication tools");
#else
    if (!fs::is_regular_file(tool("clang"))) return fail("publisher SDK is missing stock Clang");
    for (const char *profile : {"x86_64", "i686"})
      if (!fs::is_regular_file(sysroot(profile) / "usr/include/stdio.h")) return fail("publisher SDK headers missing for " + std::string(profile));
#endif
  }
  return llvm::Error::success();
}
std::vector<std::string> Sdk::compileFlags(llvm::StringRef profile) const {
  if (!supportsProfile(profile)) return {};
  return {"--target=" + std::string(is64(profile) ? "x86_64-unknown-linux-gnu" : "i686-unknown-linux-gnu"),
          "--sysroot=" + sysroot(profile).string(), "-resource-dir=" + (root / "host/usr/lib/llvm-18/lib/clang/18").string()};
}
llvm::Expected<std::vector<std::string>> Sdk::linkCommand(
    llvm::StringRef profile, const std::vector<fs::path> &objects,
    const fs::path &output, const std::vector<std::string> &libraries,
    bool shared, const std::vector<std::string> &linkOptions,
    const std::vector<fs::path> &libraryDirectories) const {
  if (!supportsProfile(profile)) return fail("native linking target is unavailable in this compiler: " + profile.str());
  const bool x64 = is64(profile);
  const std::string libraryTriple = x64 ? "x86_64-linux-gnu" : "i386-linux-gnu";
  fs::path target = sysroot(profile);
  fs::path lib = target / "usr/lib" / libraryTriple;
  fs::path runtime = target / "lib" / libraryTriple;
  fs::path loader = runtime / (x64 ? "ld-linux-x86-64.so.2" : "ld-linux.so.2");
  const std::string runtimeArch = x64 ? "x86_64" : "i386";
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
  std::vector<std::string> args = {tool("ld.lld").string(), "-m", x64 ? "elf_x86_64" : "elf_i386", "--sysroot=" + target.string(), shared ? "-shared" : "-pie", "--strip-all", "--build-id", "--eh-frame-hdr", "--hash-style=gnu",
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
