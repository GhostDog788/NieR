#include "Build.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/FileSystem.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <set>
#include <sys/wait.h>
#include <unistd.h>

namespace nier::driver {
namespace {
fs::path absolutePath(const fs::path &path) {
  return fs::weakly_canonical(fs::absolute(path));
}
bool inside(const fs::path &path, const fs::path &root) {
  auto relative = path.lexically_relative(root);
  return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
std::string normalize(std::string text, const fs::path &root) {
  const std::string prefix = root.string();
  size_t position = 0;
  while ((position = text.find(prefix, position)) != std::string::npos) {
    text.replace(position, prefix.size(), "$PRIVATE");
    position += 8;
  }
  return text;
}
llvm::Expected<std::vector<std::string>> strings(const llvm::json::Object &object,
                                                llvm::StringRef key) {
  std::vector<std::string> result;
  auto value = object.get(key);
  if (!value) return result;
  auto array = value->getAsArray();
  if (!array) return fail("private build field must be a string array: " + key.str());
  for (const auto &item : *array) {
    auto text = item.getAsString();
    if (!text || text->contains('\0')) return fail("invalid private build string");
    result.push_back(text->str());
  }
  return result;
}
llvm::Error copySource(const fs::path &source, const fs::path &destination,
                       const fs::path &root) {
  fs::create_directories(destination);
  for (const auto &entry : fs::directory_iterator(source)) {
    auto name = entry.path().filename();
    if (name == ".git" || name == ".sdk") continue;
    auto target = destination / name;
    if (entry.is_symlink()) {
      auto resolved = fs::canonical(entry.path());
      if (!inside(resolved, root) || !fs::is_regular_file(resolved))
        return fail("source symlink needs explicit SDK integration: " + entry.path().string());
      fs::copy_file(resolved, target);
    } else if (entry.is_directory()) {
      if (auto error = copySource(entry.path(), target, root)) return error;
    } else if (entry.is_regular_file()) fs::copy_file(entry.path(), target);
    else return fail("unsupported special source file: " + entry.path().string());
  }
  return llvm::Error::success();
}
fs::path metadataPath(const fs::path &directory, const fs::path &output,
                       llvm::StringRef kind) {
  return directory / (digest(absolutePath(output).string()) + "." + kind.str() + ".json");
}
llvm::Error atomicJson(const fs::path &path, llvm::json::Value value) {
  fs::path temporary = path.string() + "." + std::to_string(getpid()) + ".tmp";
  if (auto error = write(temporary, jsonText(std::move(value)))) return error;
  fs::rename(temporary, path);
  return llvm::Error::success();
}
llvm::Expected<std::vector<std::string>> expandResponses(
    const std::vector<std::string> &arguments, unsigned depth = 0) {
  if (depth > 8) return fail("response-file nesting exceeds capture limit");
  std::vector<std::string> result;
  for (const auto &argument : arguments) {
    if (argument.empty() || argument.front() != '@') { result.push_back(argument); continue; }
    auto contents = read(argument.substr(1), 1024 * 1024);
    if (!contents) return contents.takeError();
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver saver(allocator);
    llvm::SmallVector<const char *, 32> tokens;
    llvm::cl::TokenizeGNUCommandLine(*contents, saver, tokens);
    std::vector<std::string> nested(tokens.begin(), tokens.end());
    auto expanded = expandResponses(nested, depth + 1);
    if (!expanded) return expanded.takeError();
    result.insert(result.end(), expanded->begin(), expanded->end());
    if (result.size() > 16384) return fail("too many linker arguments");
  }
  return result;
}
std::string quoteConfig(llvm::StringRef argument) {
  std::string result = "\"";
  for (char c : argument) {
    if (c == '\\' || c == '"') result.push_back('\\');
    result.push_back(c);
  }
  return result + "\"\n";
}
std::string shellQuote(const std::string &argument) {
  std::string result = "'";
  for (char c : argument) result += c == '\'' ? "'\\''" : std::string(1, c);
  return result + "'";
}
llvm::Expected<std::vector<std::string>> captureMarkers(
    const llvm::object::ObjectFile &object, llvm::StringRef name) {
  std::vector<std::string> result;
  bool seen = false;
  for (const auto &section : object.sections()) {
    auto sectionName = section.getName();
    if (!sectionName) return sectionName.takeError();
    if (*sectionName != ".nier.capture") continue;
    if (seen) return fail("multiple private capture sections in " + name.str());
    seen = true;
    auto contents = section.getContents();
    if (!contents) return contents.takeError();
    if (contents->size() > 8 * 1024 * 1024)
      return fail("private capture witness exceeds limit: " + name.str());
    llvm::StringRef remaining = *contents;
    while (!remaining.empty()) {
      const auto end = remaining.find('\0');
      if (end == 0 || end == llvm::StringRef::npos || end > 4096)
        return fail("invalid private capture witness in " + name.str());
      result.push_back(remaining.take_front(end).str());
      remaining = remaining.drop_front(end + 1);
    }
  }
  return result;
}
llvm::Expected<llvm::json::Value> consumedObject(llvm::StringRef bytes,
    llvm::StringRef name, const fs::path &metadata, bool retained = false) {
  auto object = llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(bytes, name));
  if (!object) return object.takeError();
  auto markers = captureMarkers(**object, name);
  if (!markers) return markers.takeError();
  if (markers->empty()) return fail("native object has no private capture marker: " + name.str());
  if (markers->size() != 1) return fail("native object has multiple private capture markers: " + name.str());
  fs::path journalPath(markers->front());
  if (!journalPath.is_absolute() || !inside(absolutePath(journalPath), metadata))
    return fail("object capture journal escapes private metadata: " + name.str());
  auto journal = readJson(journalPath);
  if (!journal) return journal.takeError();
  auto *item = journal->getAsObject();
  if (!item || !item->getString("capture") || !item->getString("capture_sha256") ||
      !item->getString("native_sha256") ||
      !item->getString("source") || !item->getString("optimization") ||
      !item->getArray("flags") || !item->getArray("dependencies"))
    return fail("invalid immutable native capture journal: " + journalPath.string());
  if (*item->getString("native_sha256") != digest(bytes))
    return fail("native object changed after successful Clang code generation: " + name.str());
  if (!inside(absolutePath(item->getString("capture")->str()), metadata))
    return fail("LLVM capture escapes private metadata");
  auto captured = read(item->getString("capture")->str());
  if (!captured) return captured.takeError();
  if (digest(*captured) != *item->getString("capture_sha256"))
    return fail("LLVM capture changed after native code generation");
  for (auto &dependency : *item->getArray("dependencies")) {
    auto *input = dependency.getAsObject();
    if (!input || !input->getString("path") || !input->getString("sha256"))
      return fail("invalid compiler input provenance");
    auto contents = read(input->getString("path")->str());
    if (!contents) return contents.takeError();
    if (digest(*contents) != *input->getString("sha256"))
      return fail("compiler input changed before link: " + input->getString("path")->str());
  }
  fs::path saved = metadata / (digest(bytes) + ".native.o");
  if (retained) {
    // Requalification can consume retained evidence but must never repair a
    // missing object snapshot or silently create evidence that was not saved.
    if (!inside(absolutePath(saved), metadata) || !fs::is_regular_file(saved))
      return fail("retained original native object is missing or escaped metadata");
    auto original = read(saved);
    if (!original) return original.takeError();
    if (*original != bytes) return fail("retained original native object changed");
  } else if (!fs::exists(saved)) {
    if (auto error = write(saved, bytes)) return error;
  }
  (*item)["native_sha256"] = digest(bytes);
  (*item)["native_object"] = saved.string();
  return std::move(*journal);
}
struct DynamicInfo { std::string soname; std::vector<std::string> needed; };
template <typename ELFT> llvm::Expected<DynamicInfo> dynamicInfo(
    const llvm::object::ELFObjectFile<ELFT> &object) {
  const auto &elf = object.getELFFile();
  auto sections = elf.sections();
  if (!sections) return sections.takeError();
  DynamicInfo result;
  for (const auto &section : *sections) {
    if (section.sh_type != llvm::ELF::SHT_DYNAMIC) continue;
    auto stringsSection = elf.getSection(section.sh_link);
    if (!stringsSection) return stringsSection.takeError();
    auto table = elf.getStringTable(**stringsSection);
    if (!table) return table.takeError();
    auto entries = elf.template getSectionContentsAsArray<typename ELFT::Dyn>(section);
    if (!entries) return entries.takeError();
    for (const auto &entry : *entries) {
      if (entry.d_tag != llvm::ELF::DT_NEEDED && entry.d_tag != llvm::ELF::DT_SONAME) continue;
      const uint64_t offset = entry.d_un.d_val;
      if (offset >= table->size()) return fail("invalid ELF dynamic string offset");
      llvm::StringRef tail = table->drop_front(offset);
      auto end = tail.find('\0');
      if (end == llvm::StringRef::npos) return fail("unterminated ELF dynamic name");
      auto name = tail.take_front(end);
      if (name.empty() || name.size() > 255 || name.contains('/') || name == "." || name == "..")
        return fail("nonportable native dependency identity");
      if (entry.d_tag == llvm::ELF::DT_NEEDED) result.needed.push_back(name.str());
      else result.soname = name.str();
    }
  }
  return result;
}
llvm::Expected<DynamicInfo> dynamicInfo(const llvm::object::ObjectFile &object) {
  if (auto *elf = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(&object)) return dynamicInfo(*elf);
  if (auto *elf = llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(&object)) return dynamicInfo(*elf);
  return fail("native build observer requires a qualified little-endian ELF object");
}
llvm::Expected<std::string> selectedArchiveMember(const fs::path &path,
    llvm::StringRef memberName, const fs::path &lane, llvm::StringRef expectedJournal) {
  auto bytes = read(path);
  if (!bytes) return bytes.takeError();
  // Archive keeps its MemoryBufferRef identifier, including while resolving
  // thin members relative to that archive. Do not pass a temporary path string.
  const std::string archiveName = path.string();
  auto archive = llvm::object::Archive::create(llvm::MemoryBufferRef(*bytes, archiveName));
  if (!archive) return archive.takeError();
  std::string result;
  bool found = false;
  llvm::Error iteration = llvm::Error::success();
  for (const auto &child : (*archive)->children(iteration)) {
    auto name = child.getName();
    if (!name) return name.takeError();
    if (*name != memberName) continue;
    if ((*archive)->isThin()) {
      auto memberPath = child.getFullName();
      if (!memberPath) return memberPath.takeError();
      if (!inside(absolutePath(*memberPath), lane))
        return fail("thin archive member escapes private build: " + memberName.str());
    }
    auto contents = child.getBuffer();
    if (!contents) return contents.takeError();
    if (contents->size() > 64 * 1024 * 1024) return fail("archive member exceeds capture limit");
    auto object = llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(*contents, *name));
    if (!object) { llvm::consumeError(object.takeError()); continue; }
    auto markers = captureMarkers(**object, *name);
    if (!markers) { llvm::consumeError(markers.takeError()); continue; }
    if (markers->size() != 1 || markers->front() != expectedJournal) continue;
    if (found) return fail("ambiguous repeated-journal archive member: " + memberName.str());
    result = contents->str(); found = true;
  }
  if (iteration) return std::move(iteration);
  if (!found) return fail("archive member does not match final native capture witness: " + memberName.str());
  return result;
}
llvm::Error runTracedLink(const std::vector<std::string> &command, int traceDescriptor) {
  const pid_t child = fork();
  if (child < 0) { close(traceDescriptor); return fail("cannot fork private native linker"); }
  if (child == 0) {
    if (dup2(traceDescriptor, STDOUT_FILENO) < 0) _exit(126);
    close(traceDescriptor);
    std::vector<char *> args;
    for (const auto &arg : command) args.push_back(const_cast<char *>(arg.c_str()));
    args.push_back(nullptr);
    execvp(args.front(), args.data());
    _exit(127);
  }
  close(traceDescriptor);
  int status;
  while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return fail("native linker wait failed");
  if (!WIFEXITED(status) || WEXITSTATUS(status)) return fail("stock native LLD failed");
  return llvm::Error::success();
}
llvm::Error recordNativeLink(const std::vector<std::string> &args, const Sdk &sdk,
    const fs::path &lane, const fs::path &metadata, const fs::path &trace,
    const fs::path &whyExtract) {
  fs::path output = "a.out";
  std::string kind = "executable";
  std::string versionScript;
  std::string interpreter, hashStyle = "gnu";
  llvm::json::Array units, libraries, linkOptions, unsupported;
  const std::set<std::string> valued = {"-m", "-z", "-rpath", "--rpath", "-rpath-link",
    "--rpath-link", "-dynamic-linker", "--dynamic-linker", "-L"};
  const std::set<std::string> ordinary = {"-pie", "--pie", "--eh-frame-hdr", "--as-needed",
    "--no-as-needed", "--enable-new-dtags", "--build-id", "-s", "--strip-all",
    "--whole-archive", "--no-whole-archive", "--start-group", "--end-group", "-(", "-)", "-t", "--trace"};
  for (size_t i = 0; i < args.size(); ++i) {
    llvm::StringRef arg(args[i]);
    if (arg == "-o") {
      if (++i == args.size()) return fail("native linker -o has no value");
      output = args[i];
    } else if (arg.starts_with("-o") && arg.size() > 2) output = arg.drop_front(2).str();
    else if (arg == "-shared" || arg == "--shared") kind = "shared";
    else if (arg == "-soname" || arg == "--soname" || arg == "-h") {
      if (++i == args.size()) return fail("native linker SONAME has no value");
      linkOptions.push_back("-soname"); linkOptions.push_back(args[i]);
    } else if (arg.starts_with("--soname=")) {
      linkOptions.push_back("-soname"); linkOptions.push_back(arg.drop_front(9).str());
    } else if (arg == "-export-dynamic" || arg == "--export-dynamic" || arg == "-E") {
      linkOptions.push_back("--export-dynamic");
    } else if (arg == "--hash-style=both" || arg == "--hash-style=gnu") {
      // LLD honors the last setting; don't preserve an overridden earlier one.
      hashStyle = arg.drop_front(13).str();
    } else if (arg.starts_with("--dynamic-linker=")) {
      interpreter = arg.drop_front(17).str();
    } else if (arg == "--undefined-version") {
      linkOptions.push_back(arg.str());
    } else if (valued.count(arg.str())) {
      if (++i == args.size()) return fail("native linker option has no value");
      const auto &value = args[i];
      if (arg == "-dynamic-linker" || arg == "--dynamic-linker") interpreter = value;
      if (arg == "-z" && value != "relro" && value != "now" && value != "nodefaultlib")
        unsupported.push_back("unqualified native -z setting " + value);
      if ((arg == "-L" || arg == "-rpath" || arg == "--rpath" || arg == "-rpath-link" ||
           arg == "--rpath-link") && !inside(absolutePath(value), absolutePath(sdk.root)) &&
           !inside(absolutePath(value), lane) && absolutePath(value) != lane)
        unsupported.push_back("library search path outside captured build/SDK " + normalize(value, lane));
    } else if (arg == "-lc") {
      // Supplied C runtime is a native SDK dependency, never application code.
    } else if (arg.starts_with("-l")) {
      // LLD's actual selected input trace and DT_NEEDED govern dependencies.
    } else if (arg == "--version-script" || arg.starts_with("--version-script=")) {
      fs::path script;
      if (arg == "--version-script") {
        if (++i == args.size()) return fail("version script has no path");
        script = args[i];
      } else script = arg.drop_front(17).str();
      if (!versionScript.empty()) return fail("multiple version scripts are not yet qualified");
      auto contents = read(script, 1024 * 1024);
      if (!contents) return contents.takeError();
      versionScript = std::move(*contents);
    } else if (arg == "-r" || arg == "--relocatable" || arg == "-static" || arg == "-T") {
      unsupported.push_back("unqualified link semantic " + arg.str());
      if (arg == "-T") {
        if (++i == args.size()) return fail("native linker script has no value");
      }
    } else if (!arg.starts_with("-") && !arg.empty()) {
      // Resolve real objects, selected archive members and DSOs from LLD's
      // input trace, not by guessing from file extensions or all archive entries.
    } else if (arg.starts_with("-L")) {
      auto directory = absolutePath(arg.drop_front(2).str());
      if (!inside(directory, absolutePath(sdk.root)) && !inside(directory, lane) && directory != lane)
        unsupported.push_back("library search path outside captured build/SDK " + normalize(arg.str(), lane));
    } else if (!ordinary.count(arg.str()) &&
               !arg.starts_with("--sysroot=") &&
               arg != "--build-id=fast")
      unsupported.push_back("unknown native link semantic " + arg.str());
  }
  output = absolutePath(output);
  if (output == "/dev/null") return llvm::Error::success();
  if (!inside(output, lane)) return fail("native link output escaped private build lane");
  if (hashStyle == "both") linkOptions.push_back("--hash-style=both");
  if (kind == "executable") {
    const char *profile = std::getenv("NIER_BUILD_PROFILE");
    if (!profile || (std::string(profile) != "x86_64" && std::string(profile) != "i686"))
      return fail("native linker lacks a qualified profile");
    const bool x64 = std::string(profile) == "x86_64";
    const auto expectedInterpreter = sdk.sysroot(profile) / "usr/lib" /
        (x64 ? "x86_64-linux-gnu" : "i386-linux-gnu") /
        (x64 ? "ld-linux-x86-64.so.2" : "ld-linux.so.2");
    if (interpreter != expectedInterpreter.string())
      unsupported.push_back("unqualified native dynamic interpreter " + interpreter);
  }
  auto bytes = read(output);
  if (!bytes) return bytes.takeError();
  const std::string outputName = output.string();
  auto outputObject = llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(*bytes, outputName));
  if (!outputObject) return outputObject.takeError();
  auto witness = captureMarkers(**outputObject, outputName);
  std::vector<std::string> selectedJournals;
  if (!witness) unsupported.push_back(llvm::toString(witness.takeError()));
  else selectedJournals = std::move(*witness);
  std::set<std::string> uniqueJournals;
  for (const auto &journal : selectedJournals)
    if (!uniqueJournals.insert(journal).second)
      unsupported.push_back("repeated journal in final native capture witness");
  size_t markerPosition = 0;
  auto traceText = read(trace, 8 * 1024 * 1024);
  if (!traceText) return traceText.takeError();
  auto extraction = read(whyExtract, 8 * 1024 * 1024);
  if (!extraction) return extraction.takeError();
  std::map<std::string, fs::path> privateDSOs;
  std::set<std::string> selectedArchiveNames;
  llvm::SmallVector<llvm::StringRef> lines;
  llvm::StringRef(*traceText).split(lines, '\n', -1, false);
  for (auto line : lines) {
    if (line.empty()) continue;
    fs::path path;
    std::string contents;
    const auto open = line.rfind('(');
    const bool member = line.ends_with(")") && open != llvm::StringRef::npos;
    if (member) {
      path = absolutePath(line.take_front(open).str());
      if (inside(path, absolutePath(sdk.root))) continue;
      if (!inside(path, lane)) { unsupported.push_back("archive input outside private build"); continue; }
      selectedArchiveNames.insert(line.str());
      if (markerPosition == selectedJournals.size()) {
        unsupported.push_back("archive extraction exceeds final native capture witness"); continue;
      }
      auto value = selectedArchiveMember(path, line.slice(open + 1, line.size() - 1), lane,
                                        selectedJournals[markerPosition]);
      if (!value) { unsupported.push_back(llvm::toString(value.takeError())); continue; }
      contents = std::move(*value);
    } else {
      path = absolutePath(line.str());
      if (inside(path, absolutePath(sdk.root))) continue;
      if (!inside(path, lane)) { unsupported.push_back("native input outside private build: " + line.str()); continue; }
      auto value = read(path);
      if (!value) { unsupported.push_back(llvm::toString(value.takeError())); continue; }
      contents = std::move(*value);
    }
    auto object = llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(contents, line));
    if (!object) { unsupported.push_back(llvm::toString(object.takeError())); continue; }
    auto *elf = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(object->get());
    if (!elf) { unsupported.push_back("non-ELF native input"); continue; }
    if (!member && elf->getEType() == llvm::ELF::ET_DYN) {
      auto info = dynamicInfo(**object);
      if (!info) { unsupported.push_back(llvm::toString(info.takeError())); continue; }
      auto dependency = readJson(metadataPath(metadata, path, "link"));
      if (!dependency) { unsupported.push_back("DSO lacks private build provenance: " + line.str()); llvm::consumeError(dependency.takeError()); continue; }
      auto *record = dependency->getAsObject();
      if (!record || record->getString("native_sha256") != digest(contents) ||
          record->getString("kind") != "shared" || info->soname.empty()) {
        unsupported.push_back("DSO identity/provenance mismatch: " + line.str()); continue;
      }
      auto errors = strings(*record, "unsupported");
      if (!errors) return errors.takeError();
      if (!errors->empty()) { unsupported.push_back("unqualified DSO dependency: " + errors->front()); continue; }
      auto inserted = privateDSOs.emplace(info->soname, path);
      if (!inserted.second && inserted.first->second != path)
        unsupported.push_back("conflicting native DSO SONAME: " + info->soname);
      continue;
    }
    if (elf->getEType() != llvm::ELF::ET_REL) { unsupported.push_back("non-relocatable ordinary link input"); continue; }
    // The final ELF's align-1 marker section must correspond exactly to the
    // private relocatable input trace after native SDK objects/DSOs are omitted.
    // This disambiguates equal archive basenames using actual selected journal
    // identities; no assumption about symbol tables or archive naming is needed.
    auto markers = captureMarkers(**object, line);
    if (!markers) { unsupported.push_back(llvm::toString(markers.takeError())); continue; }
    if (markers->size() != 1 || markerPosition >= selectedJournals.size() ||
        markers->front() != selectedJournals[markerPosition]) {
      unsupported.push_back("native trace/final capture witness order mismatch: " + line.str()); continue;
    }
    ++markerPosition;
    auto journal = consumedObject(contents, line, metadata);
    if (!journal) { unsupported.push_back(llvm::toString(journal.takeError())); continue; }
    units.push_back(std::move(*journal));
  }
  if (markerPosition != selectedJournals.size())
    unsupported.push_back("final native capture witness contains unmatched input journals");
  lines.clear();
  llvm::StringRef(*extraction).split(lines, '\n', -1, false);
  for (size_t i = 1; i < lines.size(); ++i) {
    llvm::SmallVector<llvm::StringRef> fields;
    lines[i].split(fields, '\t');
    if (fields.size() != 3) return fail("malformed LLD archive extraction evidence");
    auto open = fields[1].rfind('(');
    if (open == llvm::StringRef::npos) return fail("invalid extracted member identity");
    if (inside(absolutePath(fields[1].take_front(open).str()), absolutePath(sdk.root))) continue;
    if (!selectedArchiveNames.count(fields[1].str())) return fail("LLD archive extraction/trace mismatch");
  }
  auto *outputELF = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(outputObject->get());
  if (!outputELF || outputELF->getEType() != llvm::ELF::ET_DYN)
    unsupported.push_back("selected native output is not the qualified PIE/shared ELF kind");
  auto dynamic = dynamicInfo(**outputObject);
  if (!dynamic) return dynamic.takeError();
  for (const auto &needed : dynamic->needed) {
    if (privateDSOs.count(needed)) libraries.push_back(":" + needed);
    else if (needed == "libc.so.6") continue;
    else if (needed == "libm.so.6") libraries.push_back("m");
    else unsupported.push_back("native dependency is not a captured/managed DSO: " + needed);
  }
  return atomicJson(metadataPath(metadata, output, "link"), llvm::json::Object{
    {"native_sha256", digest(*bytes)}, {"kind", kind}, {"units", std::move(units)},
    {"libraries", std::move(libraries)}, {"link_options", std::move(linkOptions)},
    {"version_script", versionScript}, {"unsupported", std::move(unsupported)}});
}
struct SelectedUnit {
  fs::path capture;
  std::string source, key, flags, optimization, archiveMemberName;
};
struct SelectedBuild {
  std::vector<SelectedUnit> units;
  std::vector<std::string> libraries, linkOptions;
  std::string kind, versionScript;
};
llvm::Expected<SelectedBuild> selectArchive(const fs::path &metadata,
    const fs::path &output, llvm::StringRef bytes, bool retained = false) {
  const std::string archiveName = output.string();
  auto archive = llvm::object::Archive::create(llvm::MemoryBufferRef(bytes, archiveName));
  if (!archive) return archive.takeError();
  SelectedBuild result;
  result.kind = "static";
  llvm::Error iteration = llvm::Error::success();
  // A static output preserves physical member order and every occurrence,
  // including equal basenames. It is not a link/extraction of those members.
  for (const auto &child : (*archive)->children(iteration)) {
    auto name = child.getName();
    if (!name) return name.takeError();
    if (name->empty() || name->size() > 128 || name->contains('/') ||
        name->contains('\\') || name->contains('\0') || *name == "." || *name == "..")
      return fail("static archive member needs a portable basename: " + name->str());
    if ((*archive)->isThin()) {
      auto memberPath = child.getFullName();
      if (!memberPath) return memberPath.takeError();
      if (!inside(absolutePath(*memberPath), metadata.parent_path()))
        return fail("thin static output member escapes private build");
    }
    auto contents = child.getBuffer();
    if (!contents) return contents.takeError();
    if (contents->size() > 64 * 1024 * 1024)
      return fail("static archive member exceeds capture limit");
    auto journal = consumedObject(*contents, *name, metadata, retained);
    if (!journal) return journal.takeError();
    auto *item = journal->getAsObject();
    if (!item->getString("key")) return fail("static member lacks native compile role");
    result.units.push_back({item->getString("capture")->str(),
      item->getString("source")->str(), item->getString("key")->str(),
      jsonText(*item->get("flags")), item->getString("optimization")->str(), name->str()});
  }
  if (iteration) return std::move(iteration);
  return result;
}
llvm::Expected<SelectedBuild> selectOutput(const fs::path &metadata, const fs::path &output,
                                         bool retained = false) {
  if (!inside(absolutePath(output), metadata.parent_path()))
    return fail("selected output escaped private build lane");
  auto selectedBytes = read(output);
  if (!selectedBytes) return selectedBytes.takeError();
  if (llvm::StringRef(*selectedBytes).starts_with("!<arch>\n") ||
      llvm::StringRef(*selectedBytes).starts_with("!<thin>\n"))
    return selectArchive(metadata, output, *selectedBytes, retained);
  auto link = readJson(metadataPath(metadata, output, "link"));
  if (!link) return fail("selected output has no native link journal: " + output.string() +
                         ": " + llvm::toString(link.takeError()));
  auto *record = link->getAsObject();
  if (!record || !record->getString("native_sha256") || !record->getString("kind") ||
      !record->getArray("units")) return fail("invalid private native link journal");
  auto bytes = read(output);
  if (!bytes) return bytes.takeError();
  if (digest(*bytes) != *record->getString("native_sha256")) return fail("selected output changed after capture");
  auto unsupported = strings(*record, "unsupported");
  if (!unsupported) return unsupported.takeError();
  if (!unsupported->empty()) return fail("selected link is not yet qualified: " + unsupported->front());
  auto libraries = strings(*record, "libraries");
  if (!libraries) return libraries.takeError();
  auto options = strings(*record, "link_options");
  if (!options) return options.takeError();
  SelectedBuild result;
  result.kind = record->getString("kind")->str();
  if (auto script = record->getString("version_script")) result.versionScript = script->str();
  result.libraries = std::move(*libraries); result.linkOptions = std::move(*options);
  std::vector<std::string> retainedWitness;
  if (retained) {
    if (result.kind != "executable" && result.kind != "shared")
      return fail("unqualified retained native output kind");
    const std::string outputName = output.string();
    auto object = llvm::object::ObjectFile::createObjectFile(
        llvm::MemoryBufferRef(*selectedBytes, outputName));
    if (!object) return object.takeError();
    auto *elf = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(object->get());
    if (!elf || elf->getEType() != llvm::ELF::ET_DYN)
      return fail("retained native output is not a qualified PIE/shared ELF");
    auto markers = captureMarkers(**object, outputName);
    if (!markers) return markers.takeError();
    retainedWitness = std::move(*markers);
    if (retainedWitness.size() != record->getArray("units")->size())
      return fail("retained native output witness and unit inventory differ");
    std::set<std::string> distinct(retainedWitness.begin(), retainedWitness.end());
    if (distinct.size() != retainedWitness.size())
      return fail("retained native output has ambiguous repeated journal witnesses");
  }
  size_t retainedPosition = 0;
  for (auto &unit : *record->getArray("units")) {
    auto *item = unit.getAsObject();
    if (!item || !item->getString("capture") || !item->getString("source") || !item->getString("key") ||
        !item->getString("optimization") || !item->getArray("flags") ||
        !item->getString("capture_sha256") || !item->getString("native_object") ||
        !item->getString("native_sha256")) return fail("invalid consumed compile journal");
    if (retained &&
        (!inside(absolutePath(item->getString("capture")->str()), metadata) ||
         !inside(absolutePath(item->getString("native_object")->str()), metadata)))
      return fail("retained native/capture input escaped metadata");
    auto capture = read(item->getString("capture")->str());
    if (!capture) return capture.takeError();
    auto native = read(item->getString("native_object")->str());
    if (!native) return native.takeError();
    if (digest(*capture) != *item->getString("capture_sha256") ||
        digest(*native) != *item->getString("native_sha256"))
      return fail("private native/capture input changed after link");
    if (retained) {
      const auto nativePath = absolutePath(item->getString("native_object")->str());
      if (!inside(nativePath, metadata))
        return fail("retained original native object escaped metadata");
      const std::string nativeName = nativePath.string();
      auto nativeObject = llvm::object::ObjectFile::createObjectFile(
          llvm::MemoryBufferRef(*native, nativeName));
      if (!nativeObject) return nativeObject.takeError();
      auto markers = captureMarkers(**nativeObject, nativeName);
      if (!markers) return markers.takeError();
      if (markers->size() != 1 || markers->front() != retainedWitness[retainedPosition++])
        return fail("retained unit order differs from the final native capture witness");
      auto immutable = consumedObject(*native, nativeName, metadata, true);
      if (!immutable) return immutable.takeError();
      if (*immutable != unit)
        return fail("retained consumed unit differs from its immutable compile journal");
    }
    result.units.push_back({item->getString("capture")->str(), item->getString("source")->str(), item->getString("key")->str(),
      jsonText(*item->get("flags")), item->getString("optimization")->str()});
  }
  if (result.units.empty()) return fail("selected link contains no captured application units");
  return result;
}
llvm::Expected<CapturedBuild> pairSelected(const std::vector<SelectedBuild> &selected) {
  if (selected.size() != 2) return fail("paired native selection requires both profiles");
  if (selected[0].libraries != selected[1].libraries || selected[0].linkOptions != selected[1].linkOptions ||
      selected[0].kind != selected[1].kind || selected[0].versionScript != selected[1].versionScript)
    return fail("profile-dependent link graph needs a still-unimplemented common contract");
  CapturedBuild result;
  result.kind = selected[0].kind; result.libraries = selected[0].libraries; result.linkOptions = selected[0].linkOptions;
  result.versionScript = selected[0].versionScript;
  bool corresponding = selected[0].units.size() == selected[1].units.size();
  if (corresponding)
    for (size_t i = 0; i < selected[0].units.size(); ++i) {
      const auto &left = selected[0].units[i], &right = selected[1].units[i];
      corresponding &= (left.source == right.source || left.key == right.key) &&
          left.flags == right.flags && left.optimization == right.optimization &&
          left.archiveMemberName == right.archiveMemberName;
    }
  if (!corresponding) {
    // Native archive extraction can visit the same selected units in a
    // different order on each target. Prove a unique correspondence using
    // source/compile role AND exact effective settings, then retain each
    // profile's observed order independently. No all-TU flag equality is
    // needed when there is no repartitioning of any native optimization unit.
    if (result.kind != "static" && selected[0].units.size() == selected[1].units.size()) {
      std::vector<size_t> peers;
      std::vector<bool> used(selected[1].units.size());
      for (const auto &left : selected[0].units) {
        std::vector<size_t> candidates;
        for (size_t j = 0; j < selected[1].units.size(); ++j) {
          const auto &right = selected[1].units[j];
          if ((left.source == right.source || left.key == right.key) &&
              left.flags == right.flags && left.optimization == right.optimization &&
              left.archiveMemberName == right.archiveMemberName) candidates.push_back(j);
        }
        if (candidates.size() != 1 || used[candidates.front()]) break;
        used[candidates.front()] = true;
        peers.push_back(candidates.front());
      }
      if (peers.size() == selected[0].units.size()) {
        result.i686Order.resize(peers.size());
        for (size_t i = 0; i < peers.size(); ++i) {
          const auto &left = selected[0].units[i], &right = selected[1].units[peers[i]];
          result.units.push_back({{left.capture}, {right.capture}, left.optimization, left.archiveMemberName});
          result.i686Order[peers[i]] = i;
        }
        return result;
      }
    }
    if (result.kind == "static")
      return fail("differing static archive inventories need per-target member partition publication");
    if (selected[0].units.empty() || selected[1].units.empty())
      return fail("empty profile-selected application inventory");
    const auto &settings = selected[0].units.front();
    CapturedUnit group;
    group.optimization = settings.optimization;
    for (size_t profile = 0; profile < selected.size(); ++profile)
      for (const auto &unit : selected[profile].units) {
        if (unit.flags != settings.flags || unit.optimization != settings.optimization)
          return fail("differing TU partitions currently require matching effective per-unit settings");
        (profile ? group.i686Paths : group.x64Paths).push_back(unit.capture);
      }
    // The shared producer determines definition correspondence and proves exact
    // reconstruction. Build paths and equal settings alone do not establish it.
    result.units.push_back(std::move(group));
    return result;
  }
  for (size_t i = 0; i < selected[0].units.size(); ++i) {
    const auto &left = selected[0].units[i], &right = selected[1].units[i];
    // Source selection can differ by native profile while serving one explicit
    // object/link role. The merger still has to prove the two LLVM contracts;
    // the matching path is evidence for correspondence, not a correctness proof.
    if ((left.source != right.source && left.key != right.key) ||
        left.flags != right.flags || left.optimization != right.optimization ||
        left.archiveMemberName != right.archiveMemberName)
      return fail("profile source/settings/order divergence cannot be silently paired");
    result.units.push_back({{left.capture}, {right.capture}, left.optimization, left.archiveMemberName});
  }
  return result;
}
} // namespace

