#include "AgentService.h"

#include "ArtifactKey.h"
#include "Logger.h"
#include "ReleaseManifest.h"

#include <boost/json.hpp>

#include <filesystem>
#include <thread>
#include <format>

using namespace Utilities;

namespace YirangAgent
{
	namespace
	{
		auto read_string(const boost::json::object& object, const char* key) -> std::expected<std::string, std::string>
		{
			if (!object.contains(key) || !object.at(key).is_string())
			{
				return std::unexpected(std::format("payload field '{}' is missing or not a string", key));
			}

			return std::string(object.at(key).as_string().c_str());
		}

		auto parse_object(const std::string& payload) -> std::expected<boost::json::object, std::string>
		{
			boost::json::value parsed;
			try
			{
				parsed = boost::json::parse(payload);
			}
			catch (const std::exception& exception)
			{
				return std::unexpected(std::format("cannot parse payload: {}", exception.what()));
			}

			if (!parsed.is_object())
			{
				return std::unexpected("payload is not a JSON object");
			}

			return parsed.as_object();
		}

		class PartialDownloadGuard
		{
		public:
			explicit PartialDownloadGuard(std::string directory) : directory_(std::move(directory)), keep_(false) {}

			~PartialDownloadGuard(void)
			{
				if (keep_)
				{
					return;
				}

				std::error_code ignored;
				std::filesystem::remove_all(directory_, ignored);
			}

			PartialDownloadGuard(const PartialDownloadGuard&) = delete;
			auto operator=(const PartialDownloadGuard&) -> PartialDownloadGuard& = delete;

			auto keep(void) -> void { keep_ = true; }

		private:
			std::string directory_;
			bool keep_;
		};

		auto retry_is_pointless(const std::string& command) -> bool { return command == Commands::kApplyVersion || command == Commands::kRollbackVersion; }
	}

	AgentService::AgentService(const AgentOptions& options,
							   std::shared_ptr<Artifact::IArtifactStore> store,
							   std::shared_ptr<Messaging::IMessagePublisher> publisher,
							   std::shared_ptr<Deploy::DeploymentEngine> engine)
		: options_(options), store_(std::move(store)), publisher_(std::move(publisher)), engine_(std::move(engine)), messages_(), last_report_()
	{
		messages_.insert({ Commands::kDownloadVersion, std::bind(&AgentService::download_version, this, std::placeholders::_1) });
		messages_.insert({ Commands::kApplyVersion, std::bind(&AgentService::apply_version, this, std::placeholders::_1) });
		messages_.insert({ Commands::kCurrentStatus, std::bind(&AgentService::current_status, this, std::placeholders::_1) });
		messages_.insert({ Commands::kCleanOldVersion, std::bind(&AgentService::clean_old_version, this, std::placeholders::_1) });
		messages_.insert({ Commands::kRollbackVersion, std::bind(&AgentService::rollback_version, this, std::placeholders::_1) });
	}

	auto AgentService::commands(void) const -> std::vector<std::string>
	{
		std::vector<std::string> names;
		names.reserve(messages_.size());

		for (const auto& [name, handler] : messages_)
		{
			names.push_back(name);
		}

		return names;
	}

	auto AgentService::last_report(void) const -> std::string { return last_report_; }

	auto AgentService::handle(const std::string& raw_message) -> std::expected<void, std::string>
	{
		auto parsed = parse_agent_message(raw_message);
		if (!parsed)
		{
			Logger::handle().write(LogTypes::Error, std::format("cannot parse agent message: {}", parsed.error()));
			report(std::string(), std::string(), std::expected<void, std::string>(std::unexpect, parsed.error()));

			return {};
		}

		const auto& message = parsed.value();

		auto iter = messages_.find(message.command);
		if (iter == messages_.end())
		{
			Logger::handle().write(LogTypes::Error, std::format("command is not found: {}", message.command));
			report(message.command, message.reply_queue_url, std::expected<void, std::string>(std::unexpect, std::format("command is not found: {}", message.command)));

			return {};
		}

		auto outcome = iter->second(message.payload);
		report(message.command, message.reply_queue_url, outcome);

		if (!outcome && retry_is_pointless(message.command))
		{
			Logger::handle().write(LogTypes::Error, std::format("'{}' failed and will not be retried: {}", message.command, outcome.error()));

			return {};
		}

		return outcome;
	}

