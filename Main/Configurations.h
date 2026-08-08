#pragma once

#include "ArgumentParser.h"
#include "LogTypes.h"

#include <expected>
#include <string>

class Configurations
{
public:
	Configurations(const Utilities::ArgumentParser& arguments);

	// 필수 설정이 비어 있으면 기동을 실패시킬 근거를 제공한다. Agent 제어 루프(MVP 1)가
	// 구현되기 전까지는 호출 측에서 경고로만 처리한다 (yirang-rollout-architecture.md §48.1)
	auto validate_required(void) const -> std::expected<void, std::string>;

	auto root_path(void) const -> std::string;
	auto main_title(void) const -> std::string;

	auto write_file_log(void) const -> Utilities::LogTypes;
	auto write_console_log(void) const -> Utilities::LogTypes;
	auto write_interval(void) const -> uint16_t;
	auto log_root_path(void) const -> std::string;

	auto control_plane_url(void) const -> std::string;
	auto heartbeat_seconds(void) const -> int;
	auto reconnect_max_seconds(void) const -> int;

	auto data_directory(void) const -> std::string;
	auto artifact_cache_gb(void) const -> int;
	auto keep_previous_releases(void) const -> int;

	auto verify_release_signature(void) const -> bool;

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

	std::string control_plane_url_;
	int heartbeat_seconds_;
	int reconnect_max_seconds_;

	std::string data_directory_;
	int artifact_cache_gb_;
	int keep_previous_releases_;

	bool verify_release_signature_;
};
