#include "Commands.h"

#include "ArtifactKey.h"
#include "Logger.h"
#include "ReleaseManifest.h"

#include <boost/json.hpp>

#include <chrono>
#include <format>
#include <iostream>

using namespace Utilities;

namespace DeployCli
{
	namespace
	{
		const std::vector<std::string> agent_commands{ "download_version", "apply_version", "current_status", "clean_old_version", "rollback_version" };

		auto is_agent_command(const std::string& name) -> bool { return std::find(agent_commands.begin(), agent_commands.end(), name) != agent_commands.end(); }

		auto parse_object(const std::string& text) -> std::expected<boost::json::object, std::string>
		{
			boost::json::value parsed;
			try
			{
				parsed = boost::json::parse(text);
			}
			catch (const std::exception& exception)
			{
				return std::unexpected(std::format("response is not valid JSON: {}", exception.what()));
			}

			if (!parsed.is_object())
			{
				return std::unexpected("response root is not a JSON object");
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
	}

	Commands::Commands(const Configurations& configurations, std::shared_ptr<Artifact::IArtifactStore> store, std::shared_ptr<RestClient> client)
		: configurations_(configurations), store_(std::move(store)), client_(std::move(client)), handlers_(), last_output_()
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
		const auto release_id = Release::ReleaseManifest::make_release_id(now);

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

		boost::json::array artifacts;
		for (const auto& entry : manifest.value().artifacts)
		{
			boost::json::object element;
			element["install_path"] = entry.install_path;
			element["sha256"] = entry.sha256;
			element["size_bytes"] = entry.size_bytes;

			artifacts.push_back(element);
		}

		boost::json::object request;
		request["release_id"] = manifest.value().release_id;
		request["group"] = configurations_.target_group();
		request["artifacts"] = artifacts;

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

		return {};
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

		boost::json::object payload;
		if (arguments.size() > 1)
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

		return {};
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

		return {};
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
