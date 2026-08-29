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
	namespace
	{
		auto split(const std::string& text, char delimiter) -> std::vector<std::string>
		{
			std::vector<std::string> pieces;

			std::string::size_type start = 0;
			while (start <= text.size())
			{
				const auto end = text.find(delimiter, start);
				pieces.push_back(text.substr(start, (end == std::string::npos) ? std::string::npos : end - start));

				if (end == std::string::npos)
				{
					break;
				}

				start = end + 1;
			}

			return pieces;
		}
	}

	Configurations::Configurations(const ArgumentParser& arguments, const std::string& config_file_name)
		: root_path_("")
		, config_path_("")
		, app_title_("yirang")
		, control_plane_url_("")
		, api_token_("")
		, request_timeout_seconds_(30)
		, output_format_("table")
		, upload_file_list_()
		, target_group_("")
		, s3_bucket_("")
		, s3_region_("us-east-1")
		, s3_endpoint_("")
		, allow_insecure_tls_(false)
		, log_root_path_("")
		, write_console_log_((int)LogTypes::Information)
		, write_file_log_((int)LogTypes::Information)
		, write_interval_(1000)
		, show_help_(false)
		, show_version_(false)
		, confirm_token_("")
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

	auto Configurations::upload_file_list(void) const -> std::vector<std::string> { return upload_file_list_; }

	auto Configurations::target_group(void) const -> std::string { return target_group_; }

	auto Configurations::s3_bucket(void) const -> std::string { return s3_bucket_; }

	auto Configurations::s3_region(void) const -> std::string { return s3_region_; }

	auto Configurations::s3_endpoint(void) const -> std::string { return s3_endpoint_; }

	auto Configurations::allow_insecure_tls(void) const -> bool { return allow_insecure_tls_; }

	auto Configurations::log_root_path(void) const -> std::string { return log_root_path_; }

	auto Configurations::write_console_log(void) const -> LogTypes { return (LogTypes)write_console_log_; }

	auto Configurations::write_file_log(void) const -> LogTypes { return (LogTypes)write_file_log_; }

	auto Configurations::write_interval(void) const -> uint16_t { return (uint16_t)write_interval_; }

	auto Configurations::show_help(void) const -> bool { return show_help_; }

	auto Configurations::show_version(void) const -> bool { return show_version_; }

	auto Configurations::confirm_token(void) const -> std::string { return confirm_token_; }

	auto Configurations::load_warning(void) const -> std::optional<std::string> { return load_warning_; }

	auto Configurations::validate_required(void) const -> std::expected<void, std::string>
	{
		if (control_plane_url_.empty())
		{
			return std::unexpected("control_plane_url is required (set it in the configuration file or pass --control_plane_url)");
		}

		return {};
	}

	auto Configurations::validate_for_deploy(void) const -> std::expected<void, std::string>
	{
		auto required = validate_required();
		if (!required)
		{
			return required;
		}

		if (upload_file_list_.empty())
		{
			return std::unexpected("upload_file_list is empty (set it in the configuration file or pass --upload_file_list a.exe,b.dll)");
		}

		if (s3_bucket_.empty())
		{
			return std::unexpected("s3_bucket is required for deploy (set it in the configuration file or pass --s3_bucket)");
		}

		return {};
	}

	auto Configurations::load(void) -> void
	{
		File source;

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

		auto read_string_array = [&message](const char* key, std::vector<std::string>& target) -> void
		{
			if (!message.contains(key) || !message.at(key).is_array())
			{
				return;
			}

			target.clear();
			for (const auto& element : message.at(key).as_array())
			{
				if (element.is_string())
				{
					target.push_back(element.as_string().c_str());
				}
			}
		};

		read_string("app_title", app_title_);
		read_string("control_plane_url", control_plane_url_);
		read_string("api_token", api_token_);
		read_int("request_timeout_seconds", request_timeout_seconds_);
		read_string("output_format", output_format_);

		read_string_array("upload_file_list", upload_file_list_);
		read_string("target_group", target_group_);
		read_string("s3_bucket", s3_bucket_);
		read_string("s3_region", s3_region_);
		read_string("s3_endpoint", s3_endpoint_);
		read_bool("allow_insecure_tls", allow_insecure_tls_);

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

		string_target = arguments.to_string("--target_group");
		if (string_target != std::nullopt)
		{
			target_group_ = string_target.value();
		}

		string_target = arguments.to_string("--s3_bucket");
		if (string_target != std::nullopt)
		{
			s3_bucket_ = string_target.value();
		}

		string_target = arguments.to_string("--s3_region");
		if (string_target != std::nullopt)
		{
			s3_region_ = string_target.value();
		}

		string_target = arguments.to_string("--s3_endpoint");
		if (string_target != std::nullopt)
		{
			s3_endpoint_ = string_target.value();
		}

		auto bool_target = arguments.to_bool("--allow_insecure_tls");
		if (bool_target != std::nullopt)
		{
			allow_insecure_tls_ = bool_target.value();
		}

		auto upload_target = arguments.to_string("--upload_file_list");
		if (upload_target != std::nullopt)
		{
			upload_file_list_.clear();
			for (const auto& element : split(upload_target.value(), ','))
			{
				if (!element.empty())
				{
					upload_file_list_.push_back(element);
				}
			}
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

		auto confirm_target = arguments.to_string("--confirm");
		if (confirm_target != std::nullopt)
		{
			confirm_token_ = confirm_target.value();
		}
	}

	auto Configurations::validate_configuration(void) -> void
	{
		if (log_root_path_.empty())
		{
			log_root_path_ = root_path_;
		}
		else if (log_root_path_.back() != '/' && log_root_path_.back() != '\\')
		{
			log_root_path_ += '/';
		}

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
}
