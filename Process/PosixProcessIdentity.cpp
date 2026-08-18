#include "ProcessIdentity.h"

#include <cerrno>
#include <cstring>
#include <format>

#include <sys/types.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <fstream>
#include <sstream>
#endif

namespace Process
{
#if !defined(__APPLE__)
	namespace
	{
		constexpr auto kStatStartTimeField = 22;
	}
#endif

	auto start_token(int64_t id) -> std::expected<uint64_t, std::string>
	{
		if (id <= 0)
		{
			return std::unexpected("invalid process id");
		}

#if defined(__APPLE__)
		int selector[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(id) };
		kinfo_proc information{};
		size_t length = sizeof(information);

		if (::sysctl(selector, 4, &information, &length, nullptr, 0) != 0)
		{
			return std::unexpected(std::format("cannot read the start time of process {}: {}", id, std::strerror(errno)));
		}

		if (length == 0)
		{
			return std::unexpected(std::format("process {} does not exist", id));
		}

		const auto seconds = static_cast<uint64_t>(information.kp_proc.p_starttime.tv_sec);
		const auto microseconds = static_cast<uint64_t>(information.kp_proc.p_starttime.tv_usec);

		return (seconds * 1000000ULL) + microseconds;
#else
		std::ifstream source(std::format("/proc/{}/stat", id));
		if (!source.is_open())
		{
			return std::unexpected(std::format("process {} does not exist", id));
		}

		std::string line;
		std::getline(source, line);

		const auto command_end = line.rfind(')');
		if (command_end == std::string::npos)
		{
			return std::unexpected(std::format("cannot parse /proc/{}/stat", id));
		}

		std::istringstream remainder(line.substr(command_end + 1));
		std::string field;
		for (int index = 3; index <= kStatStartTimeField; ++index)
		{
			if (!(remainder >> field))
			{
				return std::unexpected(std::format("cannot parse /proc/{}/stat", id));
			}
		}

		try
		{
			return static_cast<uint64_t>(std::stoull(field));
		}
		catch (const std::exception& exception)
		{
			return std::unexpected(std::format("cannot parse the start time of process {}: {}", id, exception.what()));
		}
#endif
	}

	auto boot_identity(void) -> std::expected<std::string, std::string>
	{
#if defined(__APPLE__)
		int selector[2] = { CTL_KERN, KERN_BOOTTIME };
		timeval booted{};
		size_t length = sizeof(booted);

		if (::sysctl(selector, 2, &booted, &length, nullptr, 0) != 0)
		{
			return std::unexpected(std::format("cannot read the boot time: {}", std::strerror(errno)));
		}

		return std::format("{}.{:06}", static_cast<int64_t>(booted.tv_sec), static_cast<int64_t>(booted.tv_usec));
#else
		std::ifstream source("/proc/sys/kernel/random/boot_id");
		if (!source.is_open())
		{
			return std::unexpected("cannot read /proc/sys/kernel/random/boot_id");
		}

		std::string identity;
		std::getline(source, identity);

		if (identity.empty())
		{
			return std::unexpected("/proc/sys/kernel/random/boot_id is empty");
		}

		return identity;
#endif
	}
}
