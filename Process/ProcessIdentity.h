#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace Process
{
	auto start_token(int64_t id) -> std::expected<uint64_t, std::string>;

	auto boot_identity(void) -> std::expected<std::string, std::string>;
}
