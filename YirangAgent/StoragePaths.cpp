#include "StoragePaths.h"

#include <algorithm>
#include <filesystem>
#include <format>

namespace YirangAgent
{
	auto validate_storage_paths(const std::string& version_root, const std::string& service_root) -> std::expected<void, std::string>
	{
		if (service_root.empty())
		{
			return {};
		}

		std::error_code error;
		const auto absolute_versions = std::filesystem::absolute(version_root, error);
		if (error)
		{
			return std::unexpected(std::format("cannot resolve version_root: {}", error.message()));
		}
		auto versions = std::filesystem::weakly_canonical(absolute_versions, error);
		if (error)
		{
			return std::unexpected(std::format("cannot resolve version_root: {}", error.message()));
		}

		const auto absolute_service = std::filesystem::absolute(service_root, error);
		if (error)
		{
			return std::unexpected(std::format("cannot resolve service_root: {}", error.message()));
		}
		const auto service = std::filesystem::weakly_canonical(absolute_service, error);
		if (error)
		{
			return std::unexpected(std::format("cannot resolve service_root: {}", error.message()));
		}

		if (versions.has_relative_path() && versions.filename().empty())
		{
			versions = versions.parent_path();
		}

		if (std::mismatch(versions.begin(), versions.end(), service.begin(), service.end()).first == versions.end())
		{
			return std::unexpected("service_root must not be the same as or inside version_root");
		}

		auto releases = std::filesystem::weakly_canonical(service / "releases", error);
		if (error)
		{
			return std::unexpected(std::format("cannot resolve service release directory: {}", error.message()));
		}
		if (releases.has_relative_path() && releases.filename().empty())
		{
			releases = releases.parent_path();
		}
		const auto overlap = std::mismatch(versions.begin(), versions.end(), releases.begin(), releases.end());
		if (overlap.first == versions.end() || overlap.second == releases.end())
		{
			return std::unexpected("version_root must not overlap service_root/releases");
		}

		return {};
	}
}
