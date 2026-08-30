#pragma once

#include "ISystemControl.h"

namespace Process
{
	class PosixSystemControl : public ISystemControl
	{
	public:
		auto request_reboot(void) -> std::expected<void, std::string> override;
	};
}
