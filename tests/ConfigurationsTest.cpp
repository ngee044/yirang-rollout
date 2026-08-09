#include <gtest/gtest.h>

#include "Configurations.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <random>

using namespace YirangAgent;
using namespace Utilities;

namespace
{
	// ArgumentParser 는 char* argv[] 를 받으므로 문자열 수명을 유지한 채 포인터 배열을 만든다.
	// argv[0] 은 파싱 대상이 아니지만(ArgumentParser 는 index 1부터 읽음) 자리를 채워야 한다.
	class ArgumentFixture
	{
	public:
		explicit ArgumentFixture(std::vector<std::string> values) : values_(std::move(values))
		{
			pointers_.reserve(values_.size() + 1);
			pointers_.push_back(program_.data());

			for (auto& value : values_)
			{
				pointers_.push_back(value.data());
			}
		}

		auto parser(void) -> ArgumentParser { return ArgumentParser(static_cast<int32_t>(pointers_.size()), pointers_.data()); }

	private:
		std::string program_{ "yirang-agent-test" };
		std::vector<std::string> values_;
		std::vector<char*> pointers_;
	};

	// 설정 파일을 실제 파일로 써야 Configurations 의 로드 경로가 그대로 검증된다.
	class TemporaryConfig
	{
	public:
		explicit TemporaryConfig(const std::string& contents)
		{
			static int sequence = 0;
			// 병렬 ctest 는 TEST 하나당 별도 프로세스로 돌고 sequence 가 프로세스마다 1 부터 다시
			// 시작한다. 프로세스 고유 토큰이 없으면 동시에 도는 두 케이스가 같은 경로를 공유하고,
			// 먼저 끝난 쪽의 소멸자가 아직 쓰고 있는 파일을 지운다.
			static const auto token = std::to_string(std::random_device{}());
			path_ = (std::filesystem::temp_directory_path() / std::string("yirang-agent-test-" + token + "-" + std::to_string(++sequence) + ".json")).string();

			std::ofstream stream(path_, std::ios::binary);
			stream << contents;
		}

		~TemporaryConfig()
		{
			std::error_code error;
			std::filesystem::remove(path_, error);
		}

		ArgumentFixture arguments(void) const { return ArgumentFixture({ "--config_path", path_ }); }

		auto path(void) const -> std::string { return path_; }

	private:
		std::string path_;
	};

	auto absent_config_path(void) -> std::string { return (std::filesystem::temp_directory_path() / "yirang-agent-absent.json").string(); }
}

// TC-CFG-01 — 설정 파일 부재 시 기본값으로 기동
TEST(ConfigurationsTest, MissingFileFallsBackToDefaults)
{
	const auto path = absent_config_path();
	std::error_code error;
	std::filesystem::remove(path, error);

	ArgumentFixture arguments({ "--config_path", path });
	const Configurations configurations(arguments.parser());

	EXPECT_EQ(configurations.main_title(), "yirang-agent");
	EXPECT_EQ(configurations.poll_wait_seconds(), 20);
	EXPECT_EQ(configurations.keep_previous_releases(), 2);
	EXPECT_EQ(configurations.s3_region(), "us-east-1");
	EXPECT_TRUE(configurations.queue_url().empty());
	EXPECT_TRUE(configurations.s3_bucket().empty());
}

// TC-CFG-02 — 손상 JSON·비객체 루트 입력 시 기본값 유지
TEST(ConfigurationsTest, MalformedJsonKeepsDefaults)
{
	{
		const TemporaryConfig config("{ this is not valid json");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		EXPECT_EQ(configurations.main_title(), "yirang-agent");
		EXPECT_EQ(configurations.poll_wait_seconds(), 20);
	}

	{
		const TemporaryConfig config(R"([1, 2, 3])");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		EXPECT_EQ(configurations.main_title(), "yirang-agent");
		EXPECT_EQ(configurations.keep_previous_releases(), 2);
	}
}

