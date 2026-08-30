#include "SqsMessagePublisher.h"

#include "AWSSQSPublisher.h"

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>

#include <format>

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
	}

	SqsMessagePublisher::SqsMessagePublisher(const QueueOptions& options) : options_(options), publisher_(nullptr)
	{
		ensure_runtime();

		const auto configuration = make_configuration(options_);

		if (!options_.access_key.empty())
		{
			publisher_ = std::make_unique<AWSSQSPublisher>(options_.access_key, options_.secret_key, configuration);
		}
		else
		{
			publisher_ = std::make_unique<AWSSQSPublisher>(configuration);
		}
	}

	SqsMessagePublisher::~SqsMessagePublisher(void) = default;

	auto SqsMessagePublisher::options(void) const -> QueueOptions { return options_; }

	auto SqsMessagePublisher::send(const std::string& queue_url, const std::string& body) -> std::expected<void, std::string>
	{
		if (queue_url.empty())
		{
			return std::unexpected("queue_url is required");
		}

		if (body.empty())
		{
			return std::unexpected("message body is empty");
		}

		auto sent = publisher_->send_message_to(Aws::String(queue_url.c_str()), Aws::String(body.c_str()), Aws::String(""));
		if (!sent)
		{
			return std::unexpected(std::format("cannot send to '{}': {}", queue_url, sent.error()));
		}

		return {};
	}
}
