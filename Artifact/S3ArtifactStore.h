#pragma once

#include "IArtifactStore.h"

#include <memory>

namespace AWSService
{
	class AWSS3Client;
}

namespace Artifact
{
	// CppToolkit AWSService::AWSS3Client 를 감싸는 구현.
	// SDK 헤더가 여기서 새지 않도록 클라이언트를 전방 선언 + unique_ptr 로 감춘다.
	class S3ArtifactStore : public IArtifactStore
	{
	public:
		explicit S3ArtifactStore(const StoreOptions& options);
		~S3ArtifactStore(void) override;

		auto upload(const std::string& object_key, const std::string& local_path) -> std::expected<void, std::string> override;
		auto presign(const std::string& object_key) -> std::expected<std::string, std::string> override;
		auto download(const std::string& object_key, const std::string& local_path) -> std::expected<void, std::string> override;
		auto exists(const std::string& object_key) -> std::expected<bool, std::string> override;

		auto options(void) const -> StoreOptions;

	private:
		StoreOptions options_;
		std::unique_ptr<AWSService::AWSS3Client> client_;
	};
} // namespace Artifact
