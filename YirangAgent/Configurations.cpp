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
		, allow_insecure_tls_(false)
		, service_executable_("")
		, service_arguments_()
		, service_working_directory_("")
		, stop_timeout_seconds_(30)
		, startup_timeout_seconds_(60)
		, health_kind_("process")
		, health_host_("127.0.0.1")
		, health_port_(0)
		, health_path_("/")
		, health_expected_status_(200)
		, health_timeout_ms_(2000)
		, health_interval_ms_(1000)
		, health_success_threshold_(1)
		, health_failure_threshold_(3)
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

	auto Configurations::allow_insecure_tls(void) const -> bool { return allow_insecure_tls_; }

	auto Configurations::service_executable(void) const -> std::string { return service_executable_; }

	auto Configurations::service_arguments(void) const -> std::vector<std::string> { return service_arguments_; }

	auto Configurations::service_working_directory(void) const -> std::string { return service_working_directory_; }

	auto Configurations::stop_timeout_seconds(void) const -> int { return stop_timeout_seconds_; }

	auto Configurations::startup_timeout_seconds(void) const -> int { return startup_timeout_seconds_; }

	auto Configurations::health_kind(void) const -> std::string { return health_kind_; }

	auto Configurations::health_host(void) const -> std::string { return health_host_; }

	auto Configurations::health_port(void) const -> int { return health_port_; }

	auto Configurations::health_path(void) const -> std::string { return health_path_; }

	auto Configurations::health_expected_status(void) const -> int { return health_expected_status_; }

	auto Configurations::health_timeout_ms(void) const -> int { return health_timeout_ms_; }

	auto Configurations::health_interval_ms(void) const -> int { return health_interval_ms_; }

	auto Configurations::health_success_threshold(void) const -> int { return health_success_threshold_; }

	auto Configurations::health_failure_threshold(void) const -> int { return health_failure_threshold_; }

	auto Configurations::validate_required(void) const -> std::expected<void, std::string>
	{
		if (device_id_.empty())
		{
			return std::unexpected("device_id is required");
		}

		if (queue_url_.empty())
		{
			return std::unexpected("queue_url is required");
		}

		if (s3_bucket_.empty())
		{
			return std::unexpected("s3_bucket is required");
		}

		if (!result_queue_url_.empty() && result_queue_url_ == queue_url_)
		{
			return std::unexpected("result_queue_url must not be the same as queue_url");
		}

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
		read_bool("allow_insecure_tls", allow_insecure_tls_);

		auto read_nested = [&message](const char* section, auto&& reader) -> void
		{
			if (message.contains(section) && message.at(section).is_object())
			{
				reader(message.at(section).as_object());
			}
		};

		read_nested("service",
					[this](const boost::json::object& section) -> void
					{
						if (section.contains("executable") && section.at("executable").is_string())
						{
							service_executable_ = section.at("executable").as_string().c_str();
						}
						if (section.contains("working_directory") && section.at("working_directory").is_string())
						{
							service_working_directory_ = section.at("working_directory").as_string().c_str();
						}
						if (section.contains("stop_timeout_seconds") && section.at("stop_timeout_seconds").is_int64())
						{
							stop_timeout_seconds_ = (int)section.at("stop_timeout_seconds").as_int64();
						}
						if (section.contains("startup_timeout_seconds") && section.at("startup_timeout_seconds").is_int64())
						{
							startup_timeout_seconds_ = (int)section.at("startup_timeout_seconds").as_int64();
						}
						if (section.contains("arguments") && section.at("arguments").is_array())
						{
							service_arguments_.clear();
							for (const auto& element : section.at("arguments").as_array())
							{
								if (element.is_string())
								{
									service_arguments_.push_back(element.as_string().c_str());
								}
							}
						}
					});

		read_nested("health",
					[this](const boost::json::object& section) -> void
					{
						if (section.contains("kind") && section.at("kind").is_string())
						{
							health_kind_ = section.at("kind").as_string().c_str();
						}
						if (section.contains("host") && section.at("host").is_string())
						{
							health_host_ = section.at("host").as_string().c_str();
						}
						if (section.contains("path") && section.at("path").is_string())
						{
							health_path_ = section.at("path").as_string().c_str();
						}
						if (section.contains("port") && section.at("port").is_int64())
						{
							health_port_ = (int)section.at("port").as_int64();
						}
						if (section.contains("expected_status") && section.at("expected_status").is_int64())
						{
							health_expected_status_ = (int)section.at("expected_status").as_int64();
						}
						if (section.contains("timeout_ms") && section.at("timeout_ms").is_int64())
						{
							health_timeout_ms_ = (int)section.at("timeout_ms").as_int64();
						}
						if (section.contains("interval_ms") && section.at("interval_ms").is_int64())
						{
							health_interval_ms_ = (int)section.at("interval_ms").as_int64();
						}
						if (section.contains("success_threshold") && section.at("success_threshold").is_int64())
						{
							health_success_threshold_ = (int)section.at("success_threshold").as_int64();
						}
						if (section.contains("failure_threshold") && section.at("failure_threshold").is_int64())
						{
							health_failure_threshold_ = (int)section.at("failure_threshold").as_int64();
						}
					});
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

		auto bool_target = arguments.to_bool("--allow_insecure_tls");
		if (bool_target != std::nullopt)
		{
			allow_insecure_tls_ = bool_target.value();
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

		if (poll_wait_seconds_ < 0 || poll_wait_seconds_ > 20)
		{
			poll_wait_seconds_ = 20;
		}

		if (stop_timeout_seconds_ < 1)
		{
			stop_timeout_seconds_ = 30;
		}

		if (startup_timeout_seconds_ < 1)
		{
			startup_timeout_seconds_ = 60;
		}

		if (health_interval_ms_ < 1)
		{
			health_interval_ms_ = 1000;
		}

		if (health_timeout_ms_ < 1)
		{
			health_timeout_ms_ = 2000;
		}

		if (health_success_threshold_ < 1)
		{
			health_success_threshold_ = 1;
		}

		if (health_failure_threshold_ < 1)
		{
			health_failure_threshold_ = 3;
		}

		if (health_kind_ != "process" && health_kind_ != "tcp" && health_kind_ != "http")
		{
			health_kind_ = "process";
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
}
