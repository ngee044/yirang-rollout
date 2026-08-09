#pragma once

#include "ArtifactTypes.h"

#include <expected>
#include <string>

namespace Artifact
{
	// 아티팩트 저장소 경계. CLI 는 upload·presign 을, Agent 는 download 를 쓴다.
	// 저장소 구현을 이 뒤에 두어 배포 로직이 SDK 타입을 직접 다루지 않게 한다.
	class IArtifactStore
	{
	public:
		virtual ~IArtifactStore(void) = default;

		virtual auto upload(const std::string& object_key, const std::string& local_path) -> std::expected<void, std::string> = 0;

		// 타겟 PC 에 자격증명을 두지 않기 위해 CLI 가 발급해 배포 티켓에 실어 보낸다.
		virtual auto presign(const std::string& object_key) -> std::expected<std::string, std::string> = 0;

		virtual auto download(const std::string& object_key, const std::string& local_path) -> std::expected<void, std::string> = 0;

		virtual auto exists(const std::string& object_key) -> std::expected<bool, std::string> = 0;
	};
} // namespace Artifact
