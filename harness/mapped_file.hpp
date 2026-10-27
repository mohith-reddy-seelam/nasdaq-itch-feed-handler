// SPDX-License-Identifier: MIT
//
// Read-only memory-mapped file. A full NASDAQ day is ~8-12 GB; mapping it lets
// the replay walk it as one std::span with the OS paging it in, instead of
// copying it into the heap.
#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace harness {

class MappedFile {
 public:
  explicit MappedFile(const std::string& path);
  ~MappedFile();
  MappedFile(const MappedFile&) = delete("owns an OS mapping");
  MappedFile& operator=(const MappedFile&) = delete("owns an OS mapping");

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }

  /// Touches every page so later passes measure the parser, not the disk.
  /// Returns a checksum so the reads cannot be optimised away.
  std::size_t prefault() const noexcept;

  /// Asks the OS to read ahead sequentially (best effort).
  void advise_sequential() const noexcept;

 private:
  const std::byte* data_ = nullptr;
  std::size_t size_ = 0;
  void* file_ = nullptr;
  void* mapping_ = nullptr;
};

}  // namespace harness
