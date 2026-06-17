#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace obe {

// ---------------------------------------------------------------------------
// FlatHashMap<Key, Value> — open-addressing hash map for the order index
// ---------------------------------------------------------------------------
//
// Replaces std::unordered_map for OrderId -> Order*. Profiling (PROFILING.md)
// showed the node-based unordered_map spending a large share of the program in
// malloc/free — one heap node per insert, one free per erase — exactly the
// traffic the MemoryPool eliminates for Orders.
//
// This map is *allocation-free on the hot path*: all entries live in three
// parallel, contiguous, cache-friendly arrays (keys / values / state) sized
// once via reserve(). Insert/find/erase touch no allocator. It uses:
//   * linear probing (great cache behaviour: probes walk adjacent slots),
//   * a separate state byte (so any Key value is valid — no reserved sentinel),
//   * backward-shift deletion (Knuth 6.4 Algorithm R) instead of tombstones,
//     keeping probe sequences short under heavy insert/erase churn.
//
// Design choice — flat hash vs. slot table: a direct-indexed slot table
// (vector indexed by id) is even faster and simpler *if* ids are dense and
// bounded. We chose the flat hash because it makes no assumption about id
// density (sparse/sharded/large ids all work), while still being allocation-
// free and cache-friendly. See INDEX.md for the evaluation and measurements.
template <typename Key, typename Value>
class FlatHashMap {
    static_assert(std::is_integral<Key>::value,
                  "FlatHashMap is specialised for integral keys");

public:
    explicit FlatHashMap(std::size_t initial_capacity = 16) {
        rehash(round_up_pow2(initial_capacity < 8 ? 8 : initial_capacity));
    }

    // Ensure the table can hold `n` entries without rehashing (keeps the load
    // factor under the max). Call once up front to stay allocation-free.
    void reserve(std::size_t n) {
        const std::size_t need = round_up_pow2((n * 8) / 7 + 1);
        if (need > capacity_) {
            rehash(need);
        }
    }

    // Returns a pointer to the value for `key`, or nullptr if absent.
    Value* find(Key key) noexcept {
        std::size_t i = home(key);
        while (state_[i] != kEmpty) {
            if (state_[i] == kOccupied && keys_[i] == key) {
                return &values_[i];
            }
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    const Value* find(Key key) const noexcept {
        return const_cast<FlatHashMap*>(this)->find(key);
    }

    bool contains(Key key) const noexcept { return find(key) != nullptr; }

    // Insert (key, value). If the key exists, its value is overwritten.
    void insert(Key key, Value value) {
        if ((size_ + 1) * 8 >= capacity_ * 7) { // load factor > 7/8
            rehash(capacity_ * 2);
        }
        std::size_t i = home(key);
        while (state_[i] == kOccupied) {
            if (keys_[i] == key) { // update in place
                values_[i] = value;
                return;
            }
            i = (i + 1) & mask_;
        }
        keys_[i] = key;
        values_[i] = value;
        state_[i] = kOccupied;
        ++size_;
    }

    // Erase by key. Returns true if the key was present.
    bool erase(Key key) noexcept {
        std::size_t i = home(key);
        while (state_[i] != kEmpty) {
            if (state_[i] == kOccupied && keys_[i] == key) {
                erase_at(i);
                return true;
            }
            i = (i + 1) & mask_;
        }
        return false;
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    static constexpr std::uint8_t kEmpty = 0;
    static constexpr std::uint8_t kOccupied = 1;

    static std::size_t round_up_pow2(std::size_t v) {
        if (v < 2) return 2;
        --v;
        for (std::size_t i = 1; i < sizeof(std::size_t) * 8; i <<= 1) {
            v |= v >> i;
        }
        return v + 1;
    }

    // SplitMix64 finalizer: scrambles sequential ids so linear probing does not
    // degrade into long runs, while staying a couple of cheap integer ops.
    std::size_t home(Key key) const noexcept {
        std::uint64_t x = static_cast<std::uint64_t>(key);
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        x ^= x >> 31;
        return static_cast<std::size_t>(x) & mask_;
    }

    // Backward-shift deletion (Knuth Algorithm R): fill the hole at `i` by
    // sliding back any following entry whose probe chain runs through `i`.
    void erase_at(std::size_t i) noexcept {
        state_[i] = kEmpty;
        --size_;
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (state_[j] == kEmpty) {
                return;
            }
            const std::size_t k = home(keys_[j]);
            // Leave keys_[j] in place if its home k lies cyclically in (i, j];
            // otherwise it can move back to the hole at i.
            const bool cannot_move =
                (i <= j) ? (k > i && k <= j) : (k > i || k <= j);
            if (cannot_move) {
                continue;
            }
            keys_[i] = keys_[j];
            values_[i] = values_[j];
            state_[i] = kOccupied;
            state_[j] = kEmpty;
            i = j;
        }
    }

    void rehash(std::size_t new_cap) {
        std::vector<Key> old_keys = std::move(keys_);
        std::vector<Value> old_values = std::move(values_);
        std::vector<std::uint8_t> old_state = std::move(state_);

        capacity_ = new_cap;
        mask_ = new_cap - 1;
        keys_.assign(new_cap, Key{});
        values_.assign(new_cap, Value{});
        state_.assign(new_cap, kEmpty);
        size_ = 0;

        for (std::size_t s = 0; s < old_state.size(); ++s) {
            if (old_state[s] == kOccupied) {
                insert(old_keys[s], old_values[s]);
            }
        }
    }

    std::vector<Key> keys_;
    std::vector<Value> values_;
    std::vector<std::uint8_t> state_;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

} // namespace obe
