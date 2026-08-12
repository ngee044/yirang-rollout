#pragma once

#include <expected>
#include <functional>
#include <string>

namespace Messaging
{
	class IMessageConsumer
	{
	public:
		virtual ~IMessageConsumer(void) = default;

		using Handler = std::function<std::expected<void, std::string>(const std::string&)>;

		virtual auto handler(Handler value) -> std::expected<void, std::string> = 0;

		virtual auto start(void) -> std::expected<void, std::string> = 0;
		virtual auto stop(void) -> void = 0;

		virtual auto running(void) const -> bool = 0;
	};
}
