#pragma once

#include <expected>
#include <string>

namespace YirangAgent
{
	auto validate_storage_paths(const std::string& version_root, const std::string& service_root) -> std::expected<void, std::string>;
}
