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
			// 병렬 ctest 는 TEST 하나당 별도 프로세스로 돌고 sequence 가 프로세스마다 1 부터 다시
			// 시작한다. 프로세스 고유 토큰이 없으면 동시에 도는 두 케이스가 같은 경로를 공유하고,
			// 먼저 끝난 쪽의 소멸자가 아직 쓰고 있는 파일을 지운다.
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

// FR-PRC-01 — 기동 후 살아 있고, 정지 요청으로 종료된다
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

// FR-PRC-02 — 정상 종료의 exit code 를 보고한다
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

// FR-PRC-01 — 실행 파일이 없으면 기동 자체가 실패로 보고된다 (exec 실패를 부모가 알아야 한다)
TEST(ProcessSupervisorTest, MissingExecutableFailsToStart)
{
	PosixProcessSupervisor supervisor;

	const auto handle = supervisor.start({ "/nonexistent/yirang-should-not-exist", {}, "", {} });

	ASSERT_FALSE(handle.has_value());
	EXPECT_NE(handle.error().find("yirang-should-not-exist"), std::string::npos);
}

// FR-PRC-01 — graceful 종료를 무시하면 타임아웃 후 강제 종료로 폴백한다
TEST(ProcessSupervisorTest, ForcesTerminationWhenGracefulIsIgnored)
{
	PosixProcessSupervisor supervisor;
	const TemporaryDirectory directory;

	// trap 설치 전에 SIGTERM 이 도착하면 기본 처리로 죽어 버린다. 준비 완료를 파일로 알리게 해
	// "graceful 을 무시하는 프로세스"라는 조건을 확정한 뒤에 정지를 요청한다.
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

// 릴리스 디렉터리에서 기동해야 하므로 작업 디렉터리 지정이 실제로 적용되어야 한다
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

// 관리 프로세스에 릴리스 정보를 넘겨야 하므로 환경변수 주입이 적용되어야 한다
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
