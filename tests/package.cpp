#include "Package.h"
#include "llvm/Support/raw_ostream.h"
#include <archive.h>
#include <archive_entry.h>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aot::driver;

namespace {
constexpr size_t MaxBytes = 64 * 1024 * 1024;
constexpr size_t MaxFiles = 512;

struct Entry {
  std::string name;
  std::string bytes;
  unsigned type = AE_IFREG;
  std::string symlink;
  std::string hardlink;
  std::optional<la_int64_t> declaredSize;
};

std::string archiveMessage(archive *handle) {
  const char *message = archive_error_string(handle);
  return message ? message : "unknown libarchive error";
}

// Generate every archive with libarchive, including entries the production
// writer deliberately cannot represent (links, directories and duplicates).
// A declared size larger than bytes is padded by the archive writer. This
// exercises reader bounds without allocating that much fixture data here.
llvm::Error writeRawArchive(const fs::path &path,
                            const std::vector<Entry> &entries,
                            bool gzip = false) {
  std::unique_ptr<archive, decltype(&archive_write_free)> writer(
      archive_write_new(), archive_write_free);
  if (!writer)
    return fail("cannot allocate test archive writer");
  if (gzip && archive_write_add_filter_gzip(writer.get()) != ARCHIVE_OK)
    return fail(archiveMessage(writer.get()));
  if (archive_write_set_format_pax_restricted(writer.get()) != ARCHIVE_OK ||
      archive_write_set_bytes_per_block(writer.get(), 512) != ARCHIVE_OK ||
      archive_write_open_filename(writer.get(), path.c_str()) != ARCHIVE_OK)
    return fail(archiveMessage(writer.get()));
  for (const auto &item : entries) {
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), item.name.c_str());
    archive_entry_set_filetype(entry.get(), item.type);
    archive_entry_set_perm(entry.get(), 0600);
    archive_entry_set_mtime(entry.get(), 0, 0);
    archive_entry_set_size(
        entry.get(), item.declaredSize.value_or(item.bytes.size()));
    if (!item.symlink.empty())
      archive_entry_set_symlink(entry.get(), item.symlink.c_str());
    if (!item.hardlink.empty())
      archive_entry_set_hardlink(entry.get(), item.hardlink.c_str());
    if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK)
      return fail(archiveMessage(writer.get()));
    if (!item.bytes.empty() &&
        archive_write_data(writer.get(), item.bytes.data(), item.bytes.size()) !=
            static_cast<la_ssize_t>(item.bytes.size()))
      return fail(archiveMessage(writer.get()));
    if (archive_write_finish_entry(writer.get()) != ARCHIVE_OK)
      return fail(archiveMessage(writer.get()));
  }
  if (archive_write_close(writer.get()) != ARCHIVE_OK)
    return fail(archiveMessage(writer.get()));
  return llvm::Error::success();
}

PackageFiles validFiles() {
  // Package validation owns the envelope/digests. Actual common-IR decoding
  // and semantic validation belong to inspectArtifact/lowerArtifact tests.
  std::string module("opaque\0module-fixture", 21);
  llvm::json::Array modules;
  modules.push_back(llvm::json::Object{{"path", "modules/0.mlirbc"},
                                      {"sha256", digest(module)},
                                      {"optimization", "O2"}});
  PackageFiles result;
  result["modules/0.mlirbc"] = module;
  result["manifest.json"] = jsonText(llvm::json::Object{
      {"format_version", 1}, {"contract", Contract}, {"name", "package-test"},
      {"runtime", "glibc-2.39-0ubuntu8.8"},
      {"profiles", llvm::json::Array{"x86_64", "i686"}},
      {"libraries", llvm::json::Array{}}, {"modules", std::move(modules)}});
  return result;
}

PackageFiles mutateManifest(
    const std::function<void(llvm::json::Object &)> &mutation) {
  auto files = validFiles();
  auto parsed = llvm::json::parse(files.at("manifest.json"));
  if (!parsed)
    throw std::runtime_error(llvm::toString(parsed.takeError()));
  mutation(*parsed->getAsObject());
  files["manifest.json"] = jsonText(std::move(*parsed));
  return files;
}

llvm::json::Object &firstModule(llvm::json::Object &manifest) {
  return *(*manifest.getArray("modules"))[0].getAsObject();
}

class Tests {
  fs::path root;
  size_t serial = 0;
  unsigned total = 0;
  unsigned failures = 0;

public:
  explicit Tests(fs::path directory) : root(std::move(directory)) {}

  fs::path nextPath() {
    return root / (std::to_string(serial++) + ".tar");
  }

