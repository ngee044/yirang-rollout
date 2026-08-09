#pragma once

#include <string>

namespace Messaging
{
	// 큐 접속 정보. endpoint 가 비어 있으면 실제 AWS 를, 채워지면 LocalStack·호환 서비스를 대상으로 한다.
	struct QueueOptions
	{
		std::string queue_url;
		std::string region{ "us-east-1" };

		// 비어 있으면 SDK 기본 자격증명 공급자(환경변수·프로파일)를 사용한다.
		std::string access_key;
		std::string secret_key;

		std::string endpoint;

		// long polling 대기. 0 이면 short polling 이라 빈 응답이 잦고 요청 비용이 늘어난다.
		int wait_time_seconds{ 20 };

		// 처리 중 다른 소비자에게 다시 보이지 않는 시간. 핸들러 최대 소요 시간보다 길어야 중복 처리를 막는다.
		int visibility_timeout_seconds{ 300 };

		// 0 이면 SDK 기본값을 쓴다.
		int max_number_of_messages{ 0 };
	};
} // namespace Messaging
