#include "sela/Support.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <optional>

using namespace sela::driver;
int main() {
  unsigned failures = 0;
  auto check = [&](bool good, llvm::StringRef label) {
    llvm::outs() << (good ? "PASS " : "FAIL ") << label << '\n';
    failures += !good;
  };
  for (const char *text : {"", ",", "x86_64,", ",i686", "armv7,,aarch64",
                           "x86_64,x86_64", "mips", " x86_64", "armv7 aarch64"}) {
    auto result = parseTargetSelection(text);
    check(!result, std::string("reject selection '") + text + "'");
    if (!result) llvm::consumeError(result.takeError());
  }
  auto result = parseTargetSelection("aarch64,i686,x86_64");
  check(result && *result == std::vector<std::string>{"x86_64", "i686", "aarch64"},
        "selection is in canonical registry order");
  if (!result) llvm::consumeError(result.takeError());
  const char *previous = std::getenv("SELA_ARCHS");
  std::optional<std::string> saved = previous ? std::optional<std::string>(previous) : std::nullopt;
  unsetenv("SELA_ARCHS");
  auto all = publicationTargets();
  check(all && all->size() == 4, "unset selection means four targets");
  if (!all) llvm::consumeError(all.takeError());
  setenv("SELA_ARCHS", "armv7", 1);
  auto arm = publicationTargets();
  check(arm && *arm == std::vector<std::string>{"armv7"}, "environment selects singleton");
  if (!arm) llvm::consumeError(arm.takeError());
  const std::vector<std::string> explicitIDs{"aarch64"};
  auto explicitResult = publicationTargets(explicitIDs);
  check(explicitResult && *explicitResult == explicitIDs, "explicit selection overrides environment");
  if (!explicitResult) llvm::consumeError(explicitResult.takeError());
  setenv("SELA_ARCHS", "", 1);
  auto empty = publicationTargets();
  check(!empty, "explicit empty environment is not a default");
  if (!empty) llvm::consumeError(empty.takeError());
  if (saved) setenv("SELA_ARCHS", saved->c_str(), 1);
  else unsetenv("SELA_ARCHS");
  return failures ? 1 : 0;
}
