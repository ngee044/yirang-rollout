#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Release
{
	// 릴리스에 포함되는 파일 하나. 개발자 로컬 경로는 배포 대상과 무관하므로 매니페스트에 싣지 않고,
	// 설치 시 릴리스 디렉터리 기준 상대 경로(install_path)만 계약에 포함한다.
	struct ArtifactEntry
	{
		std::string install_path;
		std::string sha256;
		uint64_t size_bytes{ 0 };

		// 매니페스트 생성 시점의 원본 경로. 진단용이며 직렬화 대상이 아니다.
		std::string source_path;
	};

	struct Manifest
	{
		std::string release_id;

		// ISO 8601 UTC. 타임존에 따라 값이 달라지면 같은 릴리스가 PC 마다 다르게 보이므로 UTC 로 고정한다.
		std::string created_at;

		std::vector<ArtifactEntry> artifacts;
	};
} // namespace Release
