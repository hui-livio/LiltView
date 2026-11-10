#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace liltview::platform
{
void seek_file(std::FILE *, std::uint64_t offset);
std::FILE *temporary_file(const std::filesystem::path &directory);
int file_descriptor(std::FILE *);
int duplicate_descriptor(int);
void close_descriptor(int) noexcept;
std::size_t read_at(int, void *, std::size_t bytes, std::uint64_t offset);
} // namespace liltview::platform
