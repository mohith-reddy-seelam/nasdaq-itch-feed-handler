// SPDX-License-Identifier: MIT
#include "mapped_file.hpp"

#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace harness {

#if defined(_WIN32)

MappedFile::MappedFile(const std::string& path) {
  HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open " + path);
  LARGE_INTEGER size;
  GetFileSizeEx(f, &size);
  size_ = static_cast<std::size_t>(size.QuadPart);
  file_ = f;
  if (size_ == 0) return;
  HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (m == nullptr) throw std::runtime_error("cannot map " + path);
  mapping_ = m;
  data_ = static_cast<const std::byte*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0));
  if (data_ == nullptr) throw std::runtime_error("cannot map view of " + path);
}

MappedFile::~MappedFile() {
  if (data_ != nullptr) UnmapViewOfFile(data_);
  if (mapping_ != nullptr) CloseHandle(static_cast<HANDLE>(mapping_));
  if (file_ != nullptr) CloseHandle(static_cast<HANDLE>(file_));
}

void MappedFile::advise_sequential() const noexcept {
  if (size_ == 0) return;
  WIN32_MEMORY_RANGE_ENTRY range{const_cast<std::byte*>(data_), size_};
  PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
}

#else

MappedFile::MappedFile(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("cannot open " + path);
  struct stat st{};
  ::fstat(fd, &st);
  size_ = static_cast<std::size_t>(st.st_size);
  if (size_ != 0) {
    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
      ::close(fd);
      throw std::runtime_error("cannot map " + path);
    }
    data_ = static_cast<const std::byte*>(p);
  }
  ::close(fd);
}

MappedFile::~MappedFile() {
  if (data_ != nullptr) ::munmap(const_cast<std::byte*>(data_), size_);
}

void MappedFile::advise_sequential() const noexcept {
  if (size_ != 0) ::madvise(const_cast<std::byte*>(data_), size_, MADV_SEQUENTIAL);
}

#endif

std::size_t MappedFile::prefault() const noexcept {
  std::size_t sum = 0;
  for (std::size_t i = 0; i < size_; i += 4096) sum += std::to_integer<std::size_t>(data_[i]);
  return sum;
}

}  // namespace harness
