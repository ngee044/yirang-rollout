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
	// CppToolkit AWSService::AWSSQSPublisher 를 감싸는 구현.
	// SDK 헤더가 새지 않도록 발행자를 전방 선언 + unique_ptr 로 감춘다.
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
} // namespace Messaging