  void check(llvm::StringRef name, bool success,
             const std::string &detail = {}) {
    ++total;
    if (!success)
      ++failures;
    llvm::outs() << (success ? "PASS " : "FAIL ") << name;
    if (!detail.empty())
      llvm::outs() << ": " << detail;
    llvm::outs() << '\n';
  }

  void validate(llvm::StringRef name, const PackageFiles &files, bool accept) {
    auto result = validatePackage(files);
    bool success = static_cast<bool>(result);
    std::string error;
    if (!success)
      error = llvm::toString(result.takeError());
    check(name, success == accept,
          success == accept ? "" : (success ? "unexpected acceptance" : error));
  }

  void readArchive(llvm::StringRef name, const fs::path &path, bool accept) {
    auto result = readPackage(path);
    bool success = static_cast<bool>(result);
    std::string error;
    if (!success)
      error = llvm::toString(result.takeError());
    check(name, success == accept,
          success == accept ? "" : (success ? "unexpected acceptance" : error));
  }

  void archive(llvm::StringRef name, const std::vector<Entry> &entries,
               bool accept) {
    auto path = nextPath();
    if (auto error = writeRawArchive(path, entries)) {
      check(name, false, "fixture creation failed: " + llvm::toString(std::move(error)));
      return;
    }
    readArchive(name, path, accept);
  }

  int finish() {
    llvm::outs() << "Package boundary tests: " << total - failures << '/' << total
                 << " passed\n";
    return failures ? 1 : 0;
  }
};

void manifestTests(Tests &tests) {
  tests.validate("standard manifest accepted", validFiles(), true);
  auto multiple = validFiles();
  multiple["modules/1.mlirbc"] = "another opaque module";
  auto multipleManifest = llvm::json::parse(multiple.at("manifest.json"));
  if (!multipleManifest)
    throw std::runtime_error(llvm::toString(multipleManifest.takeError()));
  multipleManifest->getAsObject()->getArray("modules")->push_back(
      llvm::json::Object{{"path", "modules/1.mlirbc"},
                         {"sha256", digest(multiple.at("modules/1.mlirbc"))},
                         {"optimization", "O0"}});
  multiple["manifest.json"] = jsonText(std::move(*multipleManifest));
  tests.validate("multiple translation units accepted", multiple, true);
  for (const char *level : {"O0", "O1", "O2", "O3", "Os", "Oz"})
    tests.validate(std::string("optimization accepted: ") + level,
                   mutateManifest([&](auto &m) {
                     firstModule(m)["optimization"] = level;
                   }), true);
  tests.validate("declared managed libm accepted",
                 mutateManifest([](auto &m) {
                   m["libraries"] = llvm::json::Array{"m"};
                 }), true);
  tests.validate("unknown format version rejected",
                 mutateManifest([](auto &m) { m["format_version"] = 2; }), false);
  tests.validate("string format version rejected",
                 mutateManifest([](auto &m) { m["format_version"] = "1"; }), false);
  tests.validate("wrong compiler contract rejected",
                 mutateManifest([](auto &m) { m["contract"] = "future"; }), false);
  tests.validate("wrong runtime rejected",
                 mutateManifest([](auto &m) { m["runtime"] = "host-libc"; }), false);
  tests.validate("reversed profiles rejected",
                 mutateManifest([](auto &m) {
                   m["profiles"] = llvm::json::Array{"i686", "x86_64"};
                 }), false);
  tests.validate("missing profile rejected",
                 mutateManifest([](auto &m) {
                   m["profiles"] = llvm::json::Array{"x86_64"};
                 }), false);
  tests.validate("unknown library rejected",
                 mutateManifest([](auto &m) {
                   m["libraries"] = llvm::json::Array{"host-secret"};
                 }), false);
  tests.validate("non-string library rejected",
                 mutateManifest([](auto &m) {
                   m["libraries"] = llvm::json::Array{17};
                 }), false);
  tests.validate("missing libraries rejected",
                 mutateManifest([](auto &m) { m.erase("libraries"); }), false);
  tests.validate("empty modules rejected",
                 mutateManifest([](auto &m) { m["modules"] = llvm::json::Array{}; }), false);
  tests.validate("non-object module rejected",
                 mutateManifest([](auto &m) { m["modules"] = llvm::json::Array{17}; }), false);
  tests.validate("wrong module path rejected",
                 mutateManifest([](auto &m) {
                   firstModule(m)["path"] = "modules/1.mlirbc";
                 }), false);
  tests.validate("source path in module record rejected",
                 mutateManifest([](auto &m) {
                   firstModule(m)["path"] = "private/source.c";
                 }), false);
  tests.validate("wrong module digest rejected",
                 mutateManifest([](auto &m) {
                   firstModule(m)["sha256"] = std::string(64, '0');
                 }), false);
  tests.validate("missing module digest rejected",
                 mutateManifest([](auto &m) { firstModule(m).erase("sha256"); }), false);
  tests.validate("unsupported optimization rejected",
                 mutateManifest([](auto &m) {
                   firstModule(m)["optimization"] = "Ofast";
                 }), false);
  tests.validate("unknown source-bearing manifest field rejected",
                 mutateManifest([](auto &m) {
                   m["source_code"] = "int private_application(void) { return 7; }";
                 }), false);
  tests.validate("unknown private module metadata rejected",
                 mutateManifest([](auto &m) {
                   firstModule(m)["debug_info"] = llvm::json::Object{
                       {"source", "private/source.c"}, {"line", 12}};
                 }), false);
  tests.validate("invalid application name rejected",
                 mutateManifest([](auto &m) { m["name"] = "../elsewhere"; }), false);
  tests.validate("overlong application name rejected",
                 mutateManifest([](auto &m) { m["name"] = std::string(65, 'a'); }), false);
  tests.validate("embedded NUL application name rejected",
                 mutateManifest([](auto &m) { m["name"] = std::string("ok\0bad", 6); }), false);

  auto files = validFiles();
  files.erase("manifest.json");
  tests.validate("missing manifest rejected", files, false);
  files = validFiles();
  files["manifest.json"] = "{";
  tests.validate("malformed JSON rejected", files, false);
  files["manifest.json"] = "[]";
  tests.validate("non-object manifest rejected", files, false);
  files["manifest.json"] = std::string(1024 * 1024 + 1, ' ');
  tests.validate("oversized manifest rejected", files, false);
  files = validFiles();
  files.erase("modules/0.mlirbc");
  tests.validate("missing module rejected", files, false);
  files = validFiles();
  files["modules/0.mlirbc"].push_back('x');
  tests.validate("corrupt module bytes rejected", files, false);
  for (const char *name : {"source.c", "private/input.ll", "capture.bc",
                           "private.ast", "debug.json", "resources/source.tar"}) {
    files = validFiles();
    files[name] = "fake private source/capture content";
    tests.validate(std::string("undeclared/private member rejected: ") + name,
                   files, false);
  }
  // Duplicate keys can hide private bytes even if a JSON parser retains only
  // the later value. The public manifest schema must not silently allow that.
  files = validFiles();
  files["manifest.json"] = "{\"name\":\"private source bytes\"," +
                           files["manifest.json"].substr(1);
  tests.validate("duplicate JSON key rejected", files, false);
  files = validFiles();
  auto position = files["manifest.json"].find("\"optimization\":");
  if (position == std::string::npos)
    throw std::runtime_error("test manifest has no optimization key");
  files["manifest.json"].insert(
      position, "\"optimization\":\"discarded private bytes\",");
  tests.validate("duplicate nested JSON key rejected", files, false);
}

