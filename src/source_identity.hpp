#pragma once
#include <filesystem>
#include <string>
namespace edgeecho::detail {
// Read-only native file ID and metadata change token. Does not lock or write.
std::string native_source_token(const std::filesystem::path &);
}
