#include "PosixProcessSupervisor.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <format>
#include <thread>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#define YIRANG_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define YIRANG_ENVIRON environ
#endif

namespace Process
{
	namespace
	{
		constexpr auto kExecFailedExitCode = 127;
		constexpr auto kPollInterval = std::chrono::milliseconds(20);

		// fork 이후 자식은 async-signal-safe 함수만 쓸 수 있으므로 문자열 배열을 미리 만들어 둔다.
		auto build_arguments(const ProcessStartOptions& options, std::vector<std::string>& storage) -> std::vector<char*>
		{
			storage.clear();
			storage.push_back(options.executable_path);
			for (const auto& argument : options.arguments)
			{
				storage.push_back(argument);
			}

			std::vector<char*> pointers;
			pointers.reserve(storage.size() + 1);
			for (auto& value : storage)
			{
				pointers.push_back(value.data());
			}
			pointers.push_back(nullptr);

			return pointers;
		}

		auto build_environment(const ProcessStartOptions& options, std::vector<std::string>& storage) -> std::vector<char*>
		{
			storage.clear();

			// 같은 이름이 지정되면 부모 값을 버려야 하므로 덮어쓸 키를 먼저 모은다.
			for (char** entry = YIRANG_ENVIRON; entry != nullptr && *entry != nullptr; ++entry)
			{
				const std::string line(*entry);
				const auto separator = line.find('=');
				const std::string name = (separator == std::string::npos) ? line : line.substr(0, separator);

				const auto overridden = std::find_if(options.environment.begin(), options.environment.end(), [&name](const auto& pair) { return pair.first == name; });
				if (overridden == options.environment.end())
				{
					storage.push_back(line);
				}
			}

			for (const auto& [name, value] : options.environment)
			{
				storage.push_back(std::format("{}={}", name, value));
			}

			std::vector<char*> pointers;
			pointers.reserve(storage.size() + 1);
			for (auto& value : storage)
			{
				pointers.push_back(value.data());
			}
			pointers.push_back(nullptr);

			return pointers;
		}

		auto to_status(int wait_status) -> ProcessStatus
		{
			ProcessStatus status;

			if (WIFEXITED(wait_status))
			{
				status.state = ProcessState::Exited;
				status.exit_code = WEXITSTATUS(wait_status);
				return status;
			}

			if (WIFSIGNALED(wait_status))
			{
				status.state = ProcessState::Signaled;
				status.signal = WTERMSIG(wait_status);
				return status;
			}

			status.state = ProcessState::Unknown;
			return status;
		}
	}

	auto PosixProcessSupervisor::start(const ProcessStartOptions& options) -> std::expected<ProcessHandle, std::string>
	{
		if (options.executable_path.empty())
		{
			return std::unexpected("executable_path is required");
		}

		std::vector<std::string> argument_storage;
		std::vector<std::string> environment_storage;
		auto argument_pointers = build_arguments(options, argument_storage);
		auto environment_pointers = build_environment(options, environment_storage);

		// exec 실패는 자식이 이미 태어난 뒤에 일어난다. 실패 사유를 부모로 되돌리려면 별도 통로가 필요하다.
		// CLOEXEC 파이프를 쓰면 exec 성공 시 자동으로 닫혀 EOF 가 곧 성공 신호가 된다.
		int report[2] = { -1, -1 };
		if (::pipe(report) != 0)
		{
			return std::unexpected(std::format("cannot create report pipe: {}", std::strerror(errno)));
		}

		if (::fcntl(report[1], F_SETFD, FD_CLOEXEC) != 0)
		{
			::close(report[0]);
			::close(report[1]);

			return std::unexpected(std::format("cannot set FD_CLOEXEC on report pipe: {}", std::strerror(errno)));
		}

		const std::string working_directory = options.working_directory;

		const pid_t child = ::fork();
		if (child < 0)
		{
			const auto reason = std::format("cannot fork: {}", std::strerror(errno));
			::close(report[0]);
			::close(report[1]);

			return std::unexpected(reason);
		}

		if (child == 0)
		{
			::close(report[0]);

			// 자식을 자기 자신의 프로세스 그룹 리더로 만들어 손자까지 한 번에 신호를 보낼 수 있게 한다.
			::setpgid(0, 0);

			if (!working_directory.empty() && ::chdir(working_directory.c_str()) != 0)
			{
				const int reason = errno;
				(void)::write(report[1], &reason, sizeof(reason));
				::_exit(kExecFailedExitCode);
			}

			::execve(argument_pointers[0], argument_pointers.data(), environment_pointers.data());

			const int reason = errno;
			(void)::write(report[1], &reason, sizeof(reason));
			::_exit(kExecFailedExitCode);
		}

		::close(report[1]);

		// 부모도 setpgid 를 호출해 자식이 exec 하기 전 신호를 보내는 경쟁을 없앤다(둘 중 먼저 성공한 쪽이 유효).
		::setpgid(child, child);

		int child_errno = 0;
		ssize_t received = 0;
		while (true)
		{
			received = ::read(report[0], &child_errno, sizeof(child_errno));
			if (received >= 0 || errno != EINTR)
			{
				break;
			}
		}
		::close(report[0]);

		if (received == sizeof(child_errno))
		{
			int discarded = 0;
			::waitpid(child, &discarded, 0);

			return std::unexpected(std::format("cannot start '{}': {}", options.executable_path, std::strerror(child_errno)));
		}

		return ProcessHandle{ static_cast<int64_t>(child) };
	}

