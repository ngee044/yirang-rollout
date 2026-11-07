#include "Commands.h"

#include "ArtifactKey.h"
#include "Logger.h"
#include "ReleaseManifest.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <sstream>

using namespace Utilities;

namespace DeployCli
{
	namespace
	{
		class TemporaryManifest
		{
		public:
			TemporaryManifest(void) = default;
			TemporaryManifest(const TemporaryManifest&) = delete;
			auto operator=(const TemporaryManifest&) -> TemporaryManifest& = delete;

			~TemporaryManifest(void)
			{
				if (!directory_.empty())
				{
					std::error_code ignored;
					std::filesystem::remove_all(directory_, ignored);
				}
			}

			auto prepare(void) -> std::expected<void, std::string>
			{
				std::error_code error;
				const auto root = std::filesystem::temp_directory_path(error);
				if (error)
				{
					return std::unexpected(std::format("cannot locate temporary directory: {}", error.message()));
				}

				auto pattern = (root / "yirang-manifest-XXXXXX").string();
				if (::mkdtemp(pattern.data()) == nullptr)
				{
					return std::unexpected(std::format("cannot create temporary manifest directory: {}", std::strerror(errno)));
				}

				directory_ = pattern;
				return {};
			}

			auto path(void) const -> std::string { return (directory_ / "manifest.json").string(); }

		private:
			std::filesystem::path directory_;
		};

		const std::vector<std::string> agent_commands{ "download_version", "apply_version",		   "current_status",  "clean_old_version",
													   "rollback_version", "update_configuration", "restart_service", "reboot_device" };

		auto is_agent_command(const std::string& name) -> bool { return std::find(agent_commands.begin(), agent_commands.end(), name) != agent_commands.end(); }

		const std::vector<std::string> destructive_commands{ "clean_old_version", "reboot_device" };

		auto is_destructive_command(const std::string& name) -> bool
		{
			return std::find(destructive_commands.begin(), destructive_commands.end(), name) != destructive_commands.end();
		}

		auto scope_of(const std::string& group) -> std::string
		{
			if (group.empty())
			{
				return "every registered device";
			}

			return std::format("group '{}'", group);
		}

		auto parse_object(const std::string& text, const char* label = "response") -> std::expected<boost::json::object, std::string>
		{
			boost::json::value parsed;
			try
			{
				parsed = boost::json::parse(text);
			}
			catch (const std::exception& exception)
			{
				return std::unexpected(std::format("{} is not valid JSON: {}", label, exception.what()));
			}

			if (!parsed.is_object())
			{
				return std::unexpected(std::format("{} root is not a JSON object", label));
			}

			return parsed.as_object();
		}

		auto text_of(const boost::json::object& object, const char* key) -> std::string
		{
			if (!object.contains(key))
			{
				return {};
			}

			const auto& value = object.at(key);
			if (value.is_string())
			{
				return value.as_string().c_str();
			}

			return boost::json::serialize(value);
		}

		auto failure_reason(long status_code, const std::string& body) -> std::string
		{
			auto object = parse_object(body);
			if (object && object.value().contains("error") && object.value().at("error").is_object())
			{
				const auto error = object.value().at("error").as_object();

				return std::format("{} {} — {}", status_code, text_of(error, "code"), text_of(error, "message"));
			}

			return std::format("{} — {}", status_code, body);
		}

		auto validate_publication(const std::string& body) -> std::expected<void, std::string>
		{
			auto object = parse_object(body);
			if (!object)
			{
				return std::unexpected(object.error());
			}

			const auto* success = object->if_contains("success");
			const auto* data = object->if_contains("data");
			if (success == nullptr || !success->is_bool() || !success->as_bool() || data == nullptr || !data->is_object())
			{
				return std::unexpected("invalid publication response: success and data are required");
			}

			const auto* targeted = data->as_object().if_contains("targeted");
			const auto* published = data->as_object().if_contains("published");
			const auto* deliveries = data->as_object().if_contains("deliveries");
			if (targeted == nullptr || !targeted->is_int64() || targeted->as_int64() <= 0 || published == nullptr || !published->is_int64() || published->as_int64() < 0
				|| published->as_int64() > targeted->as_int64() || deliveries == nullptr || !deliveries->is_array()
				|| deliveries->as_array().size() != static_cast<uint64_t>(targeted->as_int64()))
			{
				return std::unexpected("invalid publication response: inconsistent delivery counts");
			}

			int64_t successful = 0;
			for (const auto& delivery : deliveries->as_array())
			{
				if (!delivery.is_object())
				{
					return std::unexpected("invalid publication response: delivery is not an object");
				}
				const auto* error = delivery.as_object().if_contains("error");
				if (error != nullptr && !error->is_string())
				{
					return std::unexpected("invalid publication response: delivery error is not a string");
				}
				if (error == nullptr || error->as_string().empty())
				{
					++successful;
				}
			}

			if (successful != published->as_int64())
			{
				return std::unexpected("invalid publication response: published count disagrees with deliveries");
			}
			if (successful != targeted->as_int64())
			{
				return std::unexpected(std::format("publication failed for {} of {} device(s)", targeted->as_int64() - successful, targeted->as_int64()));
			}

			return {};
		}