	auto AgentService::report(const std::string& command, const std::string& reply_queue_url, const std::expected<void, std::string>& outcome) -> void
	{
		const auto& destination = reply_queue_url.empty() ? options_.result_queue_url : reply_queue_url;

		if (publisher_ == nullptr || destination.empty())
		{
			return;
		}

		boost::json::object body;
		body["device_id"] = options_.device_id;
		body["group"] = options_.group;
		body["command"] = command;
		body["success"] = outcome.has_value();
		body["detail"] = outcome.has_value() ? last_report_ : outcome.error();

		auto sent = publisher_->send(destination, boost::json::serialize(body));
		if (!sent)
		{
			Logger::handle().write(LogTypes::Error, std::format("cannot report result for '{}': {}", command, sent.error()));
		}
	}

	auto AgentService::version_directory(const std::string& release_id) const -> std::string
	{
		return (std::filesystem::path(options_.version_root) / release_id).string();
	}

	auto AgentService::download_version(const std::string& message) -> std::expected<void, std::string>
	{
		if (store_ == nullptr)
		{
			return std::unexpected("artifact store is not configured");
		}

		if (options_.version_root.empty())
		{
			return std::unexpected("version_root is not configured");
		}

		auto object = parse_object(message);
		if (!object)
		{
			return std::unexpected(object.error());
		}

		auto release_id = read_string(object.value(), "release_id");
		if (!release_id)
		{
			return std::unexpected(release_id.error());
		}

		if (!object.value().contains("artifacts") || !object.value().at("artifacts").is_array())
		{
			return std::unexpected("payload field 'artifacts' is missing or not an array");
		}

		if (object.value().at("artifacts").as_array().empty())
		{
			return std::unexpected("payload field 'artifacts' must not be empty");
		}

		const auto target = version_directory(release_id.value());

		std::error_code error;
		std::filesystem::create_directories(target, error);
		if (error)
		{
			return std::unexpected(std::format("cannot create version directory '{}': {}", target, error.message()));
		}

		auto discard_on_failure = PartialDownloadGuard{ target };

		for (const auto& element : object.value().at("artifacts").as_array())
		{
			if (!element.is_object())
			{
				return std::unexpected("artifact entry is not an object");
			}

			const auto entry = element.as_object();

			auto install_path = read_string(entry, "install_path");
			if (!install_path)
			{
				return std::unexpected(install_path.error());
			}

			auto sha256 = read_string(entry, "sha256");
			if (!sha256)
			{
				return std::unexpected(sha256.error());
			}

			auto key = Artifact::make_object_key(release_id.value(), install_path.value());
			if (!key)
			{
				return std::unexpected(key.error());
			}

			const auto destination = (std::filesystem::path(target) / install_path.value()).string();

			auto downloaded = store_->download(key.value(), destination);
			if (!downloaded)
			{
				return std::unexpected(downloaded.error());
			}

			auto actual = Release::ReleaseManifest::file_sha256(destination);
			if (!actual)
			{
				return std::unexpected(actual.error());
			}

			if (actual.value() != sha256.value())
			{
				return std::unexpected(std::format("sha256 mismatch for '{}': expected {}, got {}", install_path.value(), sha256.value(), actual.value()));
			}
		}

		discard_on_failure.keep();

		last_report_ = std::format("downloaded release '{}' into '{}'", release_id.value(), target);
		Logger::handle().write(LogTypes::Information, last_report_);

		return {};
	}

	auto AgentService::clean_old_version(const std::string& message) -> std::expected<void, std::string>
	{
		(void)message;

		if (options_.version_root.empty())
		{
			return std::unexpected("version_root is not configured");
		}

		std::error_code error;
		if (!std::filesystem::is_directory(options_.version_root, error))
		{
			last_report_ = std::format("version_root '{}' is not a directory — nothing to clean", options_.version_root);

			return {};
		}

		if (!options_.service_root.empty())
		{
			const auto version_root = std::filesystem::weakly_canonical(options_.version_root, error);
			const auto service_root = std::filesystem::weakly_canonical(options_.service_root, error);

			if (version_root == service_root)
			{
				return std::unexpected("version_root must not be the same as service_root");
			}
		}

		std::filesystem::directory_iterator entry(options_.version_root, error);
		if (error)
		{
			return std::unexpected(std::format("cannot list '{}': {}", options_.version_root, error.message()));
		}

		uint32_t removed = 0;
		const std::filesystem::directory_iterator last;
		while (entry != last)
		{
			std::error_code remove_error;
			std::filesystem::remove_all(entry->path(), remove_error);
			if (remove_error)
			{
				return std::unexpected(std::format("cannot remove '{}': {}", entry->path().string(), remove_error.message()));
			}

			++removed;

			entry.increment(error);
			if (error)
			{
				return std::unexpected(std::format("cannot walk '{}': {}", options_.version_root, error.message()));
			}
		}

		last_report_ = std::format("removed {} version directories from '{}'", removed, options_.version_root);
		Logger::handle().write(LogTypes::Information, last_report_);

		return {};
	}

