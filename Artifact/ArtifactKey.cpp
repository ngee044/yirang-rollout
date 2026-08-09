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

	auto make_object_key(const std::string& release_id, const std::string& install_path) -> std::expected<std::string, std::string>
	{
		if (release_id.empty())
		{
			return std::unexpected("release_id is required");
		}

		// release_id 에 구분자가 있으면 키 계층이 깨져 다른 릴리스 영역을 침범할 수 있다.
		if (contains_separator(release_id))
		{
			return std::unexpected(std::format("release_id must not contain a path separator: '{}'", release_id));
		}

		// '.' 과 '..' 는 구분자를 포함하지 않아 위 검사를 통과한다. '..' 를 허용하면
		// <version_root>/../<install_path> 로 설치 루트 밖에 파일이 떨어지고, '.' 은 릴리스 격리를 깬다.
		if (release_id == "." || release_id == "..")
		{
			return std::unexpected(std::format("release_id must not be a path reference: '{}'", release_id));
		}

		if (install_path.empty())
		{
			return std::unexpected("install_path is required");
		}

		// 역슬래시는 Windows 경로가 그대로 넘어온 경우다. 키 규칙을 하나로 유지하기 위해 거부한다.
		if (install_path.find('\\') != std::string::npos)
		{
			return std::unexpected(std::format("install_path must use '/' as separator: '{}'", install_path));
		}

		if (install_path.front() == '/')
		{
			return std::unexpected(std::format("install_path must be relative: '{}'", install_path));
		}

		// 'C:/...' 같은 드라이브 지정도 절대 경로다.
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
} // namespace Artifact
