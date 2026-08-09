#include "Configurations.h"

#include "Converter.h"
#include "File.h"
#include "Logger.h"

#include <boost/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>

using namespace Utilities;

namespace YirangAgent
{
	Configurations::Configurations(const ArgumentParser& arguments, const std::string& config_file_name)
		: root_path_("")
		, config_path_("")
		, main_title_("yirang-agent")
		, write_file_log_((int)LogTypes::Information)
		, write_console_log_((int)LogTypes::Information)
		, write_interval_(1000)
		, log_root_path_("")
		, device_id_("")
		, group_("")
		, queue_url_("")
		, result_queue_url_("")
		, poll_wait_seconds_(20)
		, version_root_("")
		, service_root_("")
		, keep_previous_releases_(2)
		, s3_bucket_("")
		, s3_region_("us-east-1")
		, s3_endpoint_("")
	{
		root_path_ = arguments.program_folder();

		auto config_path = arguments.to_string("--config_path");
		if (config_path != std::nullopt)
		{
			config_path_ = config_path.value();
		}
		else
		{
			config_path_ = (std::filesystem::path(root_path_) / config_file_name).string();
		}

		load();
		parse(arguments);
		validate_configuration();
	}

	auto Configurations::root_path(void) const -> std::string { return root_path_; }

	auto Configurations::main_title(void) const -> std::string { return main_title_; }

	auto Configurations::write_file_log(void) const -> LogTypes { return (LogTypes)write_file_log_; }

	auto Configurations::write_console_log(void) const -> LogTypes { return (LogTypes)write_console_log_; }

	auto Configurations::write_interval(void) const -> uint16_t { return (uint16_t)write_interval_; }

	auto Configurations::log_root_path(void) const -> std::string { return log_root_path_; }

	auto Configurations::device_id(void) const -> std::string { return device_id_; }

	auto Configurations::group(void) const -> std::string { return group_; }

	auto Configurations::queue_url(void) const -> std::string { return queue_url_; }

	auto Configurations::result_queue_url(void) const -> std::string { return result_queue_url_; }

	auto Configurations::poll_wait_seconds(void) const -> int { return poll_wait_seconds_; }

	auto Configurations::version_root(void) const -> std::string { return version_root_; }

	auto Configurations::service_root(void) const -> std::string { return service_root_; }

	auto Configurations::keep_previous_releases(void) const -> int { return keep_previous_releases_; }

	auto Configurations::s3_bucket(void) const -> std::string { return s3_bucket_; }

	auto Configurations::s3_region(void) const -> std::string { return s3_region_; }

	auto Configurations::s3_endpoint(void) const -> std::string { return s3_endpoint_; }

	auto Configurations::validate_required(void) const -> std::expected<void, std::string>
	{
		if (queue_url_.empty())
		{
			return std::unexpected("queue_url is required");
		}

		if (s3_bucket_.empty())
		{
			return std::unexpected("s3_bucket is required");
		}

		// version_root 는 비어 있으면 기본값으로 채워지므로 검사 대상이 아니다.
		// 다만 서비스 실행 경로와 같으면 clean_old_version 이 가동 중인 앱을 지운다.
		if (!service_root_.empty() && version_root_ == service_root_)
		{
			return std::unexpected("version_root must not be the same as service_root");
		}

		return {};
	}

