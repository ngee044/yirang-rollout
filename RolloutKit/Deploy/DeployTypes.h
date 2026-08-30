#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace Deploy
{
	inline constexpr auto kNoActiveRelease = "no active release to start";

	auto validate_release_relative_path(const std::string& path, const std::string& label) -> std::expected<void, std::string>;

	struct ServiceSpec
	{
		std::string executable;

		std::vector<std::string> arguments;

		std::string working_directory;

		std::chrono::seconds stop_timeout{ 30 };

		std::chrono::seconds startup_timeout{ 60 };
	};

	struct RuntimeRecord
	{
		std::string release_id;

		int64_t process_id{ 0 };

		uint64_t start_token{ 0 };

		std::string boot_identity;
	};
}
