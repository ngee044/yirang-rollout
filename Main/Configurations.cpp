#include "Configurations.h"

#include "Converter.h"
#include "File.h"
#include "Logger.h"

#include <boost/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>

using namespace Utilities;

Configurations::Configurations(const ArgumentParser& arguments)
	: root_path_("")
	, config_path_("")
	, main_title_("yirang-rollout")
	, write_file_log_((int)LogTypes::Information)
	, write_console_log_((int)LogTypes::Information)
	, write_interval_(1000)
	, log_root_path_("")
	, control_plane_url_("")
	, heartbeat_seconds_(15)
	, reconnect_max_seconds_(30)
	, data_directory_("")
	, artifact_cache_gb_(5)
	, keep_previous_releases_(2)
	, verify_release_signature_(true)
{
	root_path_ = arguments.program_folder();

	auto config_path = arguments.to_string("--config_path");
	if (config_path != std::nullopt)
	{
		config_path_ = config_path.value();
	}
	else
	{
		config_path_ = (std::filesystem::path(root_path_) / "Configurations.json").string();
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

auto Configurations::control_plane_url(void) const -> std::string { return control_plane_url_; }

auto Configurations::heartbeat_seconds(void) const -> int { return heartbeat_seconds_; }

auto Configurations::reconnect_max_seconds(void) const -> int { return reconnect_max_seconds_; }

auto Configurations::data_directory(void) const -> std::string { return data_directory_; }

auto Configurations::artifact_cache_gb(void) const -> int { return artifact_cache_gb_; }

auto Configurations::keep_previous_releases(void) const -> int { return keep_previous_releases_; }

auto Configurations::verify_release_signature(void) const -> bool { return verify_release_signature_; }

auto Configurations::validate_required(void) const -> std::expected<void, std::string>
{
	if (control_plane_url_.empty())
	{
		return std::unexpected("control_plane_url is required");
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
	auto read_bool = [&message](const char* key, bool& target) -> void
	{
		if (message.contains(key) && message.at(key).is_bool())
		{
			target = message.at(key).as_bool();
		}
	};

	read_string("main_title", main_title_);
	read_int("write_file_log", write_file_log_);
	read_int("write_console_log", write_console_log_);
	read_int("write_interval", write_interval_);
	read_string("log_root_path", log_root_path_);

	read_string("control_plane_url", control_plane_url_);
	read_int("heartbeat_seconds", heartbeat_seconds_);
	read_int("reconnect_max_seconds", reconnect_max_seconds_);

	read_string("data_directory", data_directory_);
	read_int("artifact_cache_gb", artifact_cache_gb_);
	read_int("keep_previous_releases", keep_previous_releases_);

	read_bool("verify_release_signature", verify_release_signature_);
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

	string_target = arguments.to_string("--control_plane_url");
	if (string_target != std::nullopt)
	{
		control_plane_url_ = string_target.value();
	}

	string_target = arguments.to_string("--data_directory");
	if (string_target != std::nullopt)
	{
		data_directory_ = string_target.value();
	}
}

auto Configurations::validate_configuration(void) -> void
{
	// Logger는 log_root 뒤에 구분자를 붙이지 않고 파일명을 이어 붙이므로(Logger.cpp의
	// std::format("{}{}_...") 계약), 사용자 지정 경로의 후행 구분자를 보장한다
	if (log_root_path_.empty())
	{
		log_root_path_ = root_path_;
	}
	else if (log_root_path_.back() != '/' && log_root_path_.back() != '\\')
	{
		log_root_path_ += '/';
	}

	// main_title은 로그 파일명 구성요소이므로 경로 구분자를 허용하지 않는다
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

	if (data_directory_.empty())
	{
		data_directory_ = (std::filesystem::path(root_path_) / "data").string();
	}

	if (heartbeat_seconds_ <= 0)
	{
		heartbeat_seconds_ = 15;
	}

	if (reconnect_max_seconds_ <= 0)
	{
		reconnect_max_seconds_ = 30;
	}

	if (artifact_cache_gb_ <= 0)
	{
		artifact_cache_gb_ = 5;
	}

	if (keep_previous_releases_ < 0)
	{
		keep_previous_releases_ = 2;
	}
}
