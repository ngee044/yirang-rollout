#pragma once

#include "ArgumentParser.h"
#include "LogTypes.h"

#include <cstdint>
#include <expected>
#include <string>

namespace YirangAgent
{
	class Configurations
	{
	public:
		Configurations(const Utilities::ArgumentParser& arguments, const std::string& config_file_name = "yirang_agent_configurations.json");

		// 이 값들이 없으면 메시지를 받지도, 아티팩트를 내려받지도 못한다. 기동을 막을 근거를 제공한다.
		auto validate_required(void) const -> std::expected<void, std::string>;

		auto root_path(void) const -> std::string;
		auto main_title(void) const -> std::string;

		auto write_file_log(void) const -> Utilities::LogTypes;
		auto write_console_log(void) const -> Utilities::LogTypes;
		auto write_interval(void) const -> uint16_t;
		auto log_root_path(void) const -> std::string;

		// 기기 식별과 대상 분류. 배포 티켓의 group 과 대조해 자기 대상인지 판정한다.
		auto device_id(void) const -> std::string;
		auto group(void) const -> std::string;

		// 이 기기가 소비할 SQS 큐. 결과 큐가 비어 있으면 결과를 보고하지 않는다.
		auto queue_url(void) const -> std::string;
		auto result_queue_url(void) const -> std::string;

		// SQS long polling 대기 시간 (0~20초).
		auto poll_wait_seconds(void) const -> int;

		// 전달받은 버전들이 <version_root>/<release_id>/ 로 쌓이는 곳.
		auto version_root(void) const -> std::string;

		// 서비스로 동작 중인 앱의 경로. version_root 와 반드시 달라야 한다.
		auto service_root(void) const -> std::string;

		auto keep_previous_releases(void) const -> int;

		auto s3_bucket(void) const -> std::string;
		auto s3_region(void) const -> std::string;

		// 비어 있으면 실제 AWS. 채워지면 MinIO·LocalStack 으로 붙는다.
		auto s3_endpoint(void) const -> std::string;

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
	};
} // namespace YirangAgent