void archiveTests(Tests &tests) {
  tests.archive("regular binary archive accepted",
                {{"manifest.json", "{}"}, {"modules/0.mlirbc", std::string("a\0b", 3)}}, true);
  for (const char *name : {"/absolute", "../escape", "modules/../escape",
                           "./manifest.json", "modules//0.mlirbc"})
    tests.archive(std::string("unsafe archive path rejected: ") + name,
                  {{name, "data"}}, false);
  tests.archive("overlong archive path rejected", {{std::string(257, 'a'), "x"}}, false);
  tests.archive("archive symlink rejected",
                {{"manifest.json", "", AE_IFLNK, "../outside"}}, false);
  tests.archive("archive hardlink rejected",
                {{"manifest.json", "{}"}, {"linked", "", AE_IFREG, "", "manifest.json"}}, false);
  tests.archive("archive directory rejected", {{"modules/", "", AE_IFDIR}}, false);
  tests.archive("archive FIFO rejected", {{"pipe", "", AE_IFIFO}}, false);
  tests.archive("duplicate archive member rejected",
                {{"manifest.json", "{}"}, {"manifest.json", "{}"}}, false);

  std::vector<Entry> entries;
  for (size_t i = 0; i < MaxFiles; ++i)
    entries.push_back({"member-" + std::to_string(i), ""});
  tests.archive("exact archive member-count limit accepted", entries, true);
  entries.push_back({"one-too-many", ""});
  tests.archive("archive member-count excess rejected", entries, false);

  Entry large{"large", ""};
  large.declaredSize = MaxBytes;
  tests.archive("exact expanded-byte limit accepted", {large}, true);
  large.declaredSize = MaxBytes + 1;
  tests.archive("oversized individual archive entry rejected", {large}, false);
  Entry halfA{"first", ""}, halfB{"second", ""};
  halfA.declaredSize = MaxBytes / 2;
  halfB.declaredSize = MaxBytes / 2 + 1;
  tests.archive("aggregate expanded-byte excess rejected", {halfA, halfB}, false);

  auto truncated = tests.nextPath();
  if (auto error = writeRawArchive(truncated, {{"payload", std::string(4096, 'x')}}))
    tests.check("truncated body rejected", false, llvm::toString(std::move(error)));
  else {
    // Plain short entry: one 512-byte header followed by an incomplete body.
    fs::resize_file(truncated, 512 + 17);
    tests.readArchive("truncated body rejected", truncated, false);
    fs::resize_file(truncated, 100);
    tests.readArchive("truncated header rejected", truncated, false);
  }
  auto oversized = tests.nextPath();
  if (auto error = writeRawArchive(oversized, {{"payload", "x"}}))
    tests.check("oversized outer file rejected", false, llvm::toString(std::move(error)));
  else {
    fs::resize_file(oversized, MaxBytes + 1024 * 1024 + 1);
    tests.readArchive("oversized outer file rejected", oversized, false);
  }
  tests.readArchive("missing archive rejected", tests.nextPath(), false);
  auto compressed = tests.nextPath();
  if (auto error = writeRawArchive(compressed, {{"payload", "x"}}, true))
    tests.check("compressed archive rejected", false, llvm::toString(std::move(error)));
  else
    tests.readArchive("compressed archive rejected", compressed, false);
}

