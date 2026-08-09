#include "SqsMessageConsumer.h"

#include "AWSSQSConsumer.h"

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>

using namespace AWSService;

namespace Messaging
{
	namespace
	{
		// Aws::InitAPI / ShutdownAPI 는 프로세스당 한 번이어야 한다. Artifact 모듈과 별개로 초기화되어도
		// SDK 가 참조 계수를 유지하지 않으므로, 두 모듈이 같은 지역 정적 패턴을 쓰되 각자 한 번만 호출한다.
		class AwsRuntime
		{
		public:
			AwsRuntime(void) { Aws::InitAPI(options_); }
			~AwsRuntime(void) { Aws::ShutdownAPI(options_); }

		private:
			Aws::SDKOptions options_;
		};

		auto ensure_runtime(void) -> void
		{
			static AwsRuntime runtime;
			(void)runtime;
		}

		auto make_configuration(const QueueOptions& options) -> Aws::Client::ClientConfiguration
		{
			Aws::Client::ClientConfiguration configuration;
			configuration.region = options.region.c_str();

			if (!options.endpoint.empty())
			{
				configuration.endpointOverride = options.endpoint.c_str();
				configuration.scheme = Aws::Http::Scheme::HTTP;
				configuration.verifySSL = false;
			}

			return configuration;
		}

		auto make_consumer_config(const QueueOptions& options) -> AWSSQSConsumerConfig
		{
			AWSSQSConsumerConfig config;
			config.wait_time_seconds = options.wait_time_seconds;
			config.visibility_timeout = options.visibility_timeout_seconds;
			config.max_number_of_messages = options.max_number_of_messages;

			return config;
		}
	}

	SqsMessageConsumer::SqsMessageConsumer(const QueueOptions& options) : options_(options), consumer_(nullptr), handler_registered_(false)
	{
		ensure_runtime();

		const auto configuration = make_configuration(options_);
		const auto consume_config = make_consumer_config(options_);

		if (!options_.access_key.empty())
		{
			consumer_ = std::make_unique<AWSSQSConsumer>(options_.access_key, options_.secret_key, configuration, consume_config);
		}
		else
		{
			consumer_ = std::make_unique<AWSSQSConsumer>(configuration, consume_config);
		}

		consumer_->sqs_url(options_.queue_url);
	}

	SqsMessageConsumer::~SqsMessageConsumer(void) { stop(); }

	auto SqsMessageConsumer::options(void) const -> QueueOptions { return options_; }

	auto SqsMessageConsumer::handler(Handler value) -> std::expected<void, std::string>
	{
		if (value == nullptr)
		{
			return std::unexpected("handler is null");
		}

		// 소비 스레드가 호출 중인 std::function 을 교체하면 데이터 레이스다. 등록은 start 이전만 허용한다.
		if (running())
		{
			return std::unexpected("handler cannot be replaced while running");
		}

		auto registered = consumer_->register_consume_handler(std::move(value));
		if (!registered)
		{
			return std::unexpected(registered.error());
		}

		handler_registered_ = true;

		return {};
	}

	auto SqsMessageConsumer::start(void) -> std::expected<void, std::string>
	{
		// 재호출하면 툴킷이 stop → 워커 join 으로 내려가는데 소비 루프 플래그가 내려가지 않아
		// join 이 영원히 반환하지 않는다. 진입에서 막는다.
		if (running())
		{
			return std::unexpected("consumer is already running");
		}

		if (options_.queue_url.empty())
		{
			return std::unexpected("queue_url is required");
		}

		// 핸들러 없이 소비를 시작하면 받은 메시지가 조용히 사라진다.
		if (!handler_registered_)
		{
			return std::unexpected("handler must be registered before start");
		}

		// AWSSQSBase::start() 가 워커를 만든 뒤에야 소비 루프를 걸 수 있다.
		auto started = consumer_->start();
		if (!started)
		{
			return std::unexpected(started.error());
		}

		auto consuming = consumer_->start_consume();
		if (!consuming)
		{
			consumer_->stop();

			return std::unexpected(consuming.error());
		}

		return {};
	}

	auto SqsMessageConsumer::stop(void) -> void
	{
		if (consumer_ == nullptr)
		{
			return;
		}

		// 소비 루프를 먼저 멈춰야 워커 종료 중에 새 메시지를 잡지 않는다.
		(void)consumer_->stop_consume();
		consumer_->stop();
	}

	auto SqsMessageConsumer::running(void) const -> bool { return (consumer_ != nullptr) && consumer_->is_running(); }
} // namespace Messaging
