#include "ArtifactKey.h"

#include <format>

namespace Artifact
{
	namespace
	{
		auto contains_separator(const std::string& value) -> bool { return value.find('/') != std::string::npos || value.find('\\') != std::string::npos; }

		auto has_parent_reference(const std::string& path) -> bool
		{
			size_t start = 0;
			while (start <= path.size())
			{
				const auto end = path.find('/', start);
				const auto segment = path.substr(start, (end == std::string::npos) ? std::string::npos : end - start);

				if (segment == "..")
				{
					return true;
				}

				if (end == std::string::npos)
				{
					break;
				}

				start = end + 1;
			}

			return false;
		}
	}

	auto validate_release_id(const std::string& release_id) -> std::expected<void, std::string>
	{
		if (release_id.empty())
		{
			return std::unexpected("release_id is required");
		}

		if (contains_separator(release_id))
		{
			return std::unexpected(std::format("release_id must not contain a path separator: '{}'", release_id));
		}

		if (release_id == "." || release_id == "..")
		{
			return std::unexpected(std::format("release_id must not be a path reference: '{}'", release_id));
		}

		if (release_id.size() >= 2 && release_id[1] == ':')
		{
			return std::unexpected(std::format("release_id must not be a drive path: '{}'", release_id));
		}

		return {};
	}

	auto make_object_key(const std::string& release_id, const std::string& install_path) -> std::expected<std::string, std::string>
	{
		auto valid = validate_release_id(release_id);
		if (!valid)
		{
			return std::unexpected(valid.error());
		}

		if (install_path.empty())
		{
			return std::unexpected("install_path is required");
		}

		if (install_path.find('\\') != std::string::npos)
		{
			return std::unexpected(std::format("install_path must use '/' as separator: '{}'", install_path));
		}

		if (install_path.front() == '/')
		{
			return std::unexpected(std::format("install_path must be relative: '{}'", install_path));
		}

		if (install_path.size() >= 2 && install_path[1] == ':')
		{
			return std::unexpected(std::format("install_path must be relative: '{}'", install_path));
		}

		if (has_parent_reference(install_path))
		{
			return std::unexpected(std::format("install_path must not contain '..': '{}'", install_path));
		}

		return std::format("releases/{}/{}", release_id, install_path);
	}
}
