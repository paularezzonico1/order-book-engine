#include "obe/Order.hpp"

#include <type_traits>

namespace obe {

// The Order is the unit managed by the free-list MemoryPool and threaded
// through intrusive FIFO queues. Keeping it trivially destructible lets the
// pool reclaim slots without running destructors on the hot path.
static_assert(std::is_trivially_destructible<Order>::value,
              "Order must be trivially destructible for pool reclamation");
static_assert(std::is_nothrow_move_constructible<Order>::value,
              "Order construction must not throw on the hot path");

} // namespace obe
