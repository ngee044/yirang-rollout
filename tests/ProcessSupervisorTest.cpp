#include <gtest/gtest.h>

#include "PosixProcessSupervisor.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <random>

using namespace Process;

namespace
{
	constexpr auto kShell = "/bin/sh";

	auto wait_until_finished(IProcessSupervisor& supervisor, const ProcessHandle& handle, std::chrono::seconds timeout) -> ProcessStatus
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;

		ProcessStatus last{ ProcessState::Unknown, std::nullopt, std::nullopt };
		while (std::chrono::steady_clock::now() < deadline)
		{
			auto current = supervisor.status(handle);
			if (!current)
			{
				return last;
			}

			last = current.value();
			if (last.state != ProcessState::Running)
			{
				return last;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}

		return last;
	}

	class TemporaryDirectory
	{
	public:
		TemporaryDirectory()
		{
			static int sequence = 0;
			static const auto token = std::to_string(std::random_device{}());
			path_ = std::filesystem::temp_directory_path() / ("yirang-process-test-" + token + "-" + std::to_string(++sequence));

			std::error_code error;
			std::filesystem::create_directories(path_, error);
		}

		~TemporaryDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(path_, error);
		}

		auto path(void) const -> std::filesystem::path { return path_; }

		auto read(const std::string& name) const -> std::string
		{
			std::ifstream stream(path_ / name, std::ios::binary);

			std::ostringstream buffer;
			buffer << stream.rdbuf();

			return buffer.str();
		}

		auto wait_for(const std::string& name, std::chrono::seconds timeout) const -> bool
		{
			const auto deadline = std::chrono::steady_clock::now() + timeout;
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (std::filesystem::exists(path_ / name))
				{
					return true;
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}

			return false;
		}

	private:
		std::filesystem::path path_;
	};
}

TEST(ProcessSupervisorTest, StartsAndStopsProcess)
{
	PosixProcessSupervisor supervisor;

	const auto handle = supervisor.start({ kShell, { "-c", "sleep 30" }, "", {} });
	ASSERT_TRUE(handle.has_value()) << (handle.has_value() ? "" : handle.error());

	const auto running = supervisor.status(handle.value());
	ASSERT_TRUE(running.has_value());
	EXPECT_EQ(running.value().state, ProcessState::Running);

	const auto stopped = supervisor.stop(handle.value(), std::chrono::seconds(2));
	ASSERT_TRUE(stopped.has_value()) << (stopped.has_value() ? "" : stopped.error());

	const auto final_status = supervisor.status(handle.value());
	ASSERT_TRUE(final_status.has_value());
	EXPECT_NE(final_status.value().state, ProcessState::Running);
}

TEST(ProcessSupervisorTest, ReportsExitCode)
{
	PosixProcessSupervisor supervisor;

	const auto handle = supervisor.start({ kShell, { "-c", "exit 7" }, "", {} });
	ASSERT_TRUE(handle.has_value()) << (handle.has_value() ? "" : handle.error());

	const auto status = wait_until_finished(supervisor, handle.value(), std::chrono::seconds(5));

	EXPECT_EQ(status.state, ProcessState::Exited);
	ASSERT_TRUE(status.exit_code.has_value());
	EXPECT_EQ(status.exit_code.value(), 7);
}

TEST(ProcessSupervisorTest, MissingExecutableFailsToStart)
{
	PosixProcessSupervisor supervisor;

	const auto handle = supervisor.start({ "/nonexistent/yirang-should-not-exist", {}, "", {} });

	ASSERT_FALSE(handle.has_value());
	EXPECT_NE(handle.error().find("yirang-should-not-exist"), std::string::npos);
}

TEST(ProcessSupervisorTest, ForcesTerminationWhenGracefulIsIgnored)
{
	PosixProcessSupervisor supervisor;
	const TemporaryDirectory directory;

	const auto handle = supervisor.start({ kShell, { "-c", "trap '' TERM; : > ready; while :; do sleep 0.2; done" }, directory.path().string(), {} });
	ASSERT_TRUE(handle.has_value()) << (handle.has_value() ? "" : handle.error());
	ASSERT_TRUE(directory.wait_for("ready", std::chrono::seconds(5)));

	const auto stopped = supervisor.stop(handle.value(), std::chrono::seconds(1));
	ASSERT_TRUE(stopped.has_value()) << (stopped.has_value() ? "" : stopped.error());

	const auto status = supervisor.status(handle.value());
	ASSERT_TRUE(status.has_value());
	EXPECT_EQ(status.value().state, ProcessState::Signaled);
	ASSERT_TRUE(status.value().signal.has_value());
	EXPECT_EQ(status.value().signal.value(), SIGKILL);
}

TEST(ProcessSupervisorTest, AppliesWorkingDirectory)
{
	PosixProcessSupervisor supervisor;
	const TemporaryDirectory directory;

	const auto handle = supervisor.start({ kShell, { "-c", "pwd > pwd.txt" }, directory.path().string(), {} });
	ASSERT_TRUE(handle.has_value()) << (handle.has_value() ? "" : handle.error());

	const auto status = wait_until_finished(supervisor, handle.value(), std::chrono::seconds(5));
	ASSERT_EQ(status.state, ProcessState::Exited);

	const auto recorded = directory.read("pwd.txt");
	EXPECT_NE(recorded.find(directory.path().filename().string()), std::string::npos);
}

TEST(ProcessSupervisorTest, AppliesEnvironmentOverrides)
{
	PosixProcessSupervisor supervisor;
	const TemporaryDirectory directory;

	const auto handle
		= supervisor.start({ kShell, { "-c", "printf '%s' \"$YIRANG_RELEASE_ID\" > env.txt" }, directory.path().string(), { { "YIRANG_RELEASE_ID", "rel_230" } } });
	ASSERT_TRUE(handle.has_value()) << (handle.has_value() ? "" : handle.error());

	const auto status = wait_until_finished(supervisor, handle.value(), std::chrono::seconds(5));
	ASSERT_EQ(status.state, ProcessState::Exited);

	EXPECT_EQ(directory.read("env.txt"), "rel_230");
}
