#pragma once

#include <expected>
#include <functional>
#include <string>

namespace Messaging
{
	// 큐 소비 경계. 수명주기는 설정 → handler 등록 → start → 사용 → stop 순서를 지킨다.
	class IMessageConsumer
	{
	public:
		virtual ~IMessageConsumer(void) = default;

		// 메시지 원문 한 건을 받는다. 실패를 반환하면 소비자가 로그를 남긴다.
		using Handler = std::function<std::expected<void, std::string>(const std::string&)>;

		virtual auto handler(Handler value) -> std::expected<void, std::string> = 0;

		virtual auto start(void) -> std::expected<void, std::string> = 0;
		virtual auto stop(void) -> void = 0;

		virtual auto running(void) const -> bool = 0;
	};
} // namespace Messaging
