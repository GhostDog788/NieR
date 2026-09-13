#pragma once
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace sela::driver {
namespace fs = std::filesystem;
llvm::Error fail(const std::string &message);
llvm::Expected<std::string> read(const fs::path &path, size_t limit = 64 * 1024 * 1024);
llvm::Error write(const fs::path &path, llvm::StringRef contents);
llvm::Error replaceFile(const fs::path &source, const fs::path &output, bool executable = false);
llvm::Expected<llvm::json::Value> readJson(const fs::path &path);
std::string jsonText(llvm::json::Value value);
std::string digest(llvm::StringRef bytes);
llvm::Error run(const std::vector<std::string> &args, const fs::path &cwd = {},
                const std::map<std::string, std::string> &environment = {},
                const std::vector<std::string> &removeEnvironment = {});
struct Scratch {
  fs::path path;
  bool keep = false;
  Scratch() = default;
  Scratch(const Scratch &) = delete;
  Scratch &operator=(const Scratch &) = delete;
  Scratch(Scratch &&other) noexcept;
  ~Scratch();
  static llvm::Expected<Scratch> create();
};
struct Sdk {
  fs::path root;
  fs::path tool(llvm::StringRef name) const;
  fs::path sysroot(llvm::StringRef profile) const;
  std::map<std::string, std::string> toolEnvironment() const;
  llvm::Error validate(bool publisher = false) const;
  std::vector<std::string> compileFlags(llvm::StringRef profile) const;
  llvm::Expected<std::vector<std::string>> linkCommand(
      llvm::StringRef profile, const std::vector<fs::path> &objects,
      const fs::path &output, const std::vector<std::string> &libraries,
      bool shared = false, const std::vector<std::string> &linkOptions = {},
      const std::vector<fs::path> &libraryDirectories = {}) const;
};
constexpr const char *Contract = "sela-prealpha-1";
}
