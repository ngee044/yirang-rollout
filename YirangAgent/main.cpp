#include "AgentService.h"
#include "Configurations.h"

#include "ArgumentParser.h"
#include "Logger.h"
#include "LogTypes.h"
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
	// 신호 처리기에서는 async-signal-safe 한 조작만 한다. 종료 판단은 메인 루프가 한다.
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

		return options;
	}

	auto make_queue_options(const Configurations& configurations) -> Messaging::QueueOptions
	{
		Messaging::QueueOptions options;
		options.queue_url = configurations.queue_url();
		options.region = configurations.s3_region();
		options.endpoint = configurations.s3_endpoint();
		options.wait_time_seconds = configurations.poll_wait_seconds();

		return options;
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
		// 큐도 버킷도 없이 뜨면 아무 일도 하지 못한 채 살아 있게 된다. 기동을 실패시킨다.
		Logger::handle().write(LogTypes::Error, std::format("configuration incomplete: {}", required.error()));
		exit_code = 1;
	}
	else
	{
		// AWS SDK 초기화와 클라이언트 생성이 수 초 걸린다(콜드 스타트 실측 6초). 그 구간에 SIGTERM 이
		// 오면 기본 처리로 프로세스가 즉시 죽어 버퍼된 로그까지 유실되므로, 무엇을 만들기 전에 등록한다.
		std::signal(SIGINT, request_stop);
		std::signal(SIGTERM, request_stop);

		auto store = std::make_shared<Artifact::S3ArtifactStore>(make_store_options(*configurations));

		// 봉투의 reply_queue_url 이 결과 큐를 지정할 수 있으므로 발행자는 항상 만든다.
		// 설정과 봉투 모두 비어 있을 때만 AgentService 가 보고를 건너뛴다.
		std::shared_ptr<Messaging::IMessagePublisher> publisher = std::make_shared<Messaging::SqsMessagePublisher>(make_queue_options(*configurations));

		auto service = std::make_shared<AgentService>(make_agent_options(*configurations), store, publisher);

		Messaging::SqsMessageConsumer consumer(make_queue_options(*configurations));

		auto registered = consumer.handler(
			[service](const std::string& body) -> std::expected<void, std::string>
			{
				auto handled = service->handle(body);
				if (!handled)
				{
					// 실패를 삼키면 어떤 메시지가 왜 실패했는지 남지 않는다.
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
