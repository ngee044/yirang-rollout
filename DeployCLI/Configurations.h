#pragma once

#include "ArgumentParser.h"
#include "LogTypes.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace DeployCli
{
	// yirang CLI 설정. Agent 측 Configuration 모듈과 별개로, CLI 는 Control Plane REST API
	// 클라이언트로서 필요한 값만 다룬다 (yirang-rollout-architecture.md §34).
	class Configurations
	{
	public:
		Configurations(const Utilities::ArgumentParser& arguments, const std::string& config_file_name = "yirang_deploy_configurations.json");

		// 필수 설정이 비어 있으면 명령 실행을 막을 근거를 제공한다.
		auto validate_required(void) const -> std::expected<void, std::string>;

		// 설정 파일을 읽지 못했더라도 기본값으로 동작하므로 실패가 아니라 경고로 남긴다.
		auto load_warning(void) const -> std::optional<std::string>;

		auto root_path(void) const -> std::string;
		auto app_title(void) const -> std::string;

		auto control_plane_url(void) const -> std::string;
		auto api_token(void) const -> std::string;
		auto request_timeout_seconds(void) const -> int;
		auto output_format(void) const -> std::string;

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

		std::string log_root_path_;
		int write_console_log_;
		int write_file_log_;
		int write_interval_;

		bool show_help_;
		bool show_version_;

		std::optional<std::string> load_warning_;
	};
} // namespace DeployCli
