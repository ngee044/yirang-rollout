#include "AgentService.h"
#include "Configurations.h"

#include "ArgumentParser.h"
#include "Logger.h"
#include "LogTypes.h"
#include "PosixProcessSupervisor.h"
#include "S3ArtifactStore.h"
#include "SqsMessageConsumer.h"
#include "SqsMessagePublisher.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <format>
#include <memory>
#include <thread>

#ifndef YIRANG_AGENT_VERSION
#define YIRANG_AGENT_VERSION "0.0.0"
#endif

using namespace Utilities;
using namespace YirangAgent;

namespace
{
	std::atomic<bool> stopping{ false };

	auto request_stop(int signal_number) -> void
	{
		(void)signal_number;
		stopping.store(true);
	}

	auto make_store_options(const Configurations& configurations) -> Artifact::StoreOptions
	{
		Artifact::StoreOptions options;
		options.bucket = configurations.s3_bucket();
		options.region = configurations.s3_region();
		options.endpoint = configurations.s3_endpoint();
		options.allow_insecure_tls = configurations.allow_insecure_tls();

		return options;
	}

	auto make_queue_options(const Configurations& configurations) -> Messaging::QueueOptions
	{
		Messaging::QueueOptions options;
		options.queue_url = configurations.queue_url();
		options.region = configurations.s3_region();
		options.endpoint = configurations.s3_endpoint();
		options.allow_insecure_tls = configurations.allow_insecure_tls();
		options.wait_time_seconds = configurations.poll_wait_seconds();

		return options;
	}

	auto make_service_spec(const Configurations& configurations) -> Deploy::ServiceSpec
	{
		Deploy::ServiceSpec spec;
		spec.executable = configurations.service_executable();
		spec.arguments = configurations.service_arguments();
		spec.working_directory = configurations.service_working_directory();
		spec.stop_timeout = std::chrono::seconds(configurations.stop_timeout_seconds());
		spec.startup_timeout = std::chrono::seconds(configurations.startup_timeout_seconds());

		return spec;
	}

	auto make_health_spec(const Configurations& configurations) -> Health::HealthCheckSpec
	{
		Health::HealthCheckSpec spec;
		spec.kind = (configurations.health_kind() == "http")  ? Health::CheckKind::Http
					: (configurations.health_kind() == "tcp") ? Health::CheckKind::Tcp
															  : Health::CheckKind::Process;
		spec.host = configurations.health_host();
		spec.port = (uint16_t)configurations.health_port();
		spec.path = configurations.health_path();
		spec.expected_status = configurations.health_expected_status();
		spec.timeout = std::chrono::milliseconds(configurations.health_timeout_ms());
		spec.interval = std::chrono::milliseconds(configurations.health_interval_ms());
		spec.success_threshold = configurations.health_success_threshold();
		spec.failure_threshold = configurations.health_failure_threshold();

		return spec;
	}

	auto make_engine(const Configurations& configurations) -> std::shared_ptr<Deploy::DeploymentEngine>
	{
		if (configurations.service_root().empty() || configurations.service_executable().empty())
		{
			return nullptr;
		}

		Install::InstallOptions install_options;
		install_options.service_root = configurations.service_root();
		install_options.keep_previous_releases = configurations.keep_previous_releases();

		auto installer = std::make_shared<Install::ReleaseInstaller>(install_options);
		auto supervisor = std::make_shared<Process::PosixProcessSupervisor>();

		return std::make_shared<Deploy::DeploymentEngine>(installer, supervisor, make_service_spec(configurations), make_health_spec(configurations));
	}

	auto make_agent_options(const Configurations& configurations) -> AgentOptions
	{
		AgentOptions options;
		options.device_id = configurations.device_id();
		options.group = configurations.group();
		options.version_root = configurations.version_root();
		options.service_root = configurations.service_root();
		options.result_queue_url = configurations.result_queue_url();

		return options;
	}
}

auto main(int32_t argc, char* argv[]) -> int32_t
{
	auto configurations = std::make_shared<Configurations>(ArgumentParser(argc, argv));

	Logger::handle().file_mode(configurations->write_file_log());
	Logger::handle().console_mode(configurations->write_console_log());
	Logger::handle().write_interval(configurations->write_interval());
	Logger::handle().log_root(configurations->log_root_path());
	Logger::handle().start(configurations->main_title());

	Logger::handle().write(LogTypes::Information, std::format("yirang-agent {} starting", YIRANG_AGENT_VERSION));

	auto exit_code = 0;

	auto required = configurations->validate_required();
	if (!required)
	{
		Logger::handle().write(LogTypes::Error, std::format("configuration incomplete: {}", required.error()));
		exit_code = 1;
	}
	else
	{
		std::signal(SIGINT, request_stop);
		std::signal(SIGTERM, request_stop);

		auto store = std::make_shared<Artifact::S3ArtifactStore>(make_store_options(*configurations));

		std::shared_ptr<Messaging::IMessagePublisher> publisher = std::make_shared<Messaging::SqsMessagePublisher>(make_queue_options(*configurations));

		auto engine = make_engine(*configurations);
		if (engine == nullptr)
		{
			Logger::handle().write(LogTypes::Warning, "service_root or service.executable is not configured — apply_version and rollback_version will fail");
		}

		auto service = std::make_shared<AgentService>(make_agent_options(*configurations), store, publisher, engine);

		if (engine != nullptr)
		{
			auto resumed = engine->start_active();
			if (resumed)
			{
				Logger::handle().write(LogTypes::Information, std::format("resumed the active release: {}", engine->last_detail()));
			}
			else if (resumed.error() == Deploy::kNoActiveRelease)
			{
				Logger::handle().write(LogTypes::Information, "no release is active yet — waiting for a deployment command");
			}
			else
			{
				Logger::handle().write(LogTypes::Error, std::format("cannot resume the active release: {}", resumed.error()));
			}
		}

		Messaging::SqsMessageConsumer consumer(make_queue_options(*configurations));

		auto registered = consumer.handler(
			[service](const std::string& body) -> std::expected<void, std::string>
			{
				auto handled = service->handle(body);
				if (!handled)
				{
					Logger::handle().write(LogTypes::Error, std::format("message handling failed: {}", handled.error()));
				}

				return handled;
			});

		if (!registered)
		{
			Logger::handle().write(LogTypes::Error, std::format("cannot register message handler: {}", registered.error()));
			exit_code = 1;
		}
		else
		{
			auto started = consumer.start();
			if (!started)
			{
				Logger::handle().write(LogTypes::Error, std::format("cannot start consumer: {}", started.error()));
				exit_code = 1;
			}
			else
			{
				Logger::handle().write(LogTypes::Information, std::format("consuming '{}' as device '{}' (group '{}')", configurations->queue_url(),
																		  configurations->device_id(), configurations->group()));

				while (!stopping.load())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
				}

				Logger::handle().write(LogTypes::Information, "stop requested");
				consumer.stop();
			}
		}
	}

	Logger::handle().write(LogTypes::Information, std::format("{} stopped", configurations->main_title()));

	configurations.reset();

	Logger::handle().stop();
	Logger::destroy();

	return exit_code;
}
