#include "Support.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aot::driver {
llvm::Error fail(const std::string &message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), "%s", message.c_str());
}
llvm::Expected<std::string> read(const fs::path &path, size_t limit) {
  std::error_code ec;
  auto size = fs::file_size(path, ec);
  if (ec || size > limit) return fail("cannot read bounded input " + path.string() + (ec ? ": " + ec.message() : ": size limit exceeded"));
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return fail("cannot open " + path.string());
  std::string result(static_cast<size_t>(size), '\0');
  stream.read(result.data(), result.size());
  if (!stream && !result.empty()) return fail("short read: " + path.string());
  return result;
}
llvm::Error write(const fs::path &path, llvm::StringRef contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), contents.size());
  stream.close();
  if (!stream) return fail("cannot write " + path.string());
  return llvm::Error::success();
}
llvm::Expected<llvm::json::Value> readJson(const fs::path &path) {
  auto bytes = read(path, 1024 * 1024);
  if (!bytes) return bytes.takeError();
  return llvm::json::parse(*bytes);
}
std::string jsonText(llvm::json::Value value) {
  std::string result;
  llvm::raw_string_ostream stream(result);
  stream << llvm::formatv("{0:2}", value) << '\n';
  return result;
}
std::string digest(llvm::StringRef bytes) {
  auto hash = llvm::SHA256::hash(llvm::ArrayRef(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
  return llvm::toHex(hash, true);
}
llvm::Error run(const std::vector<std::string> &args, const fs::path &cwd,
                const std::map<std::string, std::string> &environment) {
  if (args.empty()) return fail("empty subprocess command");
  pid_t child = fork();
  if (child < 0) return fail("fork failed: " + std::string(std::strerror(errno)));
  if (child == 0) {
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) { perror("chdir"); _exit(126); }
    for (auto &entry : environment) setenv(entry.first.c_str(), entry.second.c_str(), 1);
    std::vector<char *> pointers;
    for (auto &arg : args) pointers.push_back(const_cast<char *>(arg.c_str()));
    pointers.push_back(nullptr);
    execvp(pointers.front(), pointers.data());
    perror(pointers.front());
    _exit(127);
  }
  int status;
  while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return fail("waitpid failed");
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return fail(args.front() + " failed " + (WIFEXITED(status) ? "with exit " + std::to_string(WEXITSTATUS(status)) : "with signal " + std::to_string(WTERMSIG(status))));
  return llvm::Error::success();
}
Scratch::Scratch(Scratch &&other) noexcept : path(std::move(other.path)), keep(other.keep) { other.path.clear(); }
Scratch::~Scratch() {
  if (!path.empty() && !keep) {
    std::error_code ec;
    fs::remove_all(path, ec); // Only the exact directory made by mkdtemp below.
  }
}
llvm::Expected<Scratch> Scratch::create() {
  std::string pattern = (fs::temp_directory_path() / "aot-private-XXXXXX").string();
  if (!mkdtemp(pattern.data())) return fail("cannot make private work directory");
  Scratch result;
  result.path = pattern;
  return std::move(result);
}
fs::path Sdk::tool(llvm::StringRef name) const { return root / "host/usr/lib/llvm-18/bin" / name.str(); }
fs::path Sdk::sysroot(llvm::StringRef profile) const {
  return root / "sysroots" / (profile == "x86_64" ? "x86_64-linux-gnu" : "i686-linux-gnu");
}
llvm::Error Sdk::validate(bool publisher) const {
  auto receipt = read(root / "sdk-lock.sha256", 128);
  if (!receipt) return fail("SDK bootstrap receipt missing: " + llvm::toString(receipt.takeError()));
  if (llvm::StringRef(*receipt).trim() != AOT_SDK_LOCK_SHA256)
    return fail("SDK package lock does not match this experimental compiler contract");
  for (const char *toolName : {"opt", "llc", "ld.lld"})
    if (!fs::is_regular_file(tool(toolName))) return fail("SDK tool missing: " + tool(toolName).string() + "; run scripts/bootstrap-sdk.sh");
  if (publisher) {
    if (!fs::is_regular_file(tool("clang"))) return fail("publisher SDK is missing stock Clang");
    for (const char *profile : {"x86_64", "i686"})
      if (!fs::is_regular_file(sysroot(profile) / "usr/include/stdio.h")) return fail("publisher SDK headers missing for " + std::string(profile));
  }
  return llvm::Error::success();
}
std::vector<std::string> Sdk::compileFlags(llvm::StringRef profile) const {
  return {"--target=" + std::string(profile == "x86_64" ? "x86_64-unknown-linux-gnu" : "i686-unknown-linux-gnu"),
          "--sysroot=" + sysroot(profile).string(), "-resource-dir=" + (root / "host/usr/lib/llvm-18/lib/clang/18").string()};
}
llvm::Expected<std::vector<std::string>> Sdk::linkCommand(
    llvm::StringRef profile, const std::vector<fs::path> &objects,
    const fs::path &output, const std::vector<std::string> &libraries) const {
  if (profile != "x86_64") return fail("device native linking is currently qualified only for x86_64");
  fs::path target = sysroot(profile);
  fs::path lib = target / "usr/lib/x86_64-linux-gnu";
  fs::path runtime = target / "lib/x86_64-linux-gnu";
  fs::path loader = runtime / "ld-linux-x86-64.so.2";
  fs::path builtins = root / "host/usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.builtins-x86_64.a";
  fs::path crtbegin = builtins.parent_path() / "clang_rt.crtbegin-x86_64.o";
  fs::path crtend = builtins.parent_path() / "clang_rt.crtend-x86_64.o";
  for (auto &path : {lib / "Scrt1.o", lib / "crti.o", lib / "crtn.o", loader, lib / "libc.so", builtins, crtbegin, crtend})
    if (!fs::is_regular_file(path)) return fail("SDK link input missing: " + path.string());
  std::vector<std::string> args = {tool("ld.lld").string(), "--sysroot=" + target.string(), "-pie", "--strip-all", "--eh-frame-hdr", "--hash-style=gnu",
      "--dynamic-linker=" + loader.string(), "--enable-new-dtags", "-rpath", runtime.string() + ":" + lib.string(), "-z", "nodefaultlib", "-z", "relro", "-z", "now",
      "-o", output.string(), (lib / "Scrt1.o").string(), (lib / "crti.o").string(), crtbegin.string()};
  for (auto &object : objects) args.push_back(object.string());
  args.push_back("-L" + lib.string());
  args.push_back("-L" + runtime.string());
  for (auto &library : libraries) {
    if (library != "m") return fail("unqualified managed native library: " + library);
    args.push_back("-l" + library);
  }
  args.insert(args.end(), {builtins.string(), "-lc", crtend.string(), (lib / "crtn.o").string()});
  return args;
}
}
