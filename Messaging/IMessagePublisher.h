#pragma once

#include <expected>
#include <string>

namespace Messaging
{
	// 큐 발행 경계. 결과 보고는 대상 큐가 호출마다 달라질 수 있으므로 URL 을 인자로 받는다.
	class IMessagePublisher
	{
	public:
		virtual ~IMessagePublisher(void) = default;

		virtual auto send(const std::string& queue_url, const std::string& body) -> std::expected<void, std::string> = 0;
	};
} // namespace Messaging
