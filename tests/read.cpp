#include "nier/Support.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace nier::driver;

namespace {
void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
void save(const fs::path &path, llvm::StringRef bytes) {
  if (auto error = write(path, bytes))
    throw std::runtime_error(llvm::toString(std::move(error)));
}
void accepts(const fs::path &path, size_t limit, llvm::StringRef expected) {
  auto bytes = read(path, limit);
  if (!bytes) throw std::runtime_error(llvm::toString(bytes.takeError()));
  require(*bytes == expected, "incorrect bytes from " + path.string());
}
void rejects(const fs::path &path, size_t limit,
             llvm::StringRef diagnostic = {}) {
  auto bytes = read(path, limit);
  require(!bytes, "unexpectedly accepted " + path.string());
  auto message = llvm::toString(bytes.takeError());
  require(llvm::StringRef(message).contains(diagnostic),
          "unexpected rejection for " + path.string() + ": " + message);
}
}

int main() {
  try {
    // A reader that mistakenly blocks opening a FIFO fails promptly instead
    // of hanging the test runner; there is intentionally no FIFO writer.
    alarm(5);
    auto scratch = Scratch::create();
    if (!scratch) throw std::runtime_error(llvm::toString(scratch.takeError()));
    auto directory = scratch->path;
    const std::string binary("Nier\0code", 9);
    save(directory / "bytes", binary);
    save(directory / "empty", "");
    accepts(directory / "bytes", binary.size(), binary);
    accepts(directory / "empty", 0, "");
    rejects(directory / "bytes", binary.size() - 1, "size limit");
    rejects(directory / "bytes", 0, "size limit");
    rejects(directory / "missing", 64);
    rejects(directory, 64, "regular file");

    // Bounds are checked before allocation, including sparse huge files.
    save(directory / "oversized", "");
    fs::resize_file(directory / "oversized", 64ULL * 1024 * 1024 + 1);
    rejects(directory / "oversized", 64ULL * 1024 * 1024, "size limit");

    fs::create_symlink("bytes", directory / "regular-link");
    accepts(directory / "regular-link", binary.size(), binary);
    fs::create_symlink("missing", directory / "broken-link");
    rejects(directory / "broken-link", 64);
    require(mkfifo((directory / "fifo").c_str(), 0600) == 0,
            "could not create FIFO fixture");
    rejects(directory / "fifo", 64, "regular file");
    fs::create_symlink("fifo", directory / "fifo-link");
    rejects(directory / "fifo-link", 64, "regular file");
    if (fs::exists("/dev/zero")) rejects("/dev/zero", 64, "regular file");
    alarm(0);
    llvm::outs() << "Bounded regular-file reads, symlinks, caps, and nonblocking FIFO rejection passed.\n";
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "read validation: " << error.what() << '\n';
    return 1;
  }
}
