#pragma once

#include <expected>
#include <string>

namespace Messaging
{
	class IMessagePublisher
	{
	public:
		virtual ~IMessagePublisher(void) = default;

		virtual auto send(const std::string& queue_url, const std::string& body) -> std::expected<void, std::string> = 0;
	};
}
