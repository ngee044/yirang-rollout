#pragma once

#include "IProcessSupervisor.h"

#include <map>
#include <mutex>
#include <set>

namespace Process
{
	class PosixProcessSupervisor : public IProcessSupervisor
	{
	public:
		~PosixProcessSupervisor(void) override = default;

		auto start(const ProcessStartOptions& options) -> std::expected<ProcessHandle, std::string> override;
		auto adopt(const ProcessHandle& handle) -> std::expected<ProcessHandle, std::string> override;
		auto stop(const ProcessHandle& handle, std::chrono::seconds timeout) -> std::expected<void, std::string> override;
		auto status(const ProcessHandle& handle) -> std::expected<ProcessStatus, std::string> override;

	private:
		auto reap(int64_t id) -> std::expected<ProcessStatus, std::string>;

		auto adopted(int64_t id) -> bool;
		auto adopted_status(const ProcessHandle& handle) -> std::expected<ProcessStatus, std::string>;
		auto settle(const ProcessHandle& handle, std::chrono::seconds timeout) -> std::expected<void, std::string>;

		std::mutex mutex_;
		std::map<int64_t, ProcessStatus> finished_;
		std::set<int64_t> adopted_;
	};
}
