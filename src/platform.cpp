// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "detail/platform.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rep::detail {
namespace {

std::atomic<std::uint64_t> g_token_counter{0};

} // namespace

FileHandle::~FileHandle() { reset(); }

FileHandle::FileHandle(FileHandle&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    reset();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

#if defined(_WIN32)

namespace {

[[nodiscard]] std::wstring to_wide(const std::filesystem::path& path) {
  return path.wstring();
}

[[nodiscard]] Error win32_failure(std::string_view what, unsigned long code) {
  return make_error(ErrorCode::IoError, std::string(what) + " failed: " + last_system_error(),
                    std::to_string(code));
}

[[nodiscard]] bool is_valid(HANDLE handle) noexcept {
  return handle != INVALID_HANDLE_VALUE && handle != nullptr;
}

} // namespace

void FileHandle::reset() noexcept {
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}

void* FileHandle::release() noexcept {
  void* handle = handle_;
  handle_ = nullptr;
  return handle;
}

std::string last_system_error() {
  const unsigned long code = ::GetLastError();
  if (code == 0) {
    return "no error";
  }
  LPWSTR buffer = nullptr;
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer),
      0, nullptr);
  std::string message;
  if (length != 0 && buffer != nullptr) {
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr,
                                             0, nullptr, nullptr);
    if (needed > 0) {
      message.resize(static_cast<std::size_t>(needed));
      ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), message.data(), needed,
                            nullptr, nullptr);
    }
    ::LocalFree(buffer);
  }
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
    message.pop_back();
  }
  if (message.empty()) {
    message = "error " + std::to_string(code);
  }
  return message;
}

std::string process_token() {
  const auto counter = g_token_counter.fetch_add(1, std::memory_order_relaxed);
  return std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(counter);
}

Result<bool> path_exists(const std::filesystem::path& path) {
  const std::wstring wide = to_wide(path);
  const DWORD attributes = ::GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return false;
    }
    return win32_failure("GetFileAttributesW", code);
  }
  return true;
}

Result<bool> is_directory(const std::filesystem::path& path) {
  const std::wstring wide = to_wide(path);
  const DWORD attributes = ::GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return false;
    }
    return win32_failure("GetFileAttributesW", code);
  }
  return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

Result<bool> is_reparse_point(const std::filesystem::path& path) {
  const std::wstring wide = to_wide(path);
  const DWORD attributes = ::GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return false;
    }
    return win32_failure("GetFileAttributesW", code);
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

Result<void> ensure_directory(const std::filesystem::path& directory) {
  const std::wstring wide = to_wide(directory);
  if (::CreateDirectoryW(wide.c_str(), nullptr) != 0) {
    return {};
  }
  const DWORD code = ::GetLastError();
  if (code == ERROR_ALREADY_EXISTS) {
    const auto directory_check = rep::detail::is_directory(directory);
    if (!directory_check.ok()) {
      return directory_check.error();
    }
    if (directory_check.value()) {
      return {};
    }
    return make_error(ErrorCode::NotADirectory,
                      "path exists and is not a directory: " + directory.string(),
                      directory.string());
  }
  if (code == ERROR_PATH_NOT_FOUND) {
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    const auto retry = rep::detail::is_directory(directory);
    if (retry.ok() && retry.value()) {
      return {};
    }
  }
  return win32_failure("CreateDirectoryW", code);
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  const std::wstring wide = to_wide(path);
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (::GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data) == 0) {
    return win32_failure("GetFileAttributesExW", ::GetLastError());
  }
  const std::uint64_t size =
      (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32u) | data.nFileSizeLow;
  return size;
}

Result<std::vector<std::byte>> read_file(const std::filesystem::path& path,
                                         std::uint64_t max_bytes) {
  const std::wstring wide = to_wide(path);
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (!is_valid(handle)) {
    return win32_failure("CreateFileW(read)", ::GetLastError());
  }
  FileHandle owner(handle);

  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle, &size) == 0) {
    return win32_failure("GetFileSizeEx", ::GetLastError());
  }
  if (size.QuadPart < 0) {
    return make_error(ErrorCode::Corrupt, "file reports a negative size", path.string());
  }
  const auto total = static_cast<std::uint64_t>(size.QuadPart);
  if (total > max_bytes) {
    return make_error(ErrorCode::LimitExceeded,
                      "file holds " + std::to_string(total) + " bytes, limit is " +
                          std::to_string(max_bytes),
                      path.string());
  }
  std::vector<std::byte> buffer(static_cast<std::size_t>(total));
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (buffer.size() - offset) > 0x10000000u ? 0x10000000u : (buffer.size() - offset));
    DWORD read = 0;
    if (::ReadFile(handle, buffer.data() + offset, chunk, &read, nullptr) == 0) {
      return win32_failure("ReadFile", ::GetLastError());
    }
    if (read == 0) {
      return make_error(ErrorCode::Truncated, "file ended before its reported size", path.string());
    }
    offset += read;
  }
  return buffer;
}

