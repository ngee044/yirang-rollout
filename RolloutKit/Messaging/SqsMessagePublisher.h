#pragma once

#include "IMessagePublisher.h"
#include "MessagingTypes.h"

#include <memory>

namespace AWSService
{
	class AWSSQSPublisher;
}

namespace Messaging
{
	class SqsMessagePublisher : public IMessagePublisher
	{
	public:
		explicit SqsMessagePublisher(const QueueOptions& options);
		~SqsMessagePublisher(void) override;

		auto send(const std::string& queue_url, const std::string& body) -> std::expected<void, std::string> override;

		auto options(void) const -> QueueOptions;

	private:
		QueueOptions options_;
		std::unique_ptr<AWSService::AWSSQSPublisher> publisher_;
	};
}
