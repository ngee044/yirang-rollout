#pragma once

#include "IProcessSupervisor.h"

#include <map>
#include <mutex>

namespace Process
{
	class PosixProcessSupervisor : public IProcessSupervisor
	{
	public:
		~PosixProcessSupervisor(void) override = default;

		auto start(const ProcessStartOptions& options) -> std::expected<ProcessHandle, std::string> override;
		auto stop(const ProcessHandle& handle, std::chrono::seconds timeout) -> std::expected<void, std::string> override;
		auto status(const ProcessHandle& handle) -> std::expected<ProcessStatus, std::string> override;

	private:
		auto reap(int64_t id) -> std::expected<ProcessStatus, std::string>;

		std::mutex mutex_;
		std::map<int64_t, ProcessStatus> finished_;
	};
}
