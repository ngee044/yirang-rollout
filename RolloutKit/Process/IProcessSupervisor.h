#pragma once

#include "ProcessTypes.h"

#include <chrono>
#include <expected>
#include <string>

namespace Process
{
	class IProcessSupervisor
	{
	public:
		virtual ~IProcessSupervisor(void) = default;

		virtual auto start(const ProcessStartOptions& options) -> std::expected<ProcessHandle, std::string> = 0;

		virtual auto adopt(const ProcessHandle& handle) -> std::expected<ProcessHandle, std::string> = 0;

		virtual auto stop(const ProcessHandle& handle, std::chrono::seconds timeout) -> std::expected<void, std::string> = 0;

		virtual auto status(const ProcessHandle& handle) -> std::expected<ProcessStatus, std::string> = 0;
	};
}
