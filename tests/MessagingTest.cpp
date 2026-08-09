#include <gtest/gtest.h>

#include "SqsMessageConsumer.h"
#include "SqsMessagePublisher.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

using namespace Messaging;

namespace
{
	// LocalStack 이 없는 환경에서 실패하지 않도록 통합 테스트는 환경변수로 켠다.
	//   YIRANG_TEST_SQS_ENDPOINT=http://localhost:4566 YIRANG_TEST_SQS_QUEUE_URL=... ctest
	auto environment(const char* name) -> std::string
	{
		const char* value = std::getenv(name);

		return (value == nullptr) ? std::string() : std::string(value);
	}

	auto local_options(void) -> QueueOptions
	{
		QueueOptions options;
		// 닿지 않는 주소를 써서 네트워크 호출 전에 걸리는 검증만 확인한다.
		options.endpoint = "http://127.0.0.1:1";
		options.access_key = "test";
		options.secret_key = "test";

		return options;
	}
}

// 핸들러 없이 소비를 시작하면 받은 메시지가 조용히 사라진다
TEST(MessagingTest, ConsumerRefusesToStartWithoutHandler)
{
	auto options = local_options();
	options.queue_url = "http://127.0.0.1:1/000000000000/yirang-test";

	SqsMessageConsumer consumer(options);

	const auto result = consumer.start();

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("handler"), std::string::npos);
	EXPECT_FALSE(consumer.running());
}

TEST(MessagingTest, ConsumerRequiresQueueUrl)
{
	SqsMessageConsumer consumer(local_options());

	const auto registered = consumer.handler([](const std::string&) -> std::expected<void, std::string> { return {}; });
	ASSERT_TRUE(registered.has_value()) << (registered.has_value() ? "" : registered.error());

	const auto result = consumer.start();

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("queue_url"), std::string::npos);
}

TEST(MessagingTest, ConsumerRejectsNullHandler)
{
	SqsMessageConsumer consumer(local_options());

	const auto result = consumer.handler(nullptr);

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("null"), std::string::npos);
}

// 소비를 시작한 적 없어도 stop() 이 안전해야 한다 (main 의 종료 경로가 항상 부른다)
TEST(MessagingTest, ConsumerStopIsSafeBeforeStart)
{
	SqsMessageConsumer consumer(local_options());

	EXPECT_NO_THROW(consumer.stop());
	EXPECT_FALSE(consumer.running());
}

TEST(MessagingTest, PublisherRejectsEmptyArguments)
{
	SqsMessagePublisher publisher(local_options());

	const auto no_queue = publisher.send("", "{}");
	ASSERT_FALSE(no_queue.has_value());
	EXPECT_NE(no_queue.error().find("queue_url"), std::string::npos);

	const auto no_body = publisher.send("http://127.0.0.1:1/q", "");
	ASSERT_FALSE(no_body.has_value());
	EXPECT_NE(no_body.error().find("body"), std::string::npos);
}

TEST(MessagingTest, OptionsAreRetained)
{
	auto options = local_options();
	options.queue_url = "http://127.0.0.1:1/q";
	options.wait_time_seconds = 5;
	options.visibility_timeout_seconds = 60;

	const SqsMessageConsumer consumer(options);

	EXPECT_EQ(consumer.options().queue_url, options.queue_url);
	EXPECT_EQ(consumer.options().wait_time_seconds, 5);
	EXPECT_EQ(consumer.options().visibility_timeout_seconds, 60);
}

// 발행 → 소비 왕복. LocalStack 이 있을 때만 실행된다.
TEST(MessagingIntegrationTest, PublishedMessageReachesConsumer)
{
	const auto endpoint = environment("YIRANG_TEST_SQS_ENDPOINT");
	const auto queue_url = environment("YIRANG_TEST_SQS_QUEUE_URL");

	if (endpoint.empty() || queue_url.empty())
	{
		GTEST_SKIP() << "YIRANG_TEST_SQS_ENDPOINT / YIRANG_TEST_SQS_QUEUE_URL 이 없어 건너뜁니다 (LocalStack 필요)";
	}

	QueueOptions options;
	options.endpoint = endpoint;
	options.queue_url = queue_url;
	options.access_key = "test";
	options.secret_key = "test";
	options.wait_time_seconds = 5;

	std::string received;
	SqsMessageConsumer consumer(options);
	ASSERT_TRUE(consumer
					.handler(
						[&received](const std::string& body) -> std::expected<void, std::string>
						{
							received = body;
							return {};
						})
					.has_value());
	ASSERT_TRUE(consumer.start().has_value());

	SqsMessagePublisher publisher(options);
	const auto sent = publisher.send(queue_url, R"({"command":"current_status","payload":{}})");
	ASSERT_TRUE(sent.has_value()) << (sent.has_value() ? "" : sent.error());

	for (int attempt = 0; attempt < 100 && received.empty(); ++attempt)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	consumer.stop();

	EXPECT_NE(received.find("current_status"), std::string::npos);
}
