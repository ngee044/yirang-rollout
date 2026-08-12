#pragma once

#include "ArgumentParser.h"
#include "LogTypes.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace DeployCli
{
	class Configurations
	{
	public:
		Configurations(const Utilities::ArgumentParser& arguments, const std::string& config_file_name = "yirang_deploy_configurations.json");

		auto validate_required(void) const -> std::expected<void, std::string>;

		auto validate_for_deploy(void) const -> std::expected<void, std::string>;

		auto load_warning(void) const -> std::optional<std::string>;

		auto root_path(void) const -> std::string;
		auto app_title(void) const -> std::string;

		auto control_plane_url(void) const -> std::string;
		auto api_token(void) const -> std::string;
		auto request_timeout_seconds(void) const -> int;
		auto output_format(void) const -> std::string;

		auto upload_file_list(void) const -> std::vector<std::string>;

		auto target_group(void) const -> std::string;

		auto s3_bucket(void) const -> std::string;
		auto s3_region(void) const -> std::string;

		auto s3_endpoint(void) const -> std::string;

		auto log_root_path(void) const -> std::string;
		auto write_console_log(void) const -> Utilities::LogTypes;
		auto write_file_log(void) const -> Utilities::LogTypes;
		auto write_interval(void) const -> uint16_t;

		auto show_help(void) const -> bool;
		auto show_version(void) const -> bool;

	private:
		auto load(void) -> void;
		auto parse(const Utilities::ArgumentParser& arguments) -> void;
		auto validate_configuration(void) -> void;

		std::string root_path_;
		std::string config_path_;
		std::string app_title_;

		std::string control_plane_url_;
		std::string api_token_;
		int request_timeout_seconds_;
		std::string output_format_;

		std::vector<std::string> upload_file_list_;
		std::string target_group_;

		std::string s3_bucket_;
		std::string s3_region_;
		std::string s3_endpoint_;

		std::string log_root_path_;
		int write_console_log_;
		int write_file_log_;
		int write_interval_;

		bool show_help_;
		bool show_version_;

		std::optional<std::string> load_warning_;
	};
}
