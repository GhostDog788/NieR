#include "sela/Artifact/Artifact.h"
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <memory>
#include <set>
#include <unistd.h>

namespace sela::driver {
namespace {
constexpr size_t MaxBytes = 64 * 1024 * 1024;
constexpr size_t MaxFiles = 512;
std::string archiveError(archive *a) {
  const char *message = archive_error_string(a);
  return message ? message : "unknown archive error";
}
}
bool validName(llvm::StringRef name) {
  if (name.empty() || name.size() > 64) return false;
  return llvm::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}
bool validLibrary(llvm::StringRef name) {
  if (name.consume_front(":")) {
    if (name.empty() || name.size() > 128 || name == "." || name == "..") return false;
    return llvm::all_of(name, [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+';
    });
  }
  return validName(name);
}
bool validArchiveMember(llvm::StringRef name) {
  return !name.empty() && name.size() <= 128 && name != "." && name != ".." &&
      llvm::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+';
      });
}
llvm::Expected<std::string> normalizeVersionScript(llvm::StringRef text) {
  if (text.empty() || text.size() > 65536) return fail("empty or oversized symbol version script");
  std::string output;
  for (size_t i = 0; i < text.size();) {
    if (text.substr(i).starts_with("/*")) {
      auto end = text.find("*/", i + 2);
      if (end == llvm::StringRef::npos) return fail("unterminated version script comment");
      output.push_back(' '); i = end + 2; continue;
    }
    if (text.substr(i).starts_with("//") || text[i] == '#') {
      auto end = text.find('\n', i);
      i = end == llvm::StringRef::npos ? text.size() : end;
      output.push_back(' '); continue;
    }
    char c = text[i++];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || llvm::StringRef("_.*?[]+-;:{} \t\r\n").contains(c)))
      return fail("unqualified C symbol version script syntax");
    output.push_back(c);
  }
  // This is passed only to LLD's --version-script parser, never its general
  // linker-script parser. Words such as INPUT may be perfectly valid symbol
  // names here; version scripts do not provide file-input directives.
  if (llvm::StringRef(output).trim().empty()) return fail("symbol version script has no declarations");
  return output;
}
llvm::Error writePackage(const fs::path &output, const PackageFiles &files) {
  size_t total = 0;
  if (files.size() > MaxFiles) return fail("too many package files");
  for (auto &file : files) {
    if (file.second.size() > MaxBytes - total) return fail("publication exceeds package size limit");
    total += file.second.size();
  }
  auto checked = validatePackage(files);
  if (!checked) return checked.takeError();
  auto scratch = Scratch::create();
  if (!scratch) return scratch.takeError();
  auto staged = scratch->path / "artifact.sela";
  int fd = open(staged.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) return fail("cannot create staged artifact: " + staged.string());
  std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
  auto abort = [&](const std::string &message) -> llvm::Error {
    archive_write_close(writer.get());
    close(fd);
    std::error_code ec;
    fs::remove(staged, ec); // Our newly created incomplete output only.
    return fail(message);
  };
  if (archive_write_set_format_pax_restricted(writer.get()) != ARCHIVE_OK || archive_write_open_fd(writer.get(), fd) != ARCHIVE_OK)
    return abort(archiveError(writer.get()));
  for (auto &file : files) {
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), file.first.c_str());
    archive_entry_set_size(entry.get(), file.second.size());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_mtime(entry.get(), 0, 0);
    if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK ||
        archive_write_data(writer.get(), file.second.data(), file.second.size()) != static_cast<la_ssize_t>(file.second.size()))
      return abort(archiveError(writer.get()));
  }
  if (archive_write_close(writer.get()) != ARCHIVE_OK) return abort(archiveError(writer.get()));
  if (close(fd)) return fail("package close failed");
  return replaceFile(staged, output);
}
llvm::Expected<PackageFiles> readPackage(const fs::path &input) {
  // Read the same bounded regular-file descriptor we validated. A pathname
  // size check followed by libarchive reopening it leaves a race to a device
  // or FIFO and can block indefinitely before the entry limits are reached.
  auto bytes = read(input, MaxBytes + 1024 * 1024);
  if (!bytes) return bytes.takeError();
  std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
  archive_read_support_format_tar(reader.get());
  archive_read_support_filter_none(reader.get());
  if (archive_read_open_memory(reader.get(), bytes->data(), bytes->size()) != ARCHIVE_OK) return fail(archiveError(reader.get()));
  PackageFiles files;
  size_t total = 0;
  archive_entry *entry;
  int status;
  while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
    const char *raw = archive_entry_pathname(entry);
    if (!raw) return fail("archive entry without pathname");
    std::string name(raw);
    fs::path path(name);
    if (name.empty() || name.size() > 256 || path.is_absolute() || path.lexically_normal().generic_string() != name ||
        name.find("..") != std::string::npos || archive_entry_filetype(entry) != AE_IFREG ||
        archive_entry_symlink(entry) || archive_entry_hardlink(entry)) return fail("invalid archive entry: " + name);
    auto length = archive_entry_size(entry);
    if (length < 0 || static_cast<uint64_t>(length) > MaxBytes - total || files.size() >= MaxFiles || files.count(name))
      return fail("archive size/count limit or duplicate entry: " + name);
    std::string data(static_cast<size_t>(length), '\0');
    size_t offset = 0;
    while (offset < data.size()) {
      auto count = archive_read_data(reader.get(), data.data() + offset, data.size() - offset);
      if (count <= 0) return fail("truncated archive entry: " + name);
      offset += static_cast<size_t>(count);
    }
    total += data.size();
    files.emplace(std::move(name), std::move(data));
  }
  if (status != ARCHIVE_EOF) return fail(archiveError(reader.get()));
  return files;
}
llvm::Expected<llvm::json::Value> validatePackage(const PackageFiles &files) {
  auto manifest = files.find("manifest.json");
  if (manifest == files.end() || manifest->second.size() > 1024 * 1024) return fail("missing or oversized manifest");
  auto parsed = llvm::json::parse(manifest->second);
  if (!parsed) return parsed.takeError();
  // LLVM's JSON reader retains only one value for duplicate keys. Require the
  // deterministic public encoding so discarded values cannot hide inputs.
  if (jsonText(*parsed) != manifest->second) return fail("manifest must use canonical Sela JSON encoding (no duplicate keys)");
  auto *object = parsed->getAsObject();
  if (!object || object->getInteger("format_version") != 1 || object->getString("contract") != Contract)
    return fail("unsupported experimental format/compiler contract");
  for (auto &entry : *object)
    if (entry.first != "format_version" && entry.first != "contract" && entry.first != "kind" &&
        entry.first != "runtime" && entry.first != "targets" && entry.first != "libraries" && entry.first != "modules" && entry.first != "link_options" && entry.first != "version_script" && entry.first != "compilation_units")
      return fail("unknown public manifest field: " + entry.first.str());
  auto kind = object->getString("kind");
  if (!kind || (*kind != "object" && *kind != "executable" && *kind != "shared" && *kind != "static")) return fail("invalid artifact kind");
  if (object->getString("runtime") != "glibc-2.39-0ubuntu8.8") return fail("unsupported managed runtime contract");
  auto *targets = object->getArray("targets");
  if (!targets || targets->empty()) return fail("missing target constraints");
  std::set<std::string> seenTargets;
  for (auto &target : *targets) {
    auto name = target.getAsString();
    if (!name || (*name != "x86_64" && *name != "i686") || !seenTargets.insert(name->str()).second)
      return fail("unsupported or duplicate target constraint");
  }
  auto *libraries = object->getArray("libraries");
  if (!libraries) return fail("missing library declarations");
  for (auto &library : *libraries)
    if (!library.getAsString() || !validLibrary(*library.getAsString())) return fail("invalid native library contract");
  if (*kind == "static" && !libraries->empty())
    return fail("static archive dependency linkage must be declared by its consuming native link");
  auto *linkOptions = object->getArray("link_options");
  if (!linkOptions) return fail("missing link options");
  for (auto &option : *linkOptions) {
    auto text = option.getAsString();
    if (!text || *kind == "object" || *kind == "static") return fail("unqualified native link option");
    if (*text == "--export-dynamic" || *text == "--hash-style=both" ||
        *text == "--undefined-version") continue;
    if (*kind != "shared" || !text->starts_with("-soname=") ||
        !validLibrary(":" + text->drop_front(8).str()))
      return fail("unqualified native link option");
  }
  auto *modules = object->getArray("modules");
  if (!modules || (modules->empty() && *kind != "static")) return fail("package has no common IR modules");
  std::set<std::string> expected{"manifest.json"};
  if (auto *scriptValue = object->get("version_script")) {
    auto *script = scriptValue->getAsObject();
    if (*kind != "shared" || !script || script->size() != 2 ||
        script->getString("path") != "link/version.script") return fail("invalid symbol version script record");
    auto contents = files.find("link/version.script");
    if (contents == files.end() || script->getString("sha256") != digest(contents->second))
      return fail("missing or corrupt symbol version script");
    auto normalized = normalizeVersionScript(contents->second);
    if (!normalized) return normalized.takeError();
    if (*normalized != contents->second) return fail("private comments in published symbol version script");
    expected.insert(contents->first);
  }
  for (size_t index = 0; index < modules->size(); ++index) {
    auto *module = (*modules)[index].getAsObject();
    if (!module) return fail("invalid module record");
    for (auto &entry : *module)
      if (entry.first != "path" && entry.first != "sha256")
        return fail("unknown public module field: " + entry.first.str());
    std::string path = "modules/" + std::to_string(index) + ".selabc";
    if (module->getString("path") != path) return fail("invalid module path");
    auto found = files.find(path);
    if (found == files.end() || module->getString("sha256") != digest(found->second)) return fail("missing module or digest mismatch: " + path);
    expected.insert(path);
  }
  auto compilationPlan = readCompilationPlan(*object);
  if (!compilationPlan) return compilationPlan.takeError();
  if (expected.size() != files.size()) return fail("package contains undeclared files");
  return std::move(*parsed);
}
llvm::Expected<CompilationPlan> readCompilationPlan(const llvm::json::Object &manifest) {
  const auto *plans = manifest.getObject("compilation_units");
  const auto *targets = manifest.getArray("targets");
  const auto *modules = manifest.getArray("modules");
  auto kind = manifest.getString("kind");
  if (!plans || !targets || !modules || !kind || plans->size() != targets->size())
    return fail("missing or inconsistent native compilation-unit plan");
  CompilationPlan result;
  for (const auto &target : *targets) {
    auto name = target.getAsString();
    if (!name) return fail("invalid compilation target");
    const auto *units = plans->getArray(*name);
    if (!units || units->size() > modules->size()) return fail("invalid compilation-unit inventory");
    std::vector<bool> seen(modules->size());
    auto &destination = result[name->str()];
    for (const auto &entry : *units) {
      const auto *unit = entry.getAsObject();
      if (!unit) return fail("invalid compilation-unit record");
      for (const auto &field : *unit)
        if (field.first != "modules" && field.first != "optimization" && field.first != "archive_member")
          return fail("unknown compilation-unit field: " + field.first.str());
      const auto *members = unit->getArray("modules");
      auto optimization = unit->getString("optimization");
      if (!members || members->empty() || !optimization ||
          (*optimization != "O0" && *optimization != "O1" && *optimization != "O2" &&
           *optimization != "O3" && *optimization != "Os" && *optimization != "Oz"))
        return fail("invalid compilation-unit members or optimization");
      ArtifactUnit record;
      record.optimization = optimization->str();
      if (*kind == "static") {
        auto member = unit->getString("archive_member");
        if (!member || !validArchiveMember(*member)) return fail("invalid static archive member identity");
        record.archiveMember = member->str();
      } else if (unit->get("archive_member")) return fail("archive member identity requires static output");
      for (const auto &member : *members) {
        auto index = member.getAsInteger();
        if (!index || *index < 0 || uint64_t(*index) >= modules->size() || seen[*index])
          return fail("compilation-unit plan duplicates or misreferences a common fragment");
        seen[*index] = true;
        record.modules.push_back(size_t(*index));
      }
      destination.push_back(std::move(record));
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end())
      return fail("compilation-unit plan drops a common fragment");
  }
  return result;
}
llvm::Expected<PackageFiles> createArtifact(
    llvm::StringRef kind, const std::vector<ArtifactModule> &modules,
    const std::vector<std::string> &libraries,
    const std::vector<std::string> &linkOptions,
    const std::vector<std::string> &targets, llvm::StringRef versionScript,
    const CompilationPlan &compilationPlan) {
  PackageFiles files;
  llvm::json::Array records, targetList, libraryList, options;
  for (size_t i = 0; i < modules.size(); ++i) {
    auto path = "modules/" + std::to_string(i) + ".selabc";
    files.emplace(path, modules[i].bytecode);
    llvm::json::Object record{{"path", path}, {"sha256", digest(modules[i].bytecode)}};
    records.push_back(std::move(record));
  }
  for (auto &target : targets) targetList.push_back(target);
  for (auto &library : libraries) libraryList.push_back(library);
  for (auto &option : linkOptions) options.push_back(option);
  CompilationPlan plan = compilationPlan;
  if (plan.empty())
    for (const auto &target : targets)
      for (size_t i = 0; i < modules.size(); ++i)
        plan[target].push_back({{i}, modules[i].optimization, modules[i].archiveMember});
  // Empty static archives still have an explicit empty plan for every target.
  if (compilationPlan.empty()) for (const auto &target : targets) plan.try_emplace(target);
  llvm::json::Object unitPlans;
  for (const auto &[target, units] : plan) {
    llvm::json::Array records;
    for (const auto &unit : units) {
      llvm::json::Array indices;
      for (auto index : unit.modules) indices.push_back(int64_t(index));
      llvm::json::Object record{{"modules", std::move(indices)}, {"optimization", unit.optimization}};
      if (!unit.archiveMember.empty()) record["archive_member"] = unit.archiveMember;
      records.push_back(std::move(record));
    }
    unitPlans[target] = std::move(records);
  }
  llvm::json::Object manifest{{"format_version", 1}, {"contract", Contract},
      {"kind", kind}, {"targets", std::move(targetList)}, {"runtime", "glibc-2.39-0ubuntu8.8"},
      {"libraries", std::move(libraryList)}, {"link_options", std::move(options)}, {"modules", std::move(records)},
      {"compilation_units", std::move(unitPlans)}};
  if (!versionScript.empty()) {
    auto normalized = normalizeVersionScript(versionScript);
    if (!normalized) return normalized.takeError();
    manifest["version_script"] = llvm::json::Object{{"path", "link/version.script"}, {"sha256", digest(*normalized)}};
    files["link/version.script"] = std::move(*normalized);
  }
  files["manifest.json"] = jsonText(std::move(manifest));
  auto checked = validatePackage(files);
  if (!checked) return checked.takeError();
  return files;
}
}