	auto Configurations::load(void) -> void
	{
		File source;
		auto opened = source.open(config_path_, std::ios::in | std::ios::binary, std::locale(""));
		if (!opened)
		{
			Logger::handle().write(LogTypes::Warning, std::format("cannot open configuration file (defaults applied): {}", config_path_));
			return;
		}

		auto source_data = source.read_bytes();
		if (!source_data)
		{
			Logger::handle().write(LogTypes::Warning, std::format("cannot read configuration file (defaults applied): {}", source_data.error()));
			return;
		}

		boost::json::value parsed_value;
		try
		{
			parsed_value = boost::json::parse(Converter::to_string(source_data.value()));
		}
		catch (const std::exception& exception)
		{
			Logger::handle().write(LogTypes::Error, std::format("cannot parse configuration file (defaults applied): {}", exception.what()));
			return;
		}

		if (!parsed_value.is_object())
		{
			Logger::handle().write(LogTypes::Error, "configuration root is not a JSON object (defaults applied)");
			return;
		}

		auto message = parsed_value.as_object();

		// JSON 키와 멤버 이름을 1:1로 유지하기 위한 헬퍼 (키 불일치 실수 방지)
		auto read_string = [&message](const char* key, std::string& target) -> void
		{
			if (message.contains(key) && message.at(key).is_string())
			{
				target = message.at(key).as_string().c_str();
			}
		};
		auto read_int = [&message](const char* key, int& target) -> void
		{
			if (message.contains(key) && message.at(key).is_int64())
			{
				target = (int)message.at(key).as_int64();
			}
		};

		read_string("main_title", main_title_);
		read_int("write_file_log", write_file_log_);
		read_int("write_console_log", write_console_log_);
		read_int("write_interval", write_interval_);
		read_string("log_root_path", log_root_path_);

		read_string("device_id", device_id_);
		read_string("group", group_);

		read_string("queue_url", queue_url_);
		read_string("result_queue_url", result_queue_url_);
		read_int("poll_wait_seconds", poll_wait_seconds_);

		read_string("version_root", version_root_);
		read_string("service_root", service_root_);
		read_int("keep_previous_releases", keep_previous_releases_);

		read_string("s3_bucket", s3_bucket_);
		read_string("s3_region", s3_region_);
		read_string("s3_endpoint", s3_endpoint_);
	}

	auto Configurations::parse(const ArgumentParser& arguments) -> void
	{
		auto int_target = arguments.to_int("--write_console_log");
		if (int_target != std::nullopt)
		{
			write_console_log_ = int_target.value();
		}

		int_target = arguments.to_int("--write_file_log");
		if (int_target != std::nullopt)
		{
			write_file_log_ = int_target.value();
		}

		auto string_target = arguments.to_string("--main_title");
		if (string_target != std::nullopt)
		{
			main_title_ = string_target.value();
		}

		string_target = arguments.to_string("--device_id");
		if (string_target != std::nullopt)
		{
			device_id_ = string_target.value();
		}

		string_target = arguments.to_string("--group");
		if (string_target != std::nullopt)
		{
			group_ = string_target.value();
		}

		string_target = arguments.to_string("--queue_url");
		if (string_target != std::nullopt)
		{
			queue_url_ = string_target.value();
		}

		string_target = arguments.to_string("--version_root");
		if (string_target != std::nullopt)
		{
			version_root_ = string_target.value();
		}

		string_target = arguments.to_string("--service_root");
		if (string_target != std::nullopt)
		{
			service_root_ = string_target.value();
		}

		string_target = arguments.to_string("--s3_bucket");
		if (string_target != std::nullopt)
		{
			s3_bucket_ = string_target.value();
		}

		string_target = arguments.to_string("--s3_endpoint");
		if (string_target != std::nullopt)
		{
			s3_endpoint_ = string_target.value();
		}
	}

	auto Configurations::validate_configuration(void) -> void
	{
		// Logger는 log_root 뒤에 구분자를 붙이지 않고 파일명을 이어 붙이므로 후행 구분자를 보장한다.
		if (log_root_path_.empty())
		{
			log_root_path_ = root_path_;
		}
		else if (log_root_path_.back() != '/' && log_root_path_.back() != '\\')
		{
			log_root_path_ += '/';
		}

		// main_title은 로그 파일명 구성요소이므로 경로 구분자를 허용하지 않는다.
		std::replace(main_title_.begin(), main_title_.end(), '/', '_');
		std::replace(main_title_.begin(), main_title_.end(), '\\', '_');

		if (write_file_log_ < (int)LogTypes::None || write_file_log_ > (int)LogTypes::Packet)
		{
			write_file_log_ = (int)LogTypes::Information;
		}

		if (write_console_log_ < (int)LogTypes::None || write_console_log_ > (int)LogTypes::Packet)
		{
			write_console_log_ = (int)LogTypes::Information;
		}

		if (write_interval_ < 0 || write_interval_ > 65535)
		{
			write_interval_ = 1000;
		}

		// SQS long polling 은 최대 20초다. 범위를 넘기면 요청 자체가 거부된다.
		if (poll_wait_seconds_ < 0 || poll_wait_seconds_ > 20)
		{
			poll_wait_seconds_ = 20;
		}

		if (version_root_.empty())
		{
			version_root_ = (std::filesystem::path(root_path_) / "versions").string();
		}

		if (keep_previous_releases_ < 0)
		{
			keep_previous_releases_ = 2;
		}

		if (s3_region_.empty())
		{
			s3_region_ = "us-east-1";
		}
	}
} // namespace YirangAgent
