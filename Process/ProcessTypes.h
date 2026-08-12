#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Process
{
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

		std::optional<int32_t> exit_code;

		std::optional<int32_t> signal;
	};

	struct ProcessStartOptions
	{
		std::string executable_path;
		std::vector<std::string> arguments;

		std::string working_directory;

		std::vector<std::pair<std::string, std::string>> environment;
	};
}
