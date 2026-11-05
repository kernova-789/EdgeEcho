#include "edgeecho/core.hpp"
#include "build_metadata.hpp"
namespace edgeecho {
const char *version() noexcept { return EDGEECHO_VERSION; }
const char *build_id() noexcept { return EDGEECHO_BUILD_ID; }
} // namespace edgeecho
