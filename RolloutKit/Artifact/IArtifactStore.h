#pragma once

#include "ArtifactTypes.h"

#include <expected>
#include <string>

namespace Artifact
{
	class IArtifactStore
	{
	public:
		virtual ~IArtifactStore(void) = default;

		virtual auto upload(const std::string& object_key, const std::string& local_path) -> std::expected<void, std::string> = 0;

		virtual auto presign(const std::string& object_key) -> std::expected<std::string, std::string> = 0;

		virtual auto download(const std::string& object_key, const std::string& local_path) -> std::expected<void, std::string> = 0;

		virtual auto exists(const std::string& object_key) -> std::expected<bool, std::string> = 0;
	};
}