	auto AgentService::current_status(const std::string& message) -> std::expected<void, std::string>
	{
		(void)message;

		std::error_code error;

		std::filesystem::path target = options_.version_root.empty() ? std::filesystem::current_path(error) : std::filesystem::path(options_.version_root);
		while (!target.empty() && !std::filesystem::exists(target, error) && target.has_parent_path() && target.parent_path() != target)
		{
			target = target.parent_path();
		}

		if (target.empty() || !std::filesystem::exists(target, error))
		{
			target = std::filesystem::current_path(error);
		}

		const auto space = std::filesystem::space(target, error);
		if (error)
		{
			return std::unexpected(std::format("cannot query disk space for '{}': {}", target.string(), error.message()));
		}

		boost::json::object report;
		report["device_id"] = options_.device_id;
		report["group"] = options_.group;
		report["version_root"] = options_.version_root;
		report["disk_probe_path"] = target.string();
		report["disk_capacity_bytes"] = static_cast<uint64_t>(space.capacity);
		report["disk_free_bytes"] = static_cast<uint64_t>(space.free);
		report["disk_available_bytes"] = static_cast<uint64_t>(space.available);
		report["hardware_concurrency"] = static_cast<uint64_t>(std::thread::hardware_concurrency());

		boost::json::array versions;
		if (!options_.version_root.empty() && std::filesystem::is_directory(options_.version_root, error))
		{
			std::error_code walk_error;
			std::filesystem::directory_iterator entry(options_.version_root, walk_error);

			const std::filesystem::directory_iterator last;
			while (!walk_error && entry != last)
			{
				if (entry->is_directory(error))
				{
					versions.push_back(boost::json::value(entry->path().filename().string()));
				}

				entry.increment(walk_error);
			}

			if (walk_error)
			{
				report["downloaded_versions_error"] = walk_error.message();
			}
		}
		report["downloaded_versions"] = versions;

		last_report_ = boost::json::serialize(report);
		Logger::handle().write(LogTypes::Information, last_report_);

		return {};
	}

	auto AgentService::apply_version(const std::string& message) -> std::expected<void, std::string>
	{
		auto object = parse_object(message);
		if (!object)
		{
			return std::unexpected(object.error());
		}

		auto release_id = read_string(object.value(), "release_id");
		if (!release_id)
		{
			return std::unexpected(release_id.error());
		}

		if (engine_ == nullptr)
		{
			return std::unexpected("deployment engine is not configured (service_root or service.executable is missing)");
		}

		const auto source = version_directory(release_id.value());

		std::error_code error;
		if (!std::filesystem::is_directory(source, error))
		{
			return std::unexpected(std::format("version '{}' is not downloaded (expected '{}')", release_id.value(), source));
		}

		auto applied = engine_->apply(release_id.value(), source);
		if (!applied)
		{
			return std::unexpected(applied.error());
		}

		last_report_ = engine_->last_detail();
		Logger::handle().write(LogTypes::Information, last_report_);

		return {};
	}

	auto AgentService::rollback_version(const std::string& message) -> std::expected<void, std::string>
	{
		auto object = parse_object(message);
		if (!object)
		{
			return std::unexpected(object.error());
		}

		auto release_id = read_string(object.value(), "release_id");
		if (!release_id)
		{
			return std::unexpected(release_id.error());
		}

		if (engine_ == nullptr)
		{
			return std::unexpected("deployment engine is not configured (service_root or service.executable is missing)");
		}

		auto reverted = engine_->rollback_to(release_id.value());
		if (!reverted)
		{
			return std::unexpected(reverted.error());
		}

		last_report_ = engine_->last_detail();
		Logger::handle().write(LogTypes::Information, last_report_);

		return {};
	}
}
