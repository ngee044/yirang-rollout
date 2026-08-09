#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Process
{
	// 프로세스 식별자. POSIX 는 pid, Windows 는 process id 를 담고 실제 핸들은 구현체가 내부에서 관리한다.
	struct ProcessHandle
	{
		int64_t id{ 0 };
	};

	enum class ProcessState : uint8_t
	{
		Running,
		Exited,
		Signaled,
		Unknown
	};

	struct ProcessStatus
	{
		ProcessState state{ ProcessState::Unknown };

		// Exited 일 때만 채워진다.
		std::optional<int32_t> exit_code;

		// Signaled 일 때만 채워진다 (SIGKILL 폴백으로 종료된 경우 등).
		std::optional<int32_t> signal;
	};

	struct ProcessStartOptions
	{
		std::string executable_path;
		std::vector<std::string> arguments;

		// 비어 있으면 부모 프로세스의 작업 디렉터리를 그대로 쓴다.
		std::string working_directory;

		// 부모 환경에 덧붙일 변수. 같은 이름이 있으면 덮어쓴다.
		std::vector<std::pair<std::string, std::string>> environment;
	};
} // namespace Process
