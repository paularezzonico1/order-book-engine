#include "obe/PriceLevel.hpp"

// PriceLevel is fully defined inline in the header (every operation is a small
// O(1) pointer splice that benefits from inlining). This translation unit
// exists so the type has a home in the build and to anchor any future
// out-of-line helpers.

namespace obe {} // namespace obe
