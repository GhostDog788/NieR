#include "sela/Support.h"
#include "sela/Targets.h"
#include "llvm/ADT/SmallVector.h"
#include <cstdlib>
#include <set>

namespace sela::driver {
std::string targetSelectionText(llvm::ArrayRef<std::string> targets) {
  std::string result;
  for (const auto &target : targets) {
    if (!result.empty()) result += ',';
    result += target;
  }
  return result;
}

llvm::Expected<std::vector<std::string>> parseTargetSelection(llvm::StringRef text) {
  llvm::SmallVector<llvm::StringRef> fields;
  text.split(fields, ',', -1, true);
  std::set<std::string> selected;
  for (auto field : fields) {
    if (field.empty()) return fail("architecture selection contains an empty target");
    if (!sela::targets::find(field)) return fail("unknown publication architecture: " + field.str());
    if (!selected.insert(field.str()).second)
      return fail("duplicate publication architecture: " + field.str());
  }
  std::vector<std::string> result;
  for (const auto &target : sela::targets::all())
    if (selected.count(target.id.str())) result.push_back(target.id.str());
  return result;
}

llvm::Expected<std::vector<std::string>> publicationTargets(
    llvm::ArrayRef<std::string> requested) {
  if (!requested.empty()) {
    // --arch accepts one ID, whereas the environment accepts a CSV list.
    for (const auto &target : requested)
      if (!sela::targets::find(target)) return fail("unknown publication architecture: " + target);
    return parseTargetSelection(targetSelectionText(requested));
  }
  if (const char *configured = std::getenv("SELA_ARCHS"))
    return parseTargetSelection(configured);
  std::vector<std::string> result;
  for (const auto &target : sela::targets::all()) result.push_back(target.id.str());
  return result;
}
} // namespace sela::driver