// TC-CFG-03 — 범위 밖 수치 클램프
TEST(ConfigurationsTest, OutOfRangeValuesAreClamped)
{
	const TemporaryConfig config(R"({
		"write_file_log": 99,
		"write_console_log": -3,
		"write_interval": 70000,
		"poll_wait_seconds": 60,
		"keep_previous_releases": -1,
		"s3_region": ""
	})");

	auto arguments = config.arguments();
	const Configurations configurations(arguments.parser());

	EXPECT_EQ(configurations.write_file_log(), LogTypes::Information);
	EXPECT_EQ(configurations.write_console_log(), LogTypes::Information);
	EXPECT_EQ(configurations.write_interval(), 1000);
	EXPECT_EQ(configurations.poll_wait_seconds(), 20);
	EXPECT_EQ(configurations.keep_previous_releases(), 2);
	EXPECT_EQ(configurations.s3_region(), "us-east-1");
}

// TC-CFG-04 — CLI 인자가 파일 값을 덮어씀
TEST(ConfigurationsTest, CommandLineArgumentsOverrideFileValues)
{
	const TemporaryConfig config(R"({
		"main_title": "from-file",
		"device_id": "from-file",
		"queue_url": "https://sqs.example/from-file",
		"s3_bucket": "bucket-from-file",
		"write_console_log": 2
	})");

	ArgumentFixture arguments({ "--config_path", config.path(), "--main_title", "from-cli", "--device_id", "pc-001", "--queue_url", "https://sqs.example/from-cli",
								"--s3_bucket", "bucket-from-cli", "--write_console_log", "5" });
	const Configurations configurations(arguments.parser());

	EXPECT_EQ(configurations.main_title(), "from-cli");
	EXPECT_EQ(configurations.device_id(), "pc-001");
	EXPECT_EQ(configurations.queue_url(), "https://sqs.example/from-cli");
	EXPECT_EQ(configurations.s3_bucket(), "bucket-from-cli");
	EXPECT_EQ(configurations.write_console_log(), LogTypes::Debug);
}

// TC-CFG-05 — 메시지를 받거나 아티팩트를 내려받을 수 없는 설정은 기동을 막는다
TEST(ConfigurationsTest, ValidateRequiredRejectsIncompleteConfiguration)
{
	{
		const TemporaryConfig config(R"({"s3_bucket": "yirang-releases"})");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		const auto result = configurations.validate_required();
		ASSERT_FALSE(result.has_value());
		EXPECT_NE(result.error().find("queue_url"), std::string::npos);
	}

	{
		const TemporaryConfig config(R"({"queue_url": "https://sqs.example/q"})");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		const auto result = configurations.validate_required();
		ASSERT_FALSE(result.has_value());
		EXPECT_NE(result.error().find("s3_bucket"), std::string::npos);
	}

	{
		const TemporaryConfig config(R"({"queue_url": "https://sqs.example/q", "s3_bucket": "yirang-releases"})");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		EXPECT_TRUE(configurations.validate_required().has_value());
	}
}

// clean_old_version 이 가동 중인 앱을 지우지 않도록 두 경로가 같으면 기동을 막는다
TEST(ConfigurationsTest, ValidateRequiredRejectsIdenticalRoots)
{
	const TemporaryConfig config(R"({
		"queue_url": "https://sqs.example/q",
		"s3_bucket": "yirang-releases",
		"version_root": "/tmp/yirang-shared",
		"service_root": "/tmp/yirang-shared"
	})");

	auto arguments = config.arguments();
	const Configurations configurations(arguments.parser());

	const auto result = configurations.validate_required();

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("service_root"), std::string::npos);
}

// TC-CFG-06 — 로그 경로 후행 구분자·main_title 구분자 치환·version_root 기본값
TEST(ConfigurationsTest, PathsAndTitleAreNormalised)
{
	{
		const TemporaryConfig config(R"({"log_root_path": "/tmp/yirang-logs", "main_title": "agent/main\\worker"})");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		EXPECT_EQ(configurations.log_root_path(), "/tmp/yirang-logs/");
		EXPECT_EQ(configurations.main_title(), "agent_main_worker");
	}

	{
		const TemporaryConfig config(R"({"log_root_path": ""})");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		EXPECT_EQ(configurations.log_root_path(), configurations.root_path());

		// version_root 가 비면 실행 파일 옆 versions/ 로 채워진다.
		EXPECT_EQ(configurations.version_root(), (std::filesystem::path(configurations.root_path()) / "versions").string());
	}
}
