#include "SqsMessageConsumer.h"

#include "AWSSQSConsumer.h"

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>

using namespace AWSService;

namespace Messaging
{
	namespace
	{
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
			}

			if (options.allow_insecure_tls)
			{
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
		if (running())
		{
			return std::unexpected("consumer is already running");
		}

		if (options_.queue_url.empty())
		{
			return std::unexpected("queue_url is required");
		}

		if (!handler_registered_)
		{
			return std::unexpected("handler must be registered before start");
		}

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

		(void)consumer_->stop_consume();
		consumer_->stop();
	}

	auto SqsMessageConsumer::running(void) const -> bool { return (consumer_ != nullptr) && consumer_->is_running(); }
}