	auto PosixProcessSupervisor::stop(const ProcessHandle& handle, std::chrono::seconds timeout) -> std::expected<void, std::string>
	{
		if (handle.id <= 0)
		{
			return std::unexpected("invalid process handle");
		}

		auto current = status(handle);
		if (!current)
		{
			return std::unexpected(current.error());
		}

		if (current.value().state != ProcessState::Running)
		{
			return {};
		}

		// 음수 pid 는 프로세스 그룹 전체를 가리킨다. 자식이 띄운 손자까지 함께 정리한다.
		if (::kill(-static_cast<pid_t>(handle.id), SIGTERM) != 0 && errno != ESRCH)
		{
			return std::unexpected(std::format("cannot send SIGTERM to {}: {}", handle.id, std::strerror(errno)));
		}

		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline)
		{
			auto polled = status(handle);
			if (!polled)
			{
				return std::unexpected(polled.error());
			}

			if (polled.value().state != ProcessState::Running)
			{
				return {};
			}

			std::this_thread::sleep_for(kPollInterval);
		}

		// graceful 종료를 무시하는 프로세스가 배포를 막지 않도록 강제 종료로 폴백한다 (FR-PRC-01).
		if (::kill(-static_cast<pid_t>(handle.id), SIGKILL) != 0 && errno != ESRCH)
		{
			return std::unexpected(std::format("cannot send SIGKILL to {}: {}", handle.id, std::strerror(errno)));
		}

		auto reaped = reap(handle.id);
		if (!reaped)
		{
			return std::unexpected(reaped.error());
		}

		return {};
	}

	auto PosixProcessSupervisor::status(const ProcessHandle& handle) -> std::expected<ProcessStatus, std::string>
	{
		if (handle.id <= 0)
		{
			return std::unexpected("invalid process handle");
		}

		{
			std::scoped_lock<std::mutex> guard(mutex_);
			const auto cached = finished_.find(handle.id);
			if (cached != finished_.end())
			{
				return cached->second;
			}
		}

		int wait_status = 0;
		const pid_t result = ::waitpid(static_cast<pid_t>(handle.id), &wait_status, WNOHANG);

		if (result == 0)
		{
			return ProcessStatus{ ProcessState::Running, std::nullopt, std::nullopt };
		}

		if (result < 0)
		{
			// ECHILD 는 우리 자식이 아니거나 이미 수확된 경우다. 상태를 단정할 수 없으므로 Unknown 으로 보고한다.
			if (errno == ECHILD)
			{
				return ProcessStatus{ ProcessState::Unknown, std::nullopt, std::nullopt };
			}

			return std::unexpected(std::format("cannot query process {}: {}", handle.id, std::strerror(errno)));
		}

		const auto status = to_status(wait_status);

		std::scoped_lock<std::mutex> guard(mutex_);
		finished_.insert({ handle.id, status });

		return status;
	}

	auto PosixProcessSupervisor::reap(int64_t id) -> std::expected<ProcessStatus, std::string>
	{
		int wait_status = 0;
		pid_t result = 0;
		while (true)
		{
			result = ::waitpid(static_cast<pid_t>(id), &wait_status, 0);
			if (result >= 0 || errno != EINTR)
			{
				break;
			}
		}

		if (result < 0)
		{
			if (errno == ECHILD)
			{
				return ProcessStatus{ ProcessState::Unknown, std::nullopt, std::nullopt };
			}

			return std::unexpected(std::format("cannot wait for process {}: {}", id, std::strerror(errno)));
		}

		const auto status = to_status(wait_status);

		std::scoped_lock<std::mutex> guard(mutex_);
		finished_.insert({ id, status });

		return status;
	}
} // namespace Process