		auto download_payload(const Release::Manifest& manifest) -> boost::json::object
		{
			boost::json::array artifacts;
			for (const auto& entry : manifest.artifacts)
			{
				boost::json::object element;
				element["install_path"] = entry.install_path;
				element["sha256"] = entry.sha256;
				element["size_bytes"] = entry.size_bytes;
				artifacts.push_back(element);
			}

			boost::json::object payload;
			payload["release_id"] = manifest.release_id;
			payload["artifacts"] = artifacts;
			return payload;
		}
	}

	Commands::Commands(const Configurations& configurations,
					   std::shared_ptr<Artifact::IArtifactStore> store,
					   std::shared_ptr<RestClient> client,
					   std::shared_ptr<Confirmation> confirmation)
		: configurations_(configurations), store_(std::move(store)), client_(std::move(client)), confirmation_(std::move(confirmation)), handlers_(), last_output_()
	{
		handlers_.insert({ "deploy", std::bind(&Commands::deploy, this, std::placeholders::_1) });
		handlers_.insert({ "command", std::bind(&Commands::command, this, std::placeholders::_1) });
		handlers_.insert({ "results", std::bind(&Commands::results, this, std::placeholders::_1) });
	}

	auto Commands::names(void) const -> std::vector<std::string>
	{
		std::vector<std::string> registered;
		registered.reserve(handlers_.size());

		for (const auto& [name, handler] : handlers_)
		{
			registered.push_back(name);
		}

		return registered;
	}

	auto Commands::last_output(void) const -> std::string { return last_output_; }

	auto Commands::run(const std::string& name, const std::vector<std::string>& arguments) -> std::expected<void, std::string>
	{
		auto iter = handlers_.find(name);
		if (iter == handlers_.end())
		{
			return std::unexpected(std::format("unknown command '{}'", name));
		}

		return iter->second(arguments);
	}

	auto Commands::deploy(const std::vector<std::string>& arguments) -> std::expected<void, std::string>
	{
		(void)arguments;

		auto ready = configurations_.validate_for_deploy();
		if (!ready)
		{
			return std::unexpected(ready.error());
		}

		if (store_ == nullptr)
		{
			return std::unexpected("artifact store is not configured");
		}

		const auto now = std::chrono::system_clock::now();
		auto generated_id = Release::ReleaseManifest::make_release_id(now);
		if (!generated_id)
		{
			return std::unexpected(generated_id.error());
		}
		const auto& release_id = generated_id.value();

		auto manifest = Release::ReleaseManifest::build(configurations_.upload_file_list(), release_id, now);
		if (!manifest)
		{
			return std::unexpected(manifest.error());
		}

		Logger::handle().write(LogTypes::Information, std::format("release '{}' — {} file(s) to upload", release_id, manifest.value().artifacts.size()));

		auto uploaded = upload_release(manifest.value());
		if (!uploaded)
		{
			return std::unexpected(uploaded.error());
		}

		auto request = download_payload(manifest.value());
		request["group"] = configurations_.target_group();

		auto response = client_->post("/api/v1/deployments", boost::json::serialize(request));
		if (!response)
		{
			return std::unexpected(response.error());
		}

		if (!response.value().ok())
		{
			return std::unexpected(std::format("deployment request rejected: {}", failure_reason(response.value().status_code, response.value().body)));
		}

		present(std::format("deployed {}", release_id), response.value().body);

		return validate_publication(response.value().body);
	}

