#pragma once

#include "ReleaseTypes.h"

#include <chrono>
#include <expected>
#include <string>
#include <vector>

namespace Release
{
	class ReleaseManifest
	{
	public:
		static auto build(const std::vector<std::string>& upload_file_list, const std::string& release_id, const std::chrono::system_clock::time_point& created_at)
			-> std::expected<Manifest, std::string>;

		static auto serialize(const Manifest& manifest) -> std::string;
		static auto parse(const std::string& text) -> std::expected<Manifest, std::string>;

		static auto make_release_id(const std::chrono::system_clock::time_point& when) -> std::string;

		static auto file_sha256(const std::string& path) -> std::expected<std::string, std::string>;
	};
}
