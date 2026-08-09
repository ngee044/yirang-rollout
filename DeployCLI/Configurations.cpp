#include "Configurations.h"

#include "Converter.h"
#include "File.h"

#include <boost/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>

using namespace Utilities;

namespace DeployCli
{
	Configurations::Configurations(const ArgumentParser& arguments, const std::string& config_file_name)
		: root_path_("")
		, config_path_("")
		, app_title_("yirang")
		, control_plane_url_("")
		, api_token_("")
		, request_timeout_seconds_(30)
		, output_format_("table")
		, log_root_path_("")
		, write_console_log_((int)LogTypes::Information)
		, write_file_log_((int)LogTypes::Information)
		, write_interval_(1000)
		, show_help_(false)
		, show_version_(false)
		, load_warning_(std::nullopt)
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

	auto Configurations::app_title(void) const -> std::string { return app_title_; }

	auto Configurations::control_plane_url(void) const -> std::string { return control_plane_url_; }

	auto Configurations::api_token(void) const -> std::string { return api_token_; }

	auto Configurations::request_timeout_seconds(void) const -> int { return request_timeout_seconds_; }

	auto Configurations::output_format(void) const -> std::string { return output_format_; }

	auto Configurations::log_root_path(void) const -> std::string { return log_root_path_; }

	auto Configurations::write_console_log(void) const -> LogTypes { return (LogTypes)write_console_log_; }

	auto Configurations::write_file_log(void) const -> LogTypes { return (LogTypes)write_file_log_; }

	auto Configurations::write_interval(void) const -> uint16_t { return (uint16_t)write_interval_; }

	auto Configurations::show_help(void) const -> bool { return show_help_; }

	auto Configurations::show_version(void) const -> bool { return show_version_; }

	auto Configurations::load_warning(void) const -> std::optional<std::string> { return load_warning_; }

	auto Configurations::validate_required(void) const -> std::expected<void, std::string>
	{
		if (control_plane_url_.empty())
		{
			return std::unexpected("control_plane_url is required (set it in the configuration file or pass --control_plane_url)");
		}

		return {};
	}

	auto Configurations::load(void) -> void
	{
		// CLI 는 로거 기동 전에 설정을 읽으므로 Logger 대신 load_warning_ 으로 사유를 전달한다.
		File source;

		// File::open 은 읽기 모드에서도 부모 디렉터리를 만들며 error_code 없는 create_directories 를
		// 쓰므로 권한 없는 --config_path 하나로 filesystem_error 를 던진다. std::locale("") 도 설치되지
		// 않은 로캘이면 던진다. 둘 다 Logger 기동 전이라 진단 없이 abort 되므로 여기서 반환값으로 바꾼다.
		std::expected<void, std::string> opened;
		try
		{
			opened = source.open(config_path_, std::ios::in | std::ios::binary, std::locale::classic());
		}
		catch (const std::exception& exception)
		{
			load_warning_ = std::format("cannot open configuration file (defaults applied): {}", exception.what());
			return;
		}

		if (!opened)
		{
			load_warning_ = std::format("cannot open configuration file (defaults applied): {}", config_path_);
			return;
		}

		auto source_data = source.read_bytes();
		if (!source_data)
		{
			load_warning_ = std::format("cannot read configuration file (defaults applied): {}", source_data.error());
			return;
		}

		boost::json::value parsed_value;
		try
		{
			parsed_value = boost::json::parse(Converter::to_string(source_data.value()));
		}
		catch (const std::exception& exception)
		{
			load_warning_ = std::format("cannot parse configuration file (defaults applied): {}", exception.what());
			return;
		}

		if (!parsed_value.is_object())
		{
			load_warning_ = "configuration root is not a JSON object (defaults applied)";
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

		read_string("app_title", app_title_);
		read_string("control_plane_url", control_plane_url_);
		read_string("api_token", api_token_);
		read_int("request_timeout_seconds", request_timeout_seconds_);
		read_string("output_format", output_format_);

		read_string("log_root_path", log_root_path_);
		read_int("write_console_log", write_console_log_);
		read_int("write_file_log", write_file_log_);
		read_int("write_interval", write_interval_);
	}

	auto Configurations::parse(const ArgumentParser& arguments) -> void
	{
		auto string_target = arguments.to_string("--control_plane_url");
		if (string_target != std::nullopt)
		{
			control_plane_url_ = string_target.value();
		}

		string_target = arguments.to_string("--api_token");
		if (string_target != std::nullopt)
		{
			api_token_ = string_target.value();
		}

		string_target = arguments.to_string("--output_format");
		if (string_target != std::nullopt)
		{
			output_format_ = string_target.value();
		}

		string_target = arguments.to_string("--log_root_path");
		if (string_target != std::nullopt)
		{
			log_root_path_ = string_target.value();
		}

		auto int_target = arguments.to_int("--request_timeout_seconds");
		if (int_target != std::nullopt)
		{
			request_timeout_seconds_ = int_target.value();
		}

		int_target = arguments.to_int("--write_console_log");
		if (int_target != std::nullopt)
		{
			write_console_log_ = int_target.value();
		}

		int_target = arguments.to_int("--write_file_log");
		if (int_target != std::nullopt)
		{
			write_file_log_ = int_target.value();
		}

		int_target = arguments.to_int("--write_interval");
		if (int_target != std::nullopt)
		{
			write_interval_ = int_target.value();
		}

		show_help_ = arguments.to_string("--help") != std::nullopt;
		show_version_ = arguments.to_string("--version") != std::nullopt;
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

		// app_title은 로그 파일명 구성요소이므로 경로 구분자를 허용하지 않는다.
		std::replace(app_title_.begin(), app_title_.end(), '/', '_');
		std::replace(app_title_.begin(), app_title_.end(), '\\', '_');

		if (write_console_log_ < (int)LogTypes::None || write_console_log_ > (int)LogTypes::Packet)
		{
			write_console_log_ = (int)LogTypes::Information;
		}

		if (write_file_log_ < (int)LogTypes::None || write_file_log_ > (int)LogTypes::Packet)
		{
			write_file_log_ = (int)LogTypes::Information;
		}

		if (write_interval_ < 0 || write_interval_ > 65535)
		{
			write_interval_ = 1000;
		}

		if (request_timeout_seconds_ <= 0)
		{
			request_timeout_seconds_ = 30;
		}

		if (output_format_ != "table" && output_format_ != "json")
		{
			output_format_ = "table";
		}
	}
} // namespace DeployCli