Result<void> write_file_durable(const std::filesystem::path& path,
                                std::span<const std::byte> bytes) {
  const std::wstring wide = to_wide(path);
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (!is_valid(handle)) {
    return win32_failure("CreateFileW(write)", ::GetLastError());
  }
  FileHandle owner(handle);

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (bytes.size() - offset) > 0x10000000u ? 0x10000000u : (bytes.size() - offset));
    DWORD written = 0;
    if (::WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) == 0) {
      return win32_failure("WriteFile", ::GetLastError());
    }
    if (written == 0) {
      return make_error(ErrorCode::IoError, "write made no progress", path.string());
    }
    offset += written;
  }
  if (::FlushFileBuffers(handle) == 0) {
    return win32_failure("FlushFileBuffers", ::GetLastError());
  }
  return {};
}

Result<void> replace_file_atomic(const std::filesystem::path& target,
                                 const std::filesystem::path& replacement) {
  const std::wstring wide_target = to_wide(target);
  const std::wstring wide_replacement = to_wide(replacement);
  if (::ReplaceFileW(wide_target.c_str(), wide_replacement.c_str(), nullptr,
                     REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != 0) {
    return {};
  }
  const DWORD code = ::GetLastError();
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
    // No previous version: a write-through move is the atomic create.
    if (::MoveFileExW(wide_replacement.c_str(), wide_target.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
      return {};
    }
    return win32_failure("MoveFileExW", ::GetLastError());
  }
  return win32_failure("ReplaceFileW", code);
}

Result<void> write_file_atomic(const std::filesystem::path& path,
                               std::span<const std::byte> bytes) {
  const std::filesystem::path staged =
      path.parent_path() / (path.filename().string() + ".stage-" + process_token());
  const auto written = write_file_durable(staged, bytes);
  if (!written.ok()) {
    return written.error();
  }
  const auto published = replace_file_atomic(path, staged);
  if (!published.ok()) {
    const auto ignored = remove_file(staged);
    (void)ignored;
    return published.error();
  }
  return {};
}

Result<std::vector<std::string>> list_file_names(const std::filesystem::path& directory) {
  std::vector<std::string> names;
  const std::filesystem::path pattern_path = directory / L"*";
  const std::wstring pattern = pattern_path.wstring();
  WIN32_FIND_DATAW data{};
  HANDLE handle = ::FindFirstFileW(pattern.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND) {
      return names;
    }
    return win32_failure("FindFirstFileW", code);
  }
  while (true) {
    const std::wstring name(data.cFileName);
    if (name != L"." && name != L"..") {
      const int needed = ::WideCharToMultiByte(CP_UTF8, 0, name.c_str(),
                                               static_cast<int>(name.size()), nullptr, 0, nullptr,
                                               nullptr);
      if (needed > 0) {
        std::string utf8(static_cast<std::size_t>(needed), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), utf8.data(),
                              needed, nullptr, nullptr);
        names.push_back(std::move(utf8));
      }
    }
    if (::FindNextFileW(handle, &data) == 0) {
      break;
    }
  }
  ::FindClose(handle);
  return names;
}

Result<void> remove_file(const std::filesystem::path& path) {
  const std::wstring wide = to_wide(path);
  if (::DeleteFileW(wide.c_str()) != 0) {
    return {};
  }
  const DWORD code = ::GetLastError();
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
    return {};
  }
  return win32_failure("DeleteFileW", code);
}

Result<FileHandle> acquire_exclusive_lock(const std::filesystem::path& path) {
  const std::wstring wide = to_wide(path);
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (!is_valid(handle)) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
      return make_error(ErrorCode::Locked,
                        "another process already owns the writer lock for this store",
                        path.string());
    }
    return win32_failure("CreateFileW(lock)", code);
  }
  return FileHandle(handle);
}

#else   // POSIX

void FileHandle::reset() noexcept {
  if (handle_ != nullptr) {
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)) - 1;
    ::close(fd);
    handle_ = nullptr;
  }
}

void* FileHandle::release() noexcept {
  void* handle = handle_;
  handle_ = nullptr;
  return handle;
}

