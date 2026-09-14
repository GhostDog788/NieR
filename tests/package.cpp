#include "sela/Artifact/Artifact.h"
#include "llvm/Support/raw_ostream.h"
#include <archive.h>
#include <archive_entry.h>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>

using namespace sela::driver;

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
  modules.push_back(llvm::json::Object{{"path", "modules/0.selabc"},
                                      {"sha256", digest(module)},
                                      {"targets", llvm::json::Array{"x86_64", "i686"}}});
  llvm::json::Object plans;
  for (const char *target : {"x86_64", "i686"})
    plans[target] = llvm::json::Array{llvm::json::Object{
        {"modules", llvm::json::Array{0}}, {"optimization", "O2"}}};
  PackageFiles result;
  result["modules/0.selabc"] = module;
  result["manifest.json"] = jsonText(llvm::json::Object{
      {"format_version", ArtifactFormatVersion}, {"contract", Contract}, {"kind", "executable"},
      {"runtime", "glibc-2.39-0ubuntu8.8"},
      {"targets", llvm::json::Array{"x86_64", "i686"}},
      {"libraries", llvm::json::Array{}}, {"link_options", llvm::json::Array{}}, {"modules", std::move(modules)},
      {"compilation_units", std::move(plans)}});
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
llvm::json::Object &firstUnit(llvm::json::Object &manifest, llvm::StringRef target = "x86_64") {
  return *(*manifest.getObject("compilation_units")->getArray(target))[0].getAsObject();
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
  multiple["modules/1.selabc"] = "another opaque module";
  auto multipleManifest = llvm::json::parse(multiple.at("manifest.json"));
  if (!multipleManifest)
    throw std::runtime_error(llvm::toString(multipleManifest.takeError()));
  multipleManifest->getAsObject()->getArray("modules")->push_back(
      llvm::json::Object{{"path", "modules/1.selabc"},
                         {"sha256", digest(multiple.at("modules/1.selabc"))},
                         {"targets", llvm::json::Array{"x86_64", "i686"}}});
  for (const char *target : {"x86_64", "i686"})
    multipleManifest->getAsObject()->getObject("compilation_units")->getArray(target)->push_back(
        llvm::json::Object{{"modules", llvm::json::Array{1}}, {"optimization", "O0"}});
  multiple["manifest.json"] = jsonText(std::move(*multipleManifest));
  tests.validate("multiple translation units accepted", multiple, true);
  for (const char *level : {"O0", "O1", "O2", "O3", "Os", "Oz"})
    tests.validate(std::string("optimization accepted: ") + level,
                   mutateManifest([&](auto &m) {
                     firstUnit(m)["optimization"] = level;
                   }), true);
  tests.validate("declared managed libm accepted",
                 mutateManifest([](auto &m) {
                   m["libraries"] = llvm::json::Array{"m"};
                 }), true);
  tests.validate("exact shared library import accepted",
                 mutateManifest([](auto &m) {
                   m["libraries"] = llvm::json::Array{":libz.so.1"};
                 }), true);
  tests.validate("path in exact shared library import rejected",
                 mutateManifest([](auto &m) {
                   m["libraries"] = llvm::json::Array{":../libz.so.1"};
                 }), false);
  tests.validate("executable dynamic exports accepted",
                 mutateManifest([](auto &m) {
                   m["link_options"] = llvm::json::Array{"--export-dynamic", "--hash-style=both", "--undefined-version"};
                 }), true);
  tests.validate("shared library SONAME accepted",
                 mutateManifest([](auto &m) {
                   m["kind"] = "shared";
                   m["link_options"] = llvm::json::Array{"-soname=libexample.so.1"};
                 }), true);
  tests.validate("SONAME on executable rejected",
                 mutateManifest([](auto &m) {
                   m["link_options"] = llvm::json::Array{"-soname=libexample.so.1"};
                 }), false);
  tests.validate("path in SONAME rejected",
                 mutateManifest([](auto &m) {
                   m["kind"] = "shared";
                   m["link_options"] = llvm::json::Array{"-soname=../libexample.so.1"};
                 }), false);
  tests.validate("unqualified linker input rejected",
                 mutateManifest([](auto &m) {
                   m["link_options"] = llvm::json::Array{"--script=/tmp/input"};
                 }), false);
  tests.validate("static archive requires member identities",
                 mutateManifest([](auto &m) { m["kind"] = "static"; }), false);
  auto emptyArchive = createArtifact("static", {});
  if (!emptyArchive)
    tests.check("empty static archive accepted", false, llvm::toString(emptyArchive.takeError()));
  else
    tests.validate("empty static archive accepted", *emptyArchive, true);
  tests.check("default publication domain covers all four distinct targets",
      defaultArtifactTargets() == std::vector<std::string>{"x86_64", "i686", "armv7", "aarch64"});
  auto armArchive = createArtifact("static", {}, {}, {}, {"armv7", "aarch64"});
  if (!armArchive)
    tests.check("independent ARM target domain accepted", false, llvm::toString(armArchive.takeError()));
  else
    tests.validate("independent ARM target domain accepted", *armArchive, true);
  tests.validate("static archive member accepted",
                 mutateManifest([](auto &m) {
                   m["kind"] = "static";
                   for (const char *target : {"x86_64", "i686"}) firstUnit(m, target)["archive_member"] = "code.c.o";
                 }), true);
  tests.validate("static archive member cannot name a path",
                 mutateManifest([](auto &m) {
                   m["kind"] = "static";
                   for (const char *target : {"x86_64", "i686"}) firstUnit(m, target)["archive_member"] = "../code.o";
                 }), false);
  tests.validate("archive identity on nonstatic module rejected",
                 mutateManifest([](auto &m) {
                   firstUnit(m)["archive_member"] = "code.o";
                 }), false);
  tests.validate("static archive cannot carry executable link flags",
                 mutateManifest([](auto &m) {
                   m["kind"] = "static";
                   for (const char *target : {"x86_64", "i686"}) firstUnit(m, target)["archive_member"] = "code.o";
                   m["link_options"] = llvm::json::Array{"--export-dynamic"};
                 }), false);
  auto duplicateMembers = createArtifact("static", {{"first", "O0", "same.o"}, {"second", "O2", "same.o"}});
  if (!duplicateMembers)
    tests.check("duplicate static member names accepted in physical order", false,
                llvm::toString(duplicateMembers.takeError()));
  else
    tests.validate("duplicate static member names accepted in physical order", *duplicateMembers, true);
  tests.validate("unknown format version rejected",
                 mutateManifest([](auto &m) { m["format_version"] = 1; }), false);
  auto targetLinks = [](llvm::json::Object &m) {
    llvm::json::Object links;
    links["x86_64"] = llvm::json::Object{
        {"libraries", llvm::json::Array{}}, {"link_options", llvm::json::Array{}}};
    links["i686"] = llvm::json::Object{
        {"libraries", llvm::json::Array{"m"}}, {"link_options", llvm::json::Array{}}};
    m["target_links"] = std::move(links);
  };
  tests.validate("target-specific libraries accepted", mutateManifest(targetLinks), true);
  tests.validate("missing target link record rejected", mutateManifest([&](auto &m) {
    targetLinks(m); m.getObject("target_links")->erase("i686");
  }), false);
  tests.validate("foreign target link record rejected", mutateManifest([&](auto &m) {
    targetLinks(m);
    (*m.getObject("target_links"))["aarch64"] = llvm::json::Object{
        {"libraries", llvm::json::Array{}}, {"link_options", llvm::json::Array{}}};
  }), false);
  tests.validate("common and per-target libraries cannot mix", mutateManifest([&](auto &m) {
    targetLinks(m); m["libraries"] = llvm::json::Array{"m"};
  }), false);
  tests.validate("common and per-target flags cannot mix", mutateManifest([&](auto &m) {
    targetLinks(m); m["link_options"] = llvm::json::Array{"--export-dynamic"};
  }), false);
  tests.validate("unknown inactive target link field rejected", mutateManifest([&](auto &m) {
    targetLinks(m); (*m.getObject("target_links")->getObject("i686"))["raw_flags"] = "-evil";
  }), false);
  tests.validate("inactive target library paths rejected", mutateManifest([&](auto &m) {
    targetLinks(m);
    (*m.getObject("target_links")->getObject("i686"))["libraries"] = llvm::json::Array{"/tmp/libbad.so"};
  }), false);
  auto scopedScript = mutateManifest([&](auto &m) {
    targetLinks(m);
    m["kind"] = "shared";
    (*m.getObject("target_links")->getObject("i686"))["version_script"] = llvm::json::Object{
        {"path", "link/i686.version.script"}, {"sha256", digest("API { global: entry; };\n")}};
  });
  scopedScript["link/i686.version.script"] = "API { global: entry; };\n";
  tests.validate("target-specific version script accepted", scopedScript, true);
  scopedScript["link/i686.version.script"] += "# tampered\n";
  tests.validate("inactive target script digest checked", scopedScript, false);
  tests.validate("string format version rejected",
                 mutateManifest([](auto &m) { m["format_version"] = "1"; }), false);
  tests.validate("wrong compiler contract rejected",
                 mutateManifest([](auto &m) { m["contract"] = "future"; }), false);
  tests.validate("wrong runtime rejected",
                 mutateManifest([](auto &m) { m["runtime"] = "host-libc"; }), false);
  tests.validate("target order is not producer provenance",
                 mutateManifest([](auto &m) {
                   m["targets"] = llvm::json::Array{"i686", "x86_64"};
                 }), true);
  tests.validate("independently qualified target subset accepted",
                 mutateManifest([](auto &m) {
                   m["targets"] = llvm::json::Array{"x86_64"};
                   firstModule(m)["targets"] = llvm::json::Array{"x86_64"};
                   m.getObject("compilation_units")->erase("i686");
                 }), true);
  tests.validate("inactive fragments may be absent from a target plan",
                 mutateManifest([](auto &m) {
                   firstModule(m)["targets"] = llvm::json::Array{"x86_64"};
                   (*m.getObject("compilation_units"))["i686"] = llvm::json::Array{};
                 }), true);
  tests.validate("inactive fragment cannot be compiled on another target",
                 mutateManifest([](auto &m) {
                   firstModule(m)["targets"] = llvm::json::Array{"x86_64"};
                 }), false);
  tests.validate("active fragment cannot be omitted",
                 mutateManifest([](auto &m) {
                   (*m.getObject("compilation_units"))["x86_64"] = llvm::json::Array{};
                 }), false);
  tests.validate("missing fragment domain rejected",
                 mutateManifest([](auto &m) { firstModule(m).erase("targets"); }), false);
  tests.validate("fragment domain outside artifact rejected",
                 mutateManifest([](auto &m) {
                   firstModule(m)["targets"] = llvm::json::Array{"armv7"};
                 }), false);
  tests.validate("duplicate fragment target rejected",
                 mutateManifest([](auto &m) {
                   firstModule(m)["targets"] = llvm::json::Array{"x86_64", "x86_64"};
                 }), false);
  tests.validate("unknown library rejected",
                 mutateManifest([](auto &m) {
                   m["libraries"] = llvm::json::Array{"../host-secret"};
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
                   firstModule(m)["path"] = "modules/1.selabc";
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
                   firstUnit(m)["optimization"] = "Ofast";
                 }), false);
  tests.validate("old per-fragment optimization metadata rejected",
                 mutateManifest([](auto &m) { firstModule(m)["optimization"] = "O2"; }), false);
  tests.validate("missing compilation plan rejected",
                 mutateManifest([](auto &m) { m.erase("compilation_units"); }), false);
  tests.validate("compilation plan cannot omit a target",
                 mutateManifest([](auto &m) { m.getObject("compilation_units")->erase("i686"); }), false);
  tests.validate("compilation plan cannot add an unknown target",
                 mutateManifest([](auto &m) { (*m.getObject("compilation_units"))["arm"] = llvm::json::Array{}; }), false);
  tests.validate("empty compilation unit rejected",
                 mutateManifest([](auto &m) { firstUnit(m)["modules"] = llvm::json::Array{}; }), false);
  tests.validate("dropped fragment rejected",
                 mutateManifest([](auto &m) { (*m.getObject("compilation_units"))["x86_64"] = llvm::json::Array{}; }), false);
  tests.validate("duplicated fragment rejected",
                 mutateManifest([](auto &m) { firstUnit(m)["modules"] = llvm::json::Array{0, 0}; }), false);
  tests.validate("out-of-range fragment rejected",
                 mutateManifest([](auto &m) { firstUnit(m)["modules"] = llvm::json::Array{1}; }), false);
  tests.validate("negative fragment rejected",
                 mutateManifest([](auto &m) { firstUnit(m)["modules"] = llvm::json::Array{-1}; }), false);
  tests.validate("noninteger fragment rejected",
                 mutateManifest([](auto &m) { firstUnit(m)["modules"] = llvm::json::Array{"0"}; }), false);
  tests.validate("private compilation-unit payload rejected",
                 mutateManifest([](auto &m) { firstUnit(m)["source"] = "private code"; }), false);
  auto grouped = createArtifact("executable", {{"first"}, {"second"}}, {}, {}, {"x86_64", "i686"}, {},
      {{"x86_64", {{{0}, "O0"}, {{1}, "O2"}}}, {"i686", {{{0, 1}, "O3"}}}});
  if (!grouped) tests.check("different native-unit partitions accepted", false, llvm::toString(grouped.takeError()));
  else tests.validate("different native-unit partitions accepted", *grouped, true);
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
  files.erase("modules/0.selabc");
  tests.validate("missing module rejected", files, false);
  files = validFiles();
  files["modules/0.selabc"].push_back('x');
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

void versionScriptTests(Tests &tests) {
  auto files = createArtifact("shared", {{"module", "O2"}}, {},
      {"-soname=libfixture.so.1"}, {"x86_64", "i686"},
      "/* private source /home/builder/code */\nLIB_1 { global: public_*; INPUT_value; local: *; };\n");
  if (!files) {
    tests.check("version script factory", false, llvm::toString(files.takeError()));
    return;
  }
  tests.validate("normalized C version script accepted", *files, true);
  tests.check("version script comments not published",
      files->at("link/version.script").find("private source") == std::string::npos);
  auto corrupt = *files;
  corrupt["link/version.script"] += " ";
  tests.validate("version script digest mismatch rejected", corrupt, false);
  corrupt = *files;
  corrupt.erase("link/version.script");
  tests.validate("missing version script rejected", corrupt, false);
  for (llvm::StringRef text : {"/* unfinished", "# comments only", "{ global: \"quoted\"; };", "INCLUDE /tmp/native-input"}) {
    auto rejected = normalizeVersionScript(text);
    tests.check("unqualified version script rejected: " + text.str(), !rejected);
    if (!rejected) llvm::consumeError(rejected.takeError());
  }
  auto executable = createArtifact("executable", {{"module", "O2"}}, {}, {},
      {"x86_64"}, "{ global: main; };");
  tests.check("unqualified executable version script rejected", !executable);
  if (!executable) llvm::consumeError(executable.takeError());
}

void archiveTests(Tests &tests) {
  tests.archive("regular binary archive accepted",
                {{"manifest.json", "{}"}, {"modules/0.selabc", std::string("a\0b", 3)}}, true);
  for (const char *name : {"/absolute", "../escape", "modules/../escape",
                           "./manifest.json", "modules//0.selabc"})
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
  // libarchive exposes time_t in its public ABI. A header/library width
  // mismatch can corrupt archive metadata even when every tar operation
  // reports success (notably Noble's ARM32 libarchive13t64).
  std::unique_ptr<archive_entry, decltype(&archive_entry_free)> timeEntry(
      archive_entry_new(), archive_entry_free);
  if (!timeEntry) {
    tests.check("libarchive timestamp ABI", false, "cannot allocate archive entry");
    return;
  }
  archive_entry_set_mtime(timeEntry.get(), 0, 0);
  tests.check("libarchive timestamp ABI preserves exact zero",
              archive_entry_mtime_is_set(timeEntry.get()) &&
                  archive_entry_mtime(timeEntry.get()) == time_t{0} &&
                  archive_entry_mtime_nsec(timeEntry.get()) == 0,
              "time_t bytes=" + std::to_string(sizeof(time_t)));
  const time_t seconds = static_cast<time_t>(sizeof(time_t) > 4 ? 3703703670LL
                                                              : 1234567890LL);
  constexpr long nanoseconds = 314159265;
  archive_entry_set_mtime(timeEntry.get(), seconds, nanoseconds);
  tests.check("libarchive timestamp ABI preserves seconds and nanoseconds",
              archive_entry_mtime(timeEntry.get()) == seconds &&
                  archive_entry_mtime_nsec(timeEntry.get()) == nanoseconds);
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
  tests.check("writer refuses invalid replacement", refused);
  decoded = readPackage(path);
  if (!decoded)
    tests.check("existing output remains intact", false, llvm::toString(decoded.takeError()));
  else
    tests.check("existing output remains intact", *decoded == files);

  auto fifo = tests.nextPath();
  if (::mkfifo(fifo.c_str(), 0600) != 0)
    tests.check("special output fixture created", false);
  else {
    auto error = writePackage(fifo, files);
    tests.check("writer preserves and rejects FIFO output", bool(error) && fs::is_fifo(fifo));
    if (error) llvm::consumeError(std::move(error));
  }

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
    versionScriptTests(tests);
    archiveTests(tests);
    writerTests(tests);
    return tests.finish();
  } catch (const std::exception &error) {
    llvm::errs() << "package tests: " << error.what() << '\n';
    return 1;
  }
}
