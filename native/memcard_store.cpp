#include "memcard_store.hpp"

#include <cstdio>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gt4::memcard {
namespace {

// Forces the bytes of `file` to stable storage before the rename.
bool flush_to_disk(std::FILE *file) {
  if (std::fflush(file) != 0) return false;
#ifdef _WIN32
  return _commit(_fileno(file)) == 0;
#else
  return fsync(fileno(file)) == 0;
#endif
}

// Opens with a wide path on Windows so non-ASCII user names keep working.
std::FILE *open_file(const std::filesystem::path &path, bool writing) {
#ifdef _WIN32
  std::FILE *file = nullptr;
  if (_wfopen_s(&file, path.c_str(), writing ? L"wb" : L"rb") != 0) return nullptr;
  return file;
#else
  return std::fopen(path.c_str(), writing ? "wb" : "rb");
#endif
}

bool write_bytes(std::FILE *file, const std::vector<std::uint8_t> &bytes, std::size_t count) {
  return std::fwrite(bytes.data(), 1, count, file) == count;
}

} // namespace

bool write_card_atomically(const std::filesystem::path &path,
                           const std::vector<std::uint8_t> &bytes,
                           CrashPoint crash) {
  std::filesystem::path temporary = path;
  temporary += ".tmp";

  std::FILE *file = open_file(temporary, true);
  if (!file) return false;

  bool ok = true;
  if (crash == CrashPoint::after_partial_write) {
    write_bytes(file, bytes, bytes.size() / 2);
    std::fclose(file);
    return false;  // simulated power loss: the temporary file is incomplete
  }
  ok = write_bytes(file, bytes, bytes.size()) && flush_to_disk(file);
  ok = (std::fclose(file) == 0) && ok;
  if (!ok) return false;

  if (crash == CrashPoint::before_rename) return false;  // simulated crash before the swap

  std::error_code error;
  std::filesystem::rename(temporary, path, error);  // MoveFileExW(REPLACE_EXISTING) on Windows
#ifndef _WIN32
  // TASK-180 (Android plan section 5): the rename itself must reach storage too. Android may kill the game right after
  // a save; without a sync of the folder the card's directory entry can still name the old file after a power loss.
  if (!error) {
    const std::filesystem::path folder = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const int directory = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return false;
    const bool synced = ::fsync(directory) == 0;
    ::close(directory);
    return synced;
  }
#endif
  return !error;
}

bool read_card_file(const std::filesystem::path &path, std::vector<std::uint8_t> &bytes) {
  std::FILE *file = open_file(path, false);
  if (!file) return false;
  bytes.clear();
  std::uint8_t chunk[1u << 16];
  std::size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0) bytes.insert(bytes.end(), chunk, chunk + got);
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok;
}

} // namespace gt4::memcard
