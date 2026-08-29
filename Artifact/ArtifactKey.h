#pragma once

#include <expected>
#include <string>

namespace Artifact
{
	auto validate_release_id(const std::string& release_id) -> std::expected<void, std::string>;

	auto make_object_key(const std::string& release_id, const std::string& install_path) -> std::expected<std::string, std::string>;
}
