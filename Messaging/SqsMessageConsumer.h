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
	// CppToolkit AWSService::AWSSQSConsumer 를 감싸는 구현.
	// SDK 헤더가 새지 않도록 소비자를 전방 선언 + unique_ptr 로 감춘다.
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

		// start() 는 handler 등록을 전제로 한다. 등록 없이 소비를 시작하면 메시지가 조용히 버려진다.
		bool handler_registered_;
	};
} // namespace Messaging
