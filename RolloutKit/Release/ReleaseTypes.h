#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Release
{
	struct ArtifactEntry
	{
		std::string install_path;
		std::string sha256;
		uint64_t size_bytes{ 0 };

		std::string source_path;
	};

	struct Manifest
	{
		std::string release_id;

		std::string created_at;

		std::vector<ArtifactEntry> artifacts;
	};
}