int nativeLinkMain(int argc, char **argv) {
  try {
    const char *sdkRoot = std::getenv("NIER_BUILD_SDK");
    const char *laneName = std::getenv("NIER_BUILD_LANE");
    const char *metadataName = std::getenv("NIER_BUILD_METADATA");
    if (!sdkRoot || !laneName || !metadataName) {
      llvm::errs() << "nier-native-ld: missing private publisher build environment\n"; return 1;
    }
    Sdk sdk{sdkRoot};
    auto args = expandResponses(std::vector<std::string>(argv + 1, argv + argc));
    if (!args) { llvm::logAllUnhandledErrors(args.takeError(), llvm::errs()); return 1; }
    fs::path output = "a.out";
    for (size_t i = 0; i < args->size(); ++i) {
      llvm::StringRef arg((*args)[i]);
      if (arg == "-o" && i + 1 < args->size()) output = (*args)[++i];
      else if (arg.starts_with("-o") && arg.size() > 2) output = arg.drop_front(2).str();
    }
    const bool query = std::find(args->begin(), args->end(), "--version") != args->end() ||
                       std::find(args->begin(), args->end(), "--help") != args->end();
    if (!query && absolutePath(output) != "/dev/null" && !inside(absolutePath(output), absolutePath(laneName))) {
      llvm::errs() << "nier-native-ld: output escaped private build lane\n"; return 1;
    }
    auto command = *args; command.insert(command.begin(), sdk.tool("ld.lld").string());
    if (query) {
      if (auto error = run(command)) { llvm::logAllUnhandledErrors(std::move(error), llvm::errs()); return 1; }
      return 0;
    }
    const auto metadata = absolutePath(metadataName);
    llvm::SmallString<256> traceName;
    int traceDescriptor;
    if (auto error = llvm::sys::fs::createUniqueFile(
          (metadata / (digest(absolutePath(output).string()) + ".%%%%%%.trace")).string(),
          traceDescriptor, traceName, llvm::sys::fs::OF_None, 0600)) {
      llvm::errs() << "nier-native-ld: cannot reserve trace: " << error.message() << '\n'; return 1;
    }
    const fs::path trace(traceName.str().str());
    const auto whyExtract = trace.string() + ".extract.tsv";
    command.push_back("--trace");
    command.push_back("--why-extract=" + whyExtract);
    if (auto error = runTracedLink(command, traceDescriptor)) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "nier-native-ld: "); return 1;
    }
    if (std::find(args->begin(), args->end(), "--trace") != args->end() ||
        std::find(args->begin(), args->end(), "-t") != args->end()) {
      auto log = read(trace);
      if (!log) { llvm::logAllUnhandledErrors(log.takeError(), llvm::errs()); return 1; }
      llvm::outs() << *log;
    }
    if (auto error = recordNativeLink(*args, sdk, absolutePath(laneName), metadata, trace, whyExtract)) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "nier-native-ld: "); return 1;
    }
    return 0;
  } catch (const std::exception &error) { llvm::errs() << "nier-native-ld: " << error.what() << '\n'; return 1; }
}

