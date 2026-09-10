#include "Package.h"
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <memory>
#include <set>
#include <unistd.h>

namespace aot::driver {
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
llvm::Error writePackage(const fs::path &output, const PackageFiles &files) {
  size_t total = 0;
  if (files.size() > MaxFiles) return fail("too many package files");
  for (auto &file : files) {
    if (file.second.size() > MaxBytes - total) return fail("publication exceeds package size limit");
    total += file.second.size();
  }
  int fd = open(output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) return fail("cannot create new package (will not overwrite): " + output.string());
  std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
  auto abort = [&](const std::string &message) -> llvm::Error {
    archive_write_close(writer.get());
    close(fd);
    std::error_code ec;
    fs::remove(output, ec); // Our newly created incomplete output only.
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
  return llvm::Error::success();
}
llvm::Expected<PackageFiles> readPackage(const fs::path &input) {
  std::error_code ec;
  auto size = fs::file_size(input, ec);
  if (ec || size > MaxBytes + 1024 * 1024) return fail("unreadable or oversized publication package");
  std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
  archive_read_support_format_tar(reader.get());
  archive_read_support_filter_none(reader.get());
  if (archive_read_open_filename(reader.get(), input.c_str(), 10240) != ARCHIVE_OK) return fail(archiveError(reader.get()));
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
  // deterministic publisher encoding so discarded values cannot hide inputs.
  if (jsonText(*parsed) != manifest->second) return fail("manifest must use the canonical publisher JSON encoding (no duplicate keys)");
  auto *object = parsed->getAsObject();
  if (!object || object->getInteger("format_version") != 1 || object->getString("contract") != Contract)
    return fail("unsupported experimental format/compiler contract");
  for (auto &entry : *object)
    if (entry.first != "format_version" && entry.first != "contract" && entry.first != "name" &&
        entry.first != "runtime" && entry.first != "profiles" && entry.first != "libraries" && entry.first != "modules")
      return fail("unknown public manifest field: " + entry.first.str());
  auto name = object->getString("name");
  if (!name || !validName(*name)) return fail("invalid application name");
  if (object->getString("runtime") != "glibc-2.39-0ubuntu8.8") return fail("unsupported managed runtime contract");
  auto *profiles = object->getArray("profiles");
  if (!profiles || profiles->size() != 2 || (*profiles)[0].getAsString() != "x86_64" || (*profiles)[1].getAsString() != "i686")
    return fail("unsupported capture profile domain");
  auto *libraries = object->getArray("libraries");
  if (!libraries) return fail("missing library declarations");
  for (auto &library : *libraries)
    if (library.getAsString() != "m") return fail("unqualified native library in package");
  auto *modules = object->getArray("modules");
  if (!modules || modules->empty()) return fail("package has no common IR modules");
  std::set<std::string> expected{"manifest.json"};
  for (size_t index = 0; index < modules->size(); ++index) {
    auto *module = (*modules)[index].getAsObject();
    if (!module) return fail("invalid module record");
    for (auto &entry : *module)
      if (entry.first != "path" && entry.first != "sha256" && entry.first != "optimization")
        return fail("unknown public module field: " + entry.first.str());
    std::string path = "modules/" + std::to_string(index) + ".mlirbc";
    auto optimization = module->getString("optimization");
    if (module->getString("path") != path || !optimization ||
        (*optimization != "O0" && *optimization != "O1" && *optimization != "O2" && *optimization != "O3" && *optimization != "Os" && *optimization != "Oz"))
      return fail("invalid module path or optimization level");
    auto found = files.find(path);
    if (found == files.end() || module->getString("sha256") != digest(found->second)) return fail("missing module or digest mismatch: " + path);
    expected.insert(path);
  }
  if (expected.size() != files.size()) return fail("package contains undeclared files");
  return std::move(*parsed);
}
}
