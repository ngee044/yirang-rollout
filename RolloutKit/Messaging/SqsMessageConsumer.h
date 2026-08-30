#pragma once

#include "IMessageConsumer.h"
#include "MessagingTypes.h"

#include <memory>

namespace AWSService
{
	class AWSSQSConsumer;
}

namespace Messaging
{
	class SqsMessageConsumer : public IMessageConsumer
	{
	public:
		explicit SqsMessageConsumer(const QueueOptions& options);
		~SqsMessageConsumer(void) override;

		auto handler(Handler value) -> std::expected<void, std::string> override;

		auto start(void) -> std::expected<void, std::string> override;
		auto stop(void) -> void override;

		auto running(void) const -> bool override;

		auto options(void) const -> QueueOptions;

	private:
		QueueOptions options_;
		std::unique_ptr<AWSService::AWSSQSConsumer> consumer_;

		bool handler_registered_;
	};
}
