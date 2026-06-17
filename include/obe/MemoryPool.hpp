#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace obe {

// ---------------------------------------------------------------------------
// MemoryPool<T> — a free-list (object pool) allocator
// ---------------------------------------------------------------------------
//
// Why: every accepted order needs an Order object. Calling the global
// allocator (new/delete -> malloc/free) on the matching hot path is slow and,
// worse, non-deterministic: it can take a lock, hit the OS, or fragment. A
// free-list pool gives us O(1), allocation-free-after-warmup acquisition with
// stable latency.
//
// How:
//   * Memory is carved from large contiguous "blocks" obtained via a single
//     ::operator new each. Blocks are never freed individually; they live for
//     the pool's lifetime and are released together in the destructor (RAII).
//   * Each slot is sized/aligned to hold either a T or a FreeNode. When a slot
//     is free it stores a FreeNode pointing at the next free slot; when live it
//     stores a T. This is the classic "intrusive free list" — zero per-slot
//     bookkeeping overhead.
//   * acquire() pops a slot and placement-constructs a T in it; release()
//     destroys the T and pushes the slot back. When the free list is empty we
//     grow by one block (geometric growth keeps amortised cost O(1)).
//
// This is the ONLY place in the engine that performs raw allocation; the rest
// of the codebase stays free of new/delete per the project's RAII contract.
template <typename T>
class MemoryPool {
public:
    explicit MemoryPool(std::size_t initial_capacity = 1024)
        : slots_per_block_(initial_capacity == 0 ? 1 : initial_capacity) {
        grow();
    }

    // Non-copyable, non-movable: it owns raw storage that live objects point
    // into, so relocating it would dangle everything.
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    ~MemoryPool() {
        for (void* block : blocks_) {
            ::operator delete(block, std::align_val_t{kAlign});
        }
    }

    // Construct a T in a pooled slot. Forwards constructor arguments.
    template <typename... Args>
    T* acquire(Args&&... args) {
        if (free_list_ == nullptr) {
            grow();
        }
        FreeNode* node = free_list_;
        free_list_ = node->next;
        ++live_count_;
        return ::new (static_cast<void*>(node)) T(std::forward<Args>(args)...);
    }

    // Destroy a pooled T and return its slot to the free list.
    void release(T* obj) noexcept {
        if (obj == nullptr) {
            return;
        }
        obj->~T();
        FreeNode* node = reinterpret_cast<FreeNode*>(obj);
        node->next = free_list_;
        free_list_ = node;
        --live_count_;
    }

    // Introspection (used by tests / benchmarks to confirm no leaks).
    std::size_t live_count() const noexcept { return live_count_; }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t block_count() const noexcept { return blocks_.size(); }

private:
    struct FreeNode {
        FreeNode* next;
    };

    static constexpr std::size_t kSlotSize =
        sizeof(T) > sizeof(FreeNode) ? sizeof(T) : sizeof(FreeNode);
    static constexpr std::size_t kAlign =
        alignof(T) > alignof(FreeNode) ? alignof(T) : alignof(FreeNode);

    void grow() {
        const std::size_t n = slots_per_block_;
        // One raw, over-aligned allocation per block.
        auto* raw = static_cast<std::uint8_t*>(
            ::operator new(n * kSlotSize, std::align_val_t{kAlign}));
        blocks_.push_back(raw);
        capacity_ += n;

        // Thread the new slots onto the front of the free list.
        for (std::size_t i = 0; i < n; ++i) {
            auto* node = reinterpret_cast<FreeNode*>(raw + i * kSlotSize);
            node->next = free_list_;
            free_list_ = node;
        }

        // Geometric growth: each block is twice the previous, so a run of
        // acquisitions amortises to O(1) and we stop hitting the allocator.
        slots_per_block_ *= 2;
    }

    std::vector<void*> blocks_;
    FreeNode* free_list_ = nullptr;
    std::size_t slots_per_block_;
    std::size_t capacity_ = 0;
    std::size_t live_count_ = 0;
};

} // namespace obe
