#pragma once

#include <expected>
#include <string>

namespace Process
{
	inline constexpr int kRebootDelayMinutes = 1;

	class ISystemControl
	{
	public:
		virtual ~ISystemControl(void) = default;

		virtual auto request_reboot(void) -> std::expected<void, std::string> = 0;
	};
}
