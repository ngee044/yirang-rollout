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

	class TemporaryConfig
	{
	public:
		explicit TemporaryConfig(const std::string& contents)
		{
			static int sequence = 0;
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

TEST(ConfigurationsTest, ValidateRequiredRejectsIncompleteConfiguration)
{
	{
		const TemporaryConfig config(R"({"device_id": "pc-001", "s3_bucket": "yirang-releases"})");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		const auto result = configurations.validate_required();
		ASSERT_FALSE(result.has_value());
		EXPECT_NE(result.error().find("queue_url"), std::string::npos);
	}

	{
		const TemporaryConfig config(R"({"device_id": "pc-001", "queue_url": "https://sqs.example/q"})");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		const auto result = configurations.validate_required();
		ASSERT_FALSE(result.has_value());
		EXPECT_NE(result.error().find("s3_bucket"), std::string::npos);
	}

	{
		const TemporaryConfig config(R"({"device_id": "pc-001", "queue_url": "https://sqs.example/q", "s3_bucket": "yirang-releases"})");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		EXPECT_TRUE(configurations.validate_required().has_value());
	}
}

TEST(ConfigurationsTest, ValidateRequiredRejectsIdenticalRoots)
{
	const TemporaryConfig config(R"({
		"device_id": "pc-001",
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

		EXPECT_EQ(configurations.version_root(), (std::filesystem::path(configurations.root_path()) / "versions").string());
	}
}

TEST(ConfigurationsTest, NestedServiceAndHealthSectionsAreLoaded)
{
	TemporaryConfig config(R"({
		"queue_url": "https://sqs.example/q",
		"s3_bucket": "b",
		"version_root": "/tmp/yr-v",
		"service_root": "/tmp/yr-s",
		"service": {
			"executable": "bin/app",
			"arguments": ["--flag", "value"],
			"working_directory": "/tmp/wd",
			"stop_timeout_seconds": 7,
			"startup_timeout_seconds": 11
		},
		"health": {
			"kind": "http",
			"host": "10.0.0.1",
			"port": 9090,
			"path": "/ready",
			"expected_status": 204,
			"timeout_ms": 1234,
			"interval_ms": 200,
			"success_threshold": 2,
			"failure_threshold": 5
		}
	})");

	auto arguments = config.arguments();
	const Configurations configurations(arguments.parser());

	EXPECT_EQ(configurations.service_executable(), "bin/app");
	ASSERT_EQ(configurations.service_arguments().size(), 2u);
	EXPECT_EQ(configurations.service_arguments().at(0), "--flag");
	EXPECT_EQ(configurations.service_working_directory(), "/tmp/wd");
	EXPECT_EQ(configurations.stop_timeout_seconds(), 7);
	EXPECT_EQ(configurations.startup_timeout_seconds(), 11);

	EXPECT_EQ(configurations.health_kind(), "http");
	EXPECT_EQ(configurations.health_host(), "10.0.0.1");
	EXPECT_EQ(configurations.health_port(), 9090);
	EXPECT_EQ(configurations.health_path(), "/ready");
	EXPECT_EQ(configurations.health_expected_status(), 204);
	EXPECT_EQ(configurations.health_timeout_ms(), 1234);
	EXPECT_EQ(configurations.health_interval_ms(), 200);
	EXPECT_EQ(configurations.health_success_threshold(), 2);
	EXPECT_EQ(configurations.health_failure_threshold(), 5);
}

TEST(ConfigurationsTest, InsecureTlsDefaultsToFalseAndIsOptIn)
{
	{
		ArgumentFixture arguments({ "--config_path", absent_config_path() });
		const Configurations configurations(arguments.parser());

		EXPECT_FALSE(configurations.allow_insecure_tls());
	}

	{
		const TemporaryConfig config(R"({"allow_insecure_tls": true})");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		EXPECT_TRUE(configurations.allow_insecure_tls());
	}

	{
		const TemporaryConfig config(R"({"allow_insecure_tls": "true"})");
		auto arguments = config.arguments();
		const Configurations configurations(arguments.parser());

		EXPECT_FALSE(configurations.allow_insecure_tls());
	}

	{
		const TemporaryConfig config(R"({"allow_insecure_tls": true})");
		ArgumentFixture arguments({ "--config_path", config.path(), "--allow_insecure_tls", "false" });
		const Configurations configurations(arguments.parser());

		EXPECT_FALSE(configurations.allow_insecure_tls());
	}
}

TEST(ConfigurationsTest, ValidateRequiredRejectsAMissingDeviceId)
{
	const TemporaryConfig config(R"({"queue_url": "https://sqs.example/q", "s3_bucket": "yirang-releases"})");
	auto arguments = config.arguments();
	const Configurations configurations(arguments.parser());

	const auto result = configurations.validate_required();

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("device_id"), std::string::npos) << result.error();
}

TEST(ConfigurationsTest, ValidateRequiredRejectsAResultQueueThatIsTheCommandQueue)
{
	const TemporaryConfig config(R"({
		"device_id": "pc-001",
		"queue_url": "https://sqs.example/q",
		"result_queue_url": "https://sqs.example/q",
		"s3_bucket": "yirang-releases"
	})");

	auto arguments = config.arguments();
	const Configurations configurations(arguments.parser());

	const auto result = configurations.validate_required();

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("result_queue_url"), std::string::npos) << result.error();
}
