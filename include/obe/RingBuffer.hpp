#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace obe {

// ---------------------------------------------------------------------------
// SpscRingBuffer<T> — wait-free single-producer / single-consumer queue
// ---------------------------------------------------------------------------
//
// Decouples the order-generator thread (producer) from the matching-engine
// thread (consumer). Exactly one thread may call push() and exactly one
// (different) thread may call pop(); under that contract the queue is wait-free
// and requires no locks.
//
// Correctness rests on two ideas:
//   1. A power-of-two capacity so index wrap is a single bitwise AND.
//   2. Release/acquire ordering on the head/tail indices. The producer
//      publishes a slot by *releasing* the tail; the consumer *acquires* the
//      tail before reading, which guarantees it observes the fully-written
//      element. Symmetrically for head.
//
// head_ and tail_ live on separate cache lines (alignas) so the producer and
// consumer never invalidate each other's cache line — avoiding false sharing,
// the classic SPSC performance killer.
template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(std::size_t capacity)
        : capacity_(round_up_pow2(capacity)),
          mask_(capacity_ - 1),
          slots_(std::allocator<Slot>{}.allocate(capacity_)) {}

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    ~SpscRingBuffer() {
        // Destroy any elements still in flight, then free raw storage.
        if (!std::is_trivially_destructible<T>::value) {
            std::size_t head = head_.load(std::memory_order_relaxed);
            const std::size_t tail = tail_.load(std::memory_order_relaxed);
            while (head != tail) {
                reinterpret_cast<T*>(&slots_[head & mask_])->~T();
                ++head;
            }
        }
        std::allocator<Slot>{}.deallocate(slots_, capacity_);
    }

    // Producer side. Returns false if the queue is full (back-pressure).
    template <typename U>
    bool push(U&& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = tail + 1;
        // Full when advancing tail would collide with the consumer's head.
        if (next - head_.load(std::memory_order_acquire) > capacity_) {
            return false;
        }
        ::new (&slots_[tail & mask_]) T(std::forward<U>(value));
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if the queue is empty.
    bool pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        T* slot = reinterpret_cast<T*>(&slots_[head & mask_]);
        out = std::move(*slot);
        slot->~T();
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    std::size_t capacity() const noexcept { return capacity_; }

    // Approximate size — exact only when the queue is quiescent.
    std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_acquire) -
               head_.load(std::memory_order_acquire);
    }

private:
    using Slot = typename std::aligned_storage<sizeof(T), alignof(T)>::type;

    static std::size_t round_up_pow2(std::size_t v) {
        if (v < 2) {
            return 2;
        }
        --v;
        for (std::size_t i = 1; i < sizeof(std::size_t) * 8; i <<= 1) {
            v |= v >> i;
        }
        return v + 1;
    }

#ifdef __cpp_lib_hardware_interference_size
    static constexpr std::size_t kCacheLine =
        std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t kCacheLine = 64;
#endif

    const std::size_t capacity_;
    const std::size_t mask_;
    Slot* slots_;

    // Producer-owned and consumer-owned indices on independent cache lines.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
};

} // namespace obe
