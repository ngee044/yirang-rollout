#include "PosixSystemControl.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <string>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Process
{
	namespace
	{
		constexpr int kExecFailedExitCode = 127;

		const char* const kShutdownCandidates[] = { "/sbin/shutdown", "/usr/sbin/shutdown", "/bin/shutdown" };

		auto locate_shutdown(void) -> std::string
		{
			for (const auto* candidate : kShutdownCandidates)
			{
				if (::access(candidate, X_OK) == 0)
				{
					return std::string(candidate);
				}
			}

			return std::string();
		}
	}

	auto PosixSystemControl::request_reboot(void) -> std::expected<void, std::string>
	{
		if (::geteuid() != 0)
		{
			return std::unexpected("rebooting requires root privileges — run the agent as a service account that may reboot this device");
		}

		const auto shutdown_path = locate_shutdown();
		if (shutdown_path.empty())
		{
			return std::unexpected("cannot find an executable 'shutdown' in /sbin, /usr/sbin or /bin");
		}

		std::string program = shutdown_path;
		std::string restart_flag = "-r";
		std::string schedule = std::format("+{}", kRebootDelayMinutes);

		char* arguments[] = { program.data(), restart_flag.data(), schedule.data(), nullptr };

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

			::setpgid(0, 0);

			::execv(arguments[0], arguments);

			const int reason = errno;
			(void)::write(report[1], &reason, sizeof(reason));
			::_exit(kExecFailedExitCode);
		}

		::close(report[1]);

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

			return std::unexpected(std::format("cannot run '{}': {}", shutdown_path, std::strerror(child_errno)));
		}

		return {};
	}
}