void writerTests(Tests &tests) {
  auto path = tests.nextPath();
  auto files = validFiles();
  if (auto error = writePackage(path, files)) {
    tests.check("production writer round trip", false, llvm::toString(std::move(error)));
    return;
  }
  auto decoded = readPackage(path);
  if (!decoded) {
    tests.check("production writer round trip", false, llvm::toString(decoded.takeError()));
    return;
  }
  tests.check("production writer round trip", *decoded == files);
  tests.validate("round-trip manifest accepted", *decoded, true);
  auto second = tests.nextPath();
  if (auto error = writePackage(second, files))
    tests.check("deterministic package bytes", false, llvm::toString(std::move(error)));
  else {
    auto firstBytes = read(path);
    auto secondBytes = read(second);
    if (!firstBytes) {
      tests.check("deterministic package bytes", false, llvm::toString(firstBytes.takeError()));
      if (!secondBytes)
        llvm::consumeError(secondBytes.takeError());
    } else if (!secondBytes)
      tests.check("deterministic package bytes", false, llvm::toString(secondBytes.takeError()));
    else
      tests.check("deterministic package bytes", *firstBytes == *secondBytes);
  }
  auto overwrite = writePackage(path, {{"wrong", "replacement"}});
  bool refused = static_cast<bool>(overwrite);
  if (overwrite)
    llvm::consumeError(std::move(overwrite));
  tests.check("writer refuses existing output", refused);
  decoded = readPackage(path);
  if (!decoded)
    tests.check("existing output remains intact", false, llvm::toString(decoded.takeError()));
  else
    tests.check("existing output remains intact", *decoded == files);

  PackageFiles tooMany;
  for (size_t i = 0; i <= MaxFiles; ++i)
    tooMany["file-" + std::to_string(i)] = "";
  auto rejectedPath = tests.nextPath();
  auto countError = writePackage(rejectedPath, tooMany);
  bool countRejected = static_cast<bool>(countError);
  if (countError)
    llvm::consumeError(std::move(countError));
  tests.check("writer rejects excess members without output",
              countRejected && !fs::exists(rejectedPath));

  PackageFiles tooLarge{{"large", std::string(MaxBytes + 1, 'x')}};
  rejectedPath = tests.nextPath();
  auto sizeError = writePackage(rejectedPath, tooLarge);
  bool sizeRejected = static_cast<bool>(sizeError);
  if (sizeError)
    llvm::consumeError(std::move(sizeError));
  tests.check("writer rejects excess bytes without output",
              sizeRejected && !fs::exists(rejectedPath));
}
} // namespace

int main() {
  try {
    auto scratch = Scratch::create();
    if (!scratch) {
      llvm::logAllUnhandledErrors(scratch.takeError(), llvm::errs(), "package tests: ");
      return 1;
    }
    Tests tests(scratch->path);
    manifestTests(tests);
    archiveTests(tests);
    writerTests(tests);
    return tests.finish();
  } catch (const std::exception &error) {
    llvm::errs() << "package tests: " << error.what() << '\n';
    return 1;
  }
}
