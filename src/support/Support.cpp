#include "sela/Support.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sela::driver {
llvm::Error fail(const std::string &message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), "%s", message.c_str());
}
llvm::Expected<std::string> read(const fs::path &path, size_t limit) {
  // Ordinary symlinks are needed for SDK inputs. Validate the opened target,
  // not the pathname, and never block opening a FIFO before rejecting it.
  int fd;
  do { fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC); }
  while (fd < 0 && errno == EINTR);
  if (fd < 0) return fail("cannot open " + path.string() + ": " + std::strerror(errno));
  struct Descriptor {
    int fd;
    ~Descriptor() { close(fd); }
  } descriptor{fd};
  auto status = [&]() -> llvm::Expected<struct stat> {
    struct stat info;
    int result;
    do { result = fstat(fd, &info); } while (result < 0 && errno == EINTR);
    if (result < 0) return fail("cannot stat opened input " + path.string() + ": " + std::strerror(errno));
    return info;
  };
  auto before = status();
  if (!before) return before.takeError();
  if (!S_ISREG(before->st_mode)) return fail("input is not a regular file: " + path.string());
  if (before->st_size < 0 || static_cast<uintmax_t>(before->st_size) > limit)
    return fail("cannot read bounded input " + path.string() + ": size limit exceeded");
  std::string result(static_cast<size_t>(before->st_size), '\0');
  size_t offset = 0;
  while (offset < result.size()) {
    // A fixed read chunk avoids platform ssize_t limits even for callers
    // choosing a larger cap than the product's ordinary 64 MiB limit.
    size_t remaining = result.size() - offset;
    size_t chunk = remaining > 1024 * 1024 ? 1024 * 1024 : remaining;
    ssize_t count = ::read(fd, result.data() + offset, chunk);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return fail("cannot read " + path.string() + ": " + std::strerror(errno));
    if (!count) return fail("input truncated while reading: " + path.string());
    offset += static_cast<size_t>(count);
  }
  char extra;
  ssize_t count;
  do { count = ::read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
  if (count < 0) return fail("cannot check input end " + path.string() + ": " + std::strerror(errno));
  if (count) return fail("input grew while reading: " + path.string());
  auto after = status();
  if (!after) return after.takeError();
  if (after->st_size != before->st_size)
    return fail("input size changed while reading: " + path.string());
  return result;
}
llvm::Error write(const fs::path &path, llvm::StringRef contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), contents.size());
  stream.close();
  if (!stream) return fail("cannot write " + path.string());
  return llvm::Error::success();
}
llvm::Error replaceFile(const fs::path &source, const fs::path &output, bool executable) {
  std::error_code ec;
  auto parent = fs::absolute(output).parent_path();
  if (!fs::is_directory(parent)) return fail("output parent is not a directory: " + parent.string());
  auto status = fs::symlink_status(output, ec);
  if (ec && ec != std::errc::no_such_file_or_directory)
    return fail("cannot inspect output: " + output.string() + ": " + ec.message());
  if (fs::exists(status) && !fs::is_regular_file(status))
    return fail("output must be a regular file, not a directory, symlink or special file: " + output.string());
  ec.clear();
  std::string temporary = (parent / ".sela-output-XXXXXX").string();
  int fd = mkstemp(temporary.data());
  if (fd < 0) return fail("cannot create output staging file");
  close(fd);
  fs::copy_file(source, temporary, fs::copy_options::overwrite_existing, ec);
  if (!ec && chmod(temporary.c_str(), executable ? 0755 : 0644) != 0)
    ec = std::error_code(errno, std::generic_category());
  if (!ec) fs::rename(temporary, output, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return fail("cannot install output " + output.string() + ": " + ec.message());
  }
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
  std::string pattern = (fs::temp_directory_path() / "sela-private-XXXXXX").string();
  if (!mkdtemp(pattern.data())) return fail("cannot make private work directory");
  Scratch result;
  result.path = pattern;
  return std::move(result);
}
}
