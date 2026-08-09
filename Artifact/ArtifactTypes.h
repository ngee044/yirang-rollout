#pragma once

#include <chrono>
#include <string>

namespace Artifact
{
	// S3 호환 저장소 접속 정보. endpoint 가 비어 있으면 실제 AWS 를, 채워지면 MinIO·LocalStack 을 대상으로 한다.
	struct StoreOptions
	{
		std::string bucket;
		std::string region{ "us-east-1" };

		// 비어 있으면 SDK 기본 자격증명 공급자(환경변수·프로파일)를 사용한다.
		std::string access_key;
		std::string secret_key;

		// 예: http://localhost:4566 (LocalStack). 채워지면 path-style 주소 방식으로 전환한다.
		std::string endpoint;

		std::chrono::seconds presigned_url_expiration{ 3600 };
	};
} // namespace Artifact
