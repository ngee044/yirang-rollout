#pragma once

#include "ArgumentParser.h"
#include "LogTypes.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace YirangAgent
{
	class Configurations
	{
	public:
		Configurations(const Utilities::ArgumentParser& arguments, const std::string& config_file_name = "yirang_agent_configurations.json");

		auto validate_required(void) const -> std::expected<void, std::string>;

		auto root_path(void) const -> std::string;
		auto main_title(void) const -> std::string;

		auto write_file_log(void) const -> Utilities::LogTypes;
		auto write_console_log(void) const -> Utilities::LogTypes;
		auto write_interval(void) const -> uint16_t;
		auto log_root_path(void) const -> std::string;

		auto device_id(void) const -> std::string;
		auto group(void) const -> std::string;

		auto queue_url(void) const -> std::string;
		auto result_queue_url(void) const -> std::string;

		auto poll_wait_seconds(void) const -> int;

		auto version_root(void) const -> std::string;

		auto service_root(void) const -> std::string;

		auto keep_previous_releases(void) const -> int;

		auto s3_bucket(void) const -> std::string;
		auto s3_region(void) const -> std::string;

		auto s3_endpoint(void) const -> std::string;
		auto allow_insecure_tls(void) const -> bool;

		auto service_executable(void) const -> std::string;
		auto service_arguments(void) const -> std::vector<std::string>;
		auto service_working_directory(void) const -> std::string;
		auto stop_timeout_seconds(void) const -> int;
		auto startup_timeout_seconds(void) const -> int;

		auto health_kind(void) const -> std::string;
		auto health_host(void) const -> std::string;
		auto health_port(void) const -> int;
		auto health_path(void) const -> std::string;
		auto health_expected_status(void) const -> int;
		auto health_timeout_ms(void) const -> int;
		auto health_interval_ms(void) const -> int;
		auto health_success_threshold(void) const -> int;
		auto health_failure_threshold(void) const -> int;

	private:
		auto load(void) -> void;
		auto parse(const Utilities::ArgumentParser& arguments) -> void;
		auto validate_configuration(void) -> void;

		std::string root_path_;
		std::string config_path_;
		std::string main_title_;

		int write_file_log_;
		int write_console_log_;
		int write_interval_;
		std::string log_root_path_;

		std::string device_id_;
		std::string group_;

		std::string queue_url_;
		std::string result_queue_url_;
		int poll_wait_seconds_;

		std::string version_root_;
		std::string service_root_;
		int keep_previous_releases_;

		std::string s3_bucket_;
		std::string s3_region_;
		std::string s3_endpoint_;
		bool allow_insecure_tls_;

		std::string service_executable_;
		std::vector<std::string> service_arguments_;
		std::string service_working_directory_;
		int stop_timeout_seconds_;
		int startup_timeout_seconds_;

		std::string health_kind_;
		std::string health_host_;
		int health_port_;
		std::string health_path_;
		int health_expected_status_;
		int health_timeout_ms_;
		int health_interval_ms_;
		int health_success_threshold_;
		int health_failure_threshold_;
	};
}