std::string last_system_error() { return std::strerror(errno); }

std::string process_token() {
  const auto counter = g_token_counter.fetch_add(1, std::memory_order_relaxed);
  return std::to_string(::getpid()) + "-" + std::to_string(counter);
}

Result<bool> path_exists(const std::filesystem::path& path) {
  std::error_code code;
  return std::filesystem::exists(path, code);
}

Result<bool> is_directory(const std::filesystem::path& path) {
  std::error_code code;
  return std::filesystem::is_directory(path, code);
}

Result<bool> is_reparse_point(const std::filesystem::path& path) {
  std::error_code code;
  return std::filesystem::is_symlink(std::filesystem::symlink_status(path, code));
}

Result<void> ensure_directory(const std::filesystem::path& directory) {
  std::error_code code;
  std::filesystem::create_directories(directory, code);
  if (code) {
    return make_error(ErrorCode::IoError, "create_directories failed: " + code.message(),
                      directory.string());
  }
  return {};
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  std::error_code code;
  const auto size = std::filesystem::file_size(path, code);
  if (code) {
    return make_error(ErrorCode::IoError, "file_size failed: " + code.message(), path.string());
  }
  return size;
}

Result<std::vector<std::byte>> read_file(const std::filesystem::path& path,
                                         std::uint64_t max_bytes) {
  const auto size = file_size(path);
  if (!size.ok()) {
    return size.error();
  }
  if (size.value() > max_bytes) {
    return make_error(ErrorCode::LimitExceeded, "file exceeds the read limit", path.string());
  }
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return make_error(ErrorCode::IoError, std::string("open failed: ") + last_system_error(),
                      path.string());
  }
  std::vector<std::byte> buffer(static_cast<std::size_t>(size.value()));
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const ssize_t read = ::read(fd, buffer.data() + offset, buffer.size() - offset);
    if (read <= 0) {
      ::close(fd);
      return make_error(ErrorCode::Truncated, "short read", path.string());
    }
    offset += static_cast<std::size_t>(read);
  }
  ::close(fd);
  return buffer;
}

Result<void> write_file_durable(const std::filesystem::path& path,
                                std::span<const std::byte> bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return make_error(ErrorCode::IoError, std::string("open failed: ") + last_system_error(),
                      path.string());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written <= 0) {
      ::close(fd);
      return make_error(ErrorCode::IoError, "write failed", path.string());
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    return make_error(ErrorCode::IoError, "fsync failed", path.string());
  }
  ::close(fd);
  return {};
}

Result<void> replace_file_atomic(const std::filesystem::path& target,
                                 const std::filesystem::path& replacement) {
  if (::rename(replacement.c_str(), target.c_str()) != 0) {
    return make_error(ErrorCode::IoError, std::string("rename failed: ") + last_system_error(),
                      target.string());
  }
  const int directory = ::open(target.parent_path().c_str(), O_RDONLY);
  if (directory >= 0) {
    (void)::fsync(directory);
    ::close(directory);
  }
  return {};
}

Result<void> write_file_atomic(const std::filesystem::path& path,
                               std::span<const std::byte> bytes) {
  const std::filesystem::path staged =
      path.parent_path() / (path.filename().string() + ".stage-" + process_token());
  const auto written = write_file_durable(staged, bytes);
  if (!written.ok()) {
    return written.error();
  }
  const auto published = replace_file_atomic(path, staged);
  if (!published.ok()) {
    const auto ignored = remove_file(staged);
    (void)ignored;
    return published.error();
  }
  return {};
}

Result<std::vector<std::string>> list_file_names(const std::filesystem::path& directory) {
  std::vector<std::string> names;
  std::error_code code;
  for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
    names.push_back(entry.path().filename().string());
  }
  if (code) {
    return make_error(ErrorCode::IoError, "directory iteration failed: " + code.message(),
                      directory.string());
  }
  return names;
}

Result<void> remove_file(const std::filesystem::path& path) {
  std::error_code code;
  std::filesystem::remove(path, code);
  if (code) {
    return make_error(ErrorCode::IoError, "remove failed: " + code.message(), path.string());
  }
  return {};
}

Result<FileHandle> acquire_exclusive_lock(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    return make_error(ErrorCode::IoError, std::string("open failed: ") + last_system_error(),
                      path.string());
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return make_error(ErrorCode::Locked, "another process owns the writer lock", path.string());
  }
  return FileHandle(reinterpret_cast<void*>(static_cast<std::intptr_t>(fd) + 1));
}

#endif

} // namespace rep::detail