	auto Commands::command(const std::vector<std::string>& arguments) -> std::expected<void, std::string>
	{
		auto ready = configurations_.validate_required();
		if (!ready)
		{
			return std::unexpected(ready.error());
		}

		if (arguments.empty())
		{
			return std::unexpected(std::format("command name is required (one of {})", boost::json::serialize(boost::json::value_from(agent_commands))));
		}

		const auto& name = arguments.front();
		if (!is_agent_command(name))
		{
			return std::unexpected(std::format("'{}' is not an agent command (one of {})", name, boost::json::serialize(boost::json::value_from(agent_commands))));
		}

		if (is_destructive_command(name))
		{
			const auto warning = configurations_.load_warning();
			if (warning != std::nullopt && configurations_.target_group().empty())
			{
				return std::unexpected(
					std::format("'{}' was not published — the configuration was not loaded ({}) and no target group was given, so the scope would be every "
								"registered device by default. pass --target_group <name>",
								name, warning.value()));
			}

			if (confirmation_ == nullptr)
			{
				return std::unexpected(std::format("'{}' was not published — confirmation is not available", name));
			}

			auto confirmed = confirmation_->require(name, scope_of(configurations_.target_group()));
			if (!confirmed)
			{
				return std::unexpected(confirmed.error());
			}
		}

		boost::json::object payload;
		if (name == "download_version")
		{
			if (arguments.size() != 2)
			{
				return std::unexpected("download_version requires a release_id (yirang command download_version <release_id>)");
			}

			auto manifest = download_manifest(arguments.at(1));
			if (!manifest)
			{
				return std::unexpected(manifest.error());
			}
			payload = download_payload(manifest.value());
		}
		else if (name == "update_configuration")
		{
			if (arguments.size() < 2)
			{
				return std::unexpected("update_configuration requires a configuration path relative to the release directory (yirang command update_configuration "
									   "config/app.json --config_values '{\"key\":1}')");
			}

			const auto& values = configurations_.config_values();
			if (values.empty())
			{
				return std::unexpected("update_configuration requires --config_values <json object>");
			}

			auto parsed = parse_object(values, "--config_values");
			if (!parsed)
			{
				return std::unexpected(parsed.error());
			}

			payload["path"] = arguments.at(1);
			payload["values"] = parsed.value();
		}
		else if (arguments.size() > 1)
		{
			payload["release_id"] = arguments.at(1);
		}

		boost::json::object request;
		request["command"] = name;
		request["group"] = configurations_.target_group();
		request["payload"] = payload;

		auto response = client_->post("/api/v1/commands", boost::json::serialize(request));
		if (!response)
		{
			return std::unexpected(response.error());
		}

		if (!response.value().ok())
		{
			return std::unexpected(std::format("command rejected: {}", failure_reason(response.value().status_code, response.value().body)));
		}

		present(std::format("published {}", name), response.value().body);

		return validate_publication(response.value().body);
	}

	auto Commands::results(const std::vector<std::string>& arguments) -> std::expected<void, std::string>
	{
		(void)arguments;

		auto ready = configurations_.validate_required();
		if (!ready)
		{
			return std::unexpected(ready.error());
		}

		auto response = client_->get("/api/v1/results");
		if (!response)
		{
			return std::unexpected(response.error());
		}

		if (!response.value().ok())
		{
			return std::unexpected(std::format("cannot read results: {}", failure_reason(response.value().status_code, response.value().body)));
		}

		present("results", response.value().body);

		return {};
	}

	auto Commands::upload_release(const Release::Manifest& manifest) -> std::expected<void, std::string>
	{
		for (const auto& entry : manifest.artifacts)
		{
			auto key = Artifact::make_object_key(manifest.release_id, entry.install_path);
			if (!key)
			{
				return std::unexpected(key.error());
			}

			auto sent = store_->upload(key.value(), entry.source_path);
			if (!sent)
			{
				return std::unexpected(std::format("cannot upload '{}' to '{}': {}", entry.source_path, key.value(), sent.error()));
			}

			Logger::handle().write(LogTypes::Information, std::format("uploaded {} -> {}", entry.install_path, key.value()));
		}

		TemporaryManifest temporary;
		auto prepared = temporary.prepare();
		if (!prepared)
		{
			return std::unexpected(prepared.error());
		}

		{
			std::ofstream sink(temporary.path(), std::ios::binary | std::ios::trunc);
			sink << Release::ReleaseManifest::serialize(manifest);
			sink.close();
			if (!sink)
			{
				return std::unexpected("cannot write the release manifest");
			}
		}

		auto sent = store_->upload(std::format("manifests/{}.json", manifest.release_id), temporary.path());
		if (!sent)
		{
			return std::unexpected(std::format("cannot upload manifest for '{}': {}", manifest.release_id, sent.error()));
		}
		return {};
	}