llvm::Expected<CapturedBuild> selectRetainedBuild(const fs::path &scratch,
    const fs::path &laneRelativeOutput) {
  const auto relative = laneRelativeOutput.lexically_normal();
  if (relative.empty() || relative == "." || relative.is_absolute() ||
      *relative.begin() == "..")
    return fail("retained selected output must stay inside each private build lane");
  const auto root = absolutePath(scratch);
  if (!fs::is_directory(root)) return fail("retained private build root does not exist");
  std::vector<SelectedBuild> selected;
  for (const char *profile : {"x86_64", "i686"}) {
    const auto lane = absolutePath(root / (std::string("build-") + profile));
    const auto metadata = absolutePath(lane / "metadata");
    const auto output = absolutePath(lane / relative);
    if (!inside(lane, root) || !inside(metadata, lane) || !inside(output, lane) ||
        !fs::is_directory(lane) || !fs::is_directory(metadata))
      return fail("retained selection escaped or lacks a private build lane");
    auto result = selectOutput(metadata, output, true);
    if (!result) return result.takeError();
    selected.push_back(std::move(*result));
  }
  return pairSelected(selected);
}

llvm::Expected<CapturedBuild> captureBuild(const BuildRequest &request,
    const Sdk &sdk, const fs::path &scratch) {
  if (request.system != "make" && request.system != "cmake") return fail("build system must be make or cmake");
  const auto relativeOutput = request.output.lexically_normal();
  if (relativeOutput.empty() || relativeOutput.is_absolute() || *relativeOutput.begin() == "..")
    return fail("selected output must stay inside the private build tree");
  const auto source = absolutePath(request.sourceDirectory);
  if (!fs::is_directory(source)) return fail("build source directory does not exist");
  if (inside(absolutePath(scratch), source)) return fail("scratch cannot be inside copied sources");
  for (const auto &target : request.targets)
    if (target.empty() || target.front() == '-') return fail("build target must be a name, not an option");
  const auto recorder = fs::read_symlink("/proc/self/exe").parent_path() / "nier-native-ld";
  if (!fs::is_regular_file(recorder)) return fail("publisher SDK lacks nier-native-ld");
  std::vector<SelectedBuild> selected;
  for (const char *profile : {"x86_64", "i686"}) {
    const bool x64 = std::string(profile) == "x86_64";
    const auto lane = absolutePath(scratch) / (std::string("build-") + profile);
    if (fs::exists(lane)) return fail("private native build lane already exists");
    fs::create_directories(lane / "metadata"); fs::create_directories(lane / "tmp");
    if (auto error = copySource(source, lane / "source", source)) return error;
    const auto library = sdk.sysroot(profile) / "usr/lib" / (x64 ? "x86_64-linux-gnu" : "i386-linux-gnu");
    std::vector<std::string> config = sdk.compileFlags(profile);
    config.insert(config.end(), {"--rtlib=compiler-rt", "--unwindlib=none", "-fPIC", "-g", "-fno-temp-file",
      // Native references and captures share the same virtual source identity.
      // Only our transient lane prefix changes; physical provenance paths do not.
      "-ffile-prefix-map=" + lane.string() + "=/nier",
      "-fstandalone-debug", std::string("-fplugin=") + NIER_CLANG_PLUGIN,
      std::string("-fpass-plugin=") + NIER_CAPTURE_PLUGIN, "--ld-path=" + recorder.string(),
      // GNU-ld-compatible version scripts may name symbols absent from a
      // particular link (including configure probes). Preserve that declared
      // native SDK default in the artifact; never patch the project script.
      "-Wl,--undefined-version,--strip-all,--build-id,--eh-frame-hdr,--hash-style=gnu,--enable-new-dtags,-z,relro,-z,now",
      "-Wl,--dynamic-linker," + (library / (x64 ? "ld-linux-x86-64.so.2" : "ld-linux.so.2")).string(),
      "-Wl,-rpath," + library.string(), "-Wl,-z,nodefaultlib"});
    config.insert(config.end(), request.cflags.begin(), request.cflags.end());
    std::string configText;
    for (const auto &argument : config) configText += quoteConfig(argument);
    const auto configPath = lane / "native.cfg";
    if (auto error = write(configPath, configText)) return error;
    const auto compiler = sdk.tool("clang").string();
    const auto compilerCommand = shellQuote(compiler) + " " + shellQuote("--config=" + configPath.string());
    const std::string inheritedPath = std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin";
    const std::string inheritedLibraries = std::getenv("LD_LIBRARY_PATH") ? std::getenv("LD_LIBRARY_PATH") : "";
    std::map<std::string, std::string> environment{
      {"NIER_BUILD_SDK", sdk.root.string()}, {"NIER_BUILD_PROFILE", profile},
      {"NIER_BUILD_LANE", lane.string()}, {"NIER_BUILD_METADATA", (lane / "metadata").string()},
      {"CC", compilerCommand}, {"AR", sdk.tool("llvm-ar").string()},
      {"RANLIB", sdk.tool("llvm-ranlib").string()}, {"LD", recorder.string()},
      {"CCACHE_DISABLE", "1"}, {"TMPDIR", (lane / "tmp").string()},
      {"PATH", (sdk.root / "host/usr/bin").string() + ":" + inheritedPath},
      {"LD_LIBRARY_PATH", (sdk.root / "host/usr/lib/llvm-18/lib").string() + ":" +
        (sdk.root / "host/usr/lib/x86_64-linux-gnu").string() +
        (inheritedLibraries.empty() ? "" : ":" + inheritedLibraries)}};
    fs::path outputDirectory;
    if (request.system == "cmake") {
      for (const auto &argument : request.configureArgs) {
        llvm::StringRef arg(argument);
        if (arg.starts_with("-S") || arg.starts_with("-B") || arg.starts_with("-G") ||
            arg.starts_with("--preset") || arg.starts_with("--toolchain") ||
            arg.starts_with("-DCMAKE_C_COMPILER") || arg.starts_with("-DCMAKE_TOOLCHAIN_FILE") ||
            arg.starts_with("-DCMAKE_AR") || arg.starts_with("-DCMAKE_RANLIB") ||
            arg.starts_with("-DCMAKE_MAKE_PROGRAM"))
          return fail("configure argument overrides private SDK integration: " + argument);
      }
      outputDirectory = lane / "build";
      std::vector<std::string> command{(sdk.root / "host/usr/bin/cmake").string(),
        "-S", (lane / "source").string(), "-B", outputDirectory.string(), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=" + compiler,
        "-DCMAKE_C_COMPILER_ARG1=--config=" + configPath.string(),
        "-DCMAKE_AR=" + sdk.tool("llvm-ar").string(), "-DCMAKE_RANLIB=" + sdk.tool("llvm-ranlib").string(),
        "-DCMAKE_MAKE_PROGRAM=" + (sdk.root / "host/usr/bin/ninja").string()};
      command.insert(command.end(), request.configureArgs.begin(), request.configureArgs.end());
      if (auto error = run(command, lane, environment)) return error;
      command = {(sdk.root / "host/usr/bin/cmake").string(), "--build", outputDirectory.string(), "--parallel", "2"};
      if (!request.targets.empty()) {
        command.push_back("--target"); command.insert(command.end(), request.targets.begin(), request.targets.end());
      }
      if (auto error = run(command, lane, environment)) return error;
    } else {
      outputDirectory = lane / "source";
      const auto configure = outputDirectory / "configure";
      if (fs::is_regular_file(configure)) {
        // Traditional configure scripts expand $CC directly rather than eval
        // shell quoting embedded inside it. Their supported CC convention is
        // a whitespace-separated executable and arguments. Make recipe quoting
        // remains separate below; never let a configure probe fall back to cc.
        if (compiler.find_first_of(" \t\r\n") != std::string::npos ||
            configPath.string().find_first_of(" \t\r\n") != std::string::npos)
          return fail("this configure CC interface requires SDK/scratch paths without whitespace");
        environment["CC"] = compiler + " --config=" + configPath.string();
        std::vector<std::string> command{configure.string()};
        command.insert(command.end(), request.configureArgs.begin(), request.configureArgs.end());
        if (auto error = run(command, outputDirectory, environment)) return error;
      } else if (!request.configureArgs.empty()) return fail("configure arguments supplied without a configure script");
      std::vector<std::string> command{"make", "-B", "-j2", "CC=" + compilerCommand,
        "AR=" + sdk.tool("llvm-ar").string(), "RANLIB=" + sdk.tool("llvm-ranlib").string(), "LD=" + recorder.string()};
      command.insert(command.end(), request.targets.begin(), request.targets.end());
      if (auto error = run(command, outputDirectory, environment)) return error;
    }
    auto result = selectOutput(lane / "metadata", absolutePath(outputDirectory / relativeOutput));
    if (!result) return result.takeError();
    selected.push_back(std::move(*result));
  }
  return pairSelected(selected);
}
} // namespace nier::driver
