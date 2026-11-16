#pragma once
#include <algorithm>
#include <liltview/memory.h>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>
namespace liltview
{
// RAII only: the storage and reference count remain a pure C object.
class MemoryBlock
{
public:
    MemoryBlock() = default;
    MemoryBlock(std::vector<std::uint8_t> bytes)
    {
        if (bytes.empty())
            return;
        auto storage = std::make_unique<std::vector<std::uint8_t>>(std::move(bytes));
        block_       = lv_memory_block_wrap(
            storage->data(), storage->size(),
            [](void *p, void *, std::uint64_t) { delete static_cast<std::vector<std::uint8_t> *>(p); }, storage.get());
        if (!block_)
            throw std::bad_alloc();
        capacity_ = storage->capacity();
        storage.release();
    }
    static MemoryBlock pixels(lv_pixels *pixels)
    {
        MemoryBlock result;
        result.block_ = lv_pixels_acquire_memory(pixels);
        if (!result.block_)
            throw std::bad_alloc();
        result.capacity_ = 0; // Borrowed backing remains charged to Core, not this wrapper.
        return result;
    }
    MemoryBlock(const MemoryBlock &other) :
        block_(other.block_),
        capacity_(other.capacity_)
    {
        lv_memory_block_retain(block_);
    }
    MemoryBlock(MemoryBlock &&other) noexcept :
        block_(std::exchange(other.block_, nullptr)),
        capacity_(other.capacity_)
    {
        other.capacity_ = 0;
    }
    MemoryBlock &operator=(MemoryBlock other) noexcept
    {
        swap(other);
        return *this;
    }
    ~MemoryBlock()
    {
        lv_memory_block_release(block_);
    }
    void swap(MemoryBlock &other) noexcept
    {
        std::swap(block_, other.block_);
        std::swap(capacity_, other.capacity_);
    }
    void reset() noexcept
    {
        MemoryBlock{}.swap(*this);
    }
    explicit operator bool() const
    {
        return block_ != nullptr;
    }
    const std::uint8_t *data() const
    {
        return static_cast<const std::uint8_t *>(lv_memory_block_data(block_));
    }
    std::size_t size() const
    {
        return static_cast<std::size_t>(lv_memory_block_size(block_));
    }
    std::size_t capacity() const
    {
        return capacity_;
    }
    bool empty() const
    {
        return size() == 0;
    }
    const std::uint8_t *begin() const
    {
        return data();
    }
    const std::uint8_t *end() const
    {
        return data() ? data() + size() : nullptr;
    }
    const std::uint8_t &operator[](std::size_t at) const
    {
        return data()[at];
    }
    const std::uint8_t &front() const { return data()[0]; }
    const MemoryBlock *operator->() const
    {
        return this;
    }
    const MemoryBlock &operator*() const
    {
        return *this;
    }
    operator std::span<const std::uint8_t>() const
    {
        return {data(), size()};
    }
    bool operator==(const std::vector<std::uint8_t> &other) const
    {
        return size() == other.size() && std::equal(begin(), end(), other.begin());
    }

private:
    lv_memory_block *block_ = nullptr;
    std::size_t capacity_   = 0;
};
} // namespace liltview
