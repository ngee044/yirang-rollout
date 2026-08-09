#pragma once

#include "IProcessSupervisor.h"

#include <map>
#include <mutex>

namespace Process
{
	// POSIX(macOS·Linux) 구현. fork + execve 를 쓰고 자식을 자기 자신의 프로세스 그룹에 두어
	// 손자 프로세스까지 한 번에 신호를 보낼 수 있게 한다 (아키텍처 §17.3).
	class PosixProcessSupervisor : public IProcessSupervisor
	{
	public:
		~PosixProcessSupervisor(void) override = default;

		auto start(const ProcessStartOptions& options) -> std::expected<ProcessHandle, std::string> override;
		auto stop(const ProcessHandle& handle, std::chrono::seconds timeout) -> std::expected<void, std::string> override;
		auto status(const ProcessHandle& handle) -> std::expected<ProcessStatus, std::string> override;

	private:
		// waitpid 는 종료 상태를 한 번만 돌려주므로 수확한 결과를 보관한다.
		auto reap(int64_t id) -> std::expected<ProcessStatus, std::string>;

		std::mutex mutex_;
		std::map<int64_t, ProcessStatus> finished_;
	};
} // namespace Process