	auto Commands::download_manifest(const std::string& release_id) -> std::expected<Release::Manifest, std::string>
	{
		auto valid = Artifact::validate_release_id(release_id);
		if (!valid)
		{
			return std::unexpected(valid.error());
		}
		if (store_ == nullptr || configurations_.s3_bucket().empty())
		{
			return std::unexpected("download_version requires an artifact store and --s3_bucket");
		}

		TemporaryManifest temporary;
		auto prepared = temporary.prepare();
		if (!prepared)
		{
			return std::unexpected(prepared.error());
		}

		auto downloaded = store_->download(std::format("manifests/{}.json", release_id), temporary.path());
		if (!downloaded)
		{
			return std::unexpected(std::format("cannot download manifest for '{}': {}", release_id, downloaded.error()));
		}

		std::ifstream source(temporary.path(), std::ios::binary);
		if (!source.is_open())
		{
			return std::unexpected("cannot open the downloaded manifest");
		}
		std::ostringstream buffer;
		buffer << source.rdbuf();
		if (source.bad())
		{
			return std::unexpected("cannot read the downloaded manifest");
		}

		auto manifest = Release::ReleaseManifest::parse(buffer.str());
		if (!manifest)
		{
			return std::unexpected(manifest.error());
		}
		if (manifest->release_id != release_id)
		{
			return std::unexpected("downloaded manifest release_id does not match the requested release");
		}
		if (manifest->artifacts.empty())
		{
			return std::unexpected("downloaded manifest artifacts must not be empty");
		}
		for (const auto& entry : manifest->artifacts)
		{
			auto key = Artifact::make_object_key(release_id, entry.install_path);
			if (!key)
			{
				return std::unexpected(key.error());
			}
		}

		return manifest;
	}

	auto Commands::present(const std::string& title, const std::string& json_body) -> void
	{
		if (configurations_.output_format() == "json")
		{
			emit(json_body);

			return;
		}

		auto object = parse_object(json_body);
		if (!object)
		{
			emit(json_body);

			return;
		}

		std::string rendered = std::format("{}\n", title);

		const auto& root = object.value();
		if (!root.contains("data") || !root.at("data").is_object())
		{
			emit(json_body);

			return;
		}

		const auto data = root.at("data").as_object();

		if (data.contains("deliveries") && data.at("deliveries").is_array())
		{
			rendered += std::format("  command   {}\n", text_of(data, "command"));
			rendered += std::format("  targeted  {}\n", text_of(data, "targeted"));
			rendered += std::format("  published {}\n", text_of(data, "published"));

			for (const auto& element : data.at("deliveries").as_array())
			{
				if (!element.is_object())
				{
					continue;
				}

				const auto delivery = element.as_object();
				const auto error = text_of(delivery, "error");

				rendered += std::format("  {:<7} {}{}\n", error.empty() ? "ok" : "FAILED", text_of(delivery, "queue"), error.empty() ? "" : std::format(" — {}", error));
			}

			emit(rendered);

			return;
		}

		if (data.contains("reports") && data.at("reports").is_array())
		{
			rendered += std::format("  count     {}\n", text_of(data, "count"));

			for (const auto& element : data.at("reports").as_array())
			{
				if (!element.is_object())
				{
					continue;
				}

				const auto report = element.as_object();
				const auto success = report.contains("success") && report.at("success").is_bool() && report.at("success").as_bool();

				rendered += std::format("  {:<7} {:<20} {:<18} {}\n", success ? "ok" : "FAILED", text_of(report, "device_id"), text_of(report, "command"),
										text_of(report, "detail"));
			}

			emit(rendered);

			return;
		}

		emit(json_body);
	}

	auto Commands::emit(const std::string& text) -> void
	{
		last_output_ = text;

		std::cout << text;
		if (!text.empty() && text.back() != '\n')
		{
			std::cout << '\n';
		}

		std::cout.flush();
	}
}
