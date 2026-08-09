#include <gtest/gtest.h>

#include "AgentMessage.h"
#include "AgentService.h"
#include "IArtifactStore.h"
#include "IMessagePublisher.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <random>

using namespace YirangAgent;

namespace
{
	class TemporaryTree
	{
	public:
		TemporaryTree(void)
		{
			static int sequence = 0;
			// 병렬 ctest 는 TEST 하나당 별도 프로세스로 돌고 sequence 가 프로세스마다 1 부터 다시
			// 시작한다. 프로세스 고유 토큰이 없으면 동시에 도는 두 케이스가 같은 경로를 공유하고,
			// 먼저 끝난 쪽의 소멸자가 아직 쓰고 있는 파일을 지운다.
			static const auto token = std::to_string(std::random_device{}());
			root_ = std::filesystem::temp_directory_path() / ("yirang-agent-test-" + token + "-" + std::to_string(++sequence));

			std::error_code error;
			std::filesystem::create_directories(root_, error);
		}

		~TemporaryTree(void)
		{
			std::error_code error;
			std::filesystem::remove_all(root_, error);
		}

		auto make_version(const std::string& release_id) -> std::string
		{
			const auto directory = root_ / "versions" / release_id;

			std::error_code error;
			std::filesystem::create_directories(directory, error);

			std::ofstream stream(directory / "app.exe", std::ios::binary);
			stream << "payload";

			return directory.string();
		}

		auto version_root(void) const -> std::string { return (root_ / "versions").string(); }
		auto service_root(void) const -> std::string { return (root_ / "service").string(); }

	private:
		std::filesystem::path root_;
	};

	auto make_service(const TemporaryTree& tree) -> AgentService
	{
		AgentOptions options;
		options.device_id = "pc-001";
		options.group = "kiosk";
		options.version_root = tree.version_root();
		options.service_root = tree.service_root();

		return AgentService(options, nullptr);
	}

	// 결과 보고를 네트워크 없이 검증하기 위한 대역.
	class RecordingPublisher : public Messaging::IMessagePublisher
	{
	public:
		auto send(const std::string& queue_url, const std::string& body) -> std::expected<void, std::string> override
		{
			queue_url_ = queue_url;
			body_ = body;
			++count_;

			return {};
		}

		auto queue_url(void) const -> std::string { return queue_url_; }
		auto body(void) const -> std::string { return body_; }
		auto count(void) const -> int { return count_; }

	private:
		std::string queue_url_;
		std::string body_;
		int count_{ 0 };
	};

	// 다운로드를 항상 실패시켜 실패 경로(잔여물 정리)를 검사한다.
	class FailingStore : public Artifact::IArtifactStore
	{
	public:
		auto upload(const std::string&, const std::string&) -> std::expected<void, std::string> override { return std::unexpected("not used"); }

		auto presign(const std::string&) -> std::expected<std::string, std::string> override { return std::unexpected("not used"); }

		auto download(const std::string&, const std::string&) -> std::expected<void, std::string> override { return std::unexpected("download refused"); }

		auto exists(const std::string&) -> std::expected<bool, std::string> override { return std::unexpected("not used"); }
	};

	auto envelope(const std::string& command, const std::string& payload = "{}") -> std::string
	{
		return R"({"command":")" + command + R"(","payload":)" + payload + "}";
	}
}

// 지원 명령 5종이 전부 등록되어야 한다 (등록 누락은 조용한 무시로 이어진다)
TEST(AgentServiceTest, RegistersEverySupportedCommand)
{
	TemporaryTree tree;
	auto service = make_service(tree);

	const auto names = service.commands();

	EXPECT_EQ(names.size(), 5u);
	for (const auto* expected : { Commands::kDownloadVersion, Commands::kApplyVersion, Commands::kCurrentStatus, Commands::kCleanOldVersion, Commands::kRollbackVersion })
	{
		EXPECT_NE(std::find(names.begin(), names.end(), std::string(expected)), names.end()) << expected;
	}
}

// 미등록 명령을 조용히 무시하면 발행 측이 처리된 줄 안다
// handle() 의 반환값은 "메시지를 소비했는가"다. 재배달해도 없는 핸들러가 생기지는 않으므로
// 미등록 명령은 보고만 남기고 소비한다 — 되돌리면 가시성 타임아웃마다 영구 재배달된다.
TEST(AgentServiceTest, UnknownCommandIsConsumedAndReportedAsFailure)
{
	TemporaryTree tree;
	auto publisher = std::make_shared<RecordingPublisher>();

	AgentOptions options;
	options.version_root = tree.version_root();
	options.result_queue_url = "https://sqs.example/results";

	AgentService service(options, nullptr, publisher);

	EXPECT_TRUE(service.handle(envelope("no_such_command")).has_value());
	EXPECT_NE(publisher->body().find("no_such_command"), std::string::npos);
	EXPECT_NE(publisher->body().find("\"success\":false"), std::string::npos);
}

// 같은 본문을 다시 파싱해도 결과가 같다. 보고만 남기고 소비한다.
TEST(AgentServiceTest, MalformedEnvelopeIsConsumedAndReported)
{
	TemporaryTree tree;
	auto publisher = std::make_shared<RecordingPublisher>();

	AgentOptions options;
	options.version_root = tree.version_root();
	options.result_queue_url = "https://sqs.example/results";

	AgentService service(options, nullptr, publisher);

	EXPECT_TRUE(service.handle("{ not json").has_value());
	EXPECT_TRUE(service.handle(R"([1,2,3])").has_value());
	EXPECT_TRUE(service.handle(R"({"payload":{}})").has_value());
	EXPECT_TRUE(service.handle(R"({"command":"","payload":{}})").has_value());

	EXPECT_EQ(publisher->count(), 4);
	EXPECT_NE(publisher->body().find("\"success\":false"), std::string::npos);
}

// clean_old_version 은 받아둔 버전 폴더를 전부 지운다
TEST(AgentServiceTest, CleanOldVersionRemovesEveryDownloadedVersion)
{
	TemporaryTree tree;
	tree.make_version("rel_1");
	tree.make_version("rel_2");
	auto service = make_service(tree);

	ASSERT_TRUE(std::filesystem::exists(tree.version_root() + "/rel_1"));

	const auto result = service.handle(envelope(Commands::kCleanOldVersion));

	ASSERT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error());
	EXPECT_FALSE(std::filesystem::exists(tree.version_root() + "/rel_1"));
	EXPECT_FALSE(std::filesystem::exists(tree.version_root() + "/rel_2"));
	EXPECT_TRUE(std::filesystem::exists(tree.version_root()));
}

// 서비스 실행 경로를 지우면 가동 중인 앱이 사라진다
TEST(AgentServiceTest, CleanOldVersionRefusesWhenRootsAreIdentical)
{
	TemporaryTree tree;
	tree.make_version("rel_1");

	AgentOptions options;
	options.version_root = tree.version_root();
	options.service_root = tree.version_root();

	AgentService service(options, nullptr);

	const auto result = service.handle(envelope(Commands::kCleanOldVersion));

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("service_root"), std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(tree.version_root() + "/rel_1"));
}

// current_status 는 디스크 여유와 받아둔 버전 목록을 보고한다
TEST(AgentServiceTest, CurrentStatusReportsDiskAndVersions)
{
	TemporaryTree tree;
	tree.make_version("rel_1");
	auto service = make_service(tree);

	const auto result = service.handle(envelope(Commands::kCurrentStatus));

	ASSERT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error());

	const auto report = service.last_report();
	EXPECT_NE(report.find("disk_available_bytes"), std::string::npos);
	EXPECT_NE(report.find("pc-001"), std::string::npos);
	EXPECT_NE(report.find("rel_1"), std::string::npos);
}

// 요구대로 old version 폴더에 파일이 없으면 실패를 되돌린다
// apply_version·rollback_version 은 릴리스 설치기(R-008) 부재로 구조적으로 항상 실패한다.
// 실패를 큐로 되돌리면 영구 재배달만 남으므로 보고 후 소비한다. R-008 구현 시 이 기대는 바뀐다.
TEST(AgentServiceTest, ApplyAndRollbackAreConsumedWhileInstallerIsMissing)
{
	TemporaryTree tree;
	auto publisher = std::make_shared<RecordingPublisher>();

	AgentOptions options;
	options.version_root = tree.version_root();
	options.result_queue_url = "https://sqs.example/results";

	AgentService service(options, nullptr, publisher);

	EXPECT_TRUE(service.handle(envelope(Commands::kRollbackVersion, R"({"release_id":"rel_absent"})")).has_value());
	EXPECT_NE(publisher->body().find("rel_absent"), std::string::npos);

	EXPECT_TRUE(service.handle(envelope(Commands::kApplyVersion, R"({"release_id":"rel_absent"})")).has_value());
	EXPECT_NE(publisher->body().find("not downloaded"), std::string::npos);
	EXPECT_EQ(publisher->count(), 2);
}

// 빈 목록을 성공으로 처리하면 빈 버전 디렉터리가 정상 릴리스로 보고된다.
TEST(AgentServiceTest, DownloadVersionRejectsEmptyArtifacts)
{
	TemporaryTree tree;
	auto store = std::make_shared<FailingStore>();
	AgentOptions options;
	options.version_root = tree.version_root();

	AgentService service(options, store, nullptr);

	const auto result = service.handle(envelope(Commands::kDownloadVersion, R"({"release_id":"rel_1","artifacts":[]})"));

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("must not be empty"), std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(tree.version_root()) / "rel_1"));
}

// 다운로드가 중도 실패하면 버전 디렉터리가 남지 않아야 한다. 남으면 손상 릴리스가
// current_status 에 정상 버전으로 잡히고 apply_version 의 존재 검사도 통과한다.
TEST(AgentServiceTest, FailedDownloadLeavesNoVersionDirectory)
{
	TemporaryTree tree;
	auto store = std::make_shared<FailingStore>();
	AgentOptions options;
	options.version_root = tree.version_root();

	AgentService service(options, store, nullptr);

	const auto result = service.handle(envelope(Commands::kDownloadVersion, R"({"release_id":"rel_2","artifacts":[{"install_path":"app.exe","sha256":"deadbeef"}]})"));

	ASSERT_FALSE(result.has_value());
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(tree.version_root()) / "rel_2"));
}

// 봉투가 결과 큐를 지정하면 설정의 기본 큐보다 우선한다 (AgentMessage.h 의 계약).
TEST(AgentServiceTest, ReplyQueueUrlOverridesConfiguredResultQueue)
{
	TemporaryTree tree;
	auto publisher = std::make_shared<RecordingPublisher>();

	AgentOptions options;
	options.version_root = tree.version_root();
	options.result_queue_url = "";

	AgentService service(options, nullptr, publisher);

	AgentMessage message;
	message.command = Commands::kCurrentStatus;
	message.payload = "{}";
	message.reply_queue_url = "https://sqs.example/from-envelope";

	ASSERT_TRUE(service.handle(serialize_agent_message(message)).has_value());
	EXPECT_EQ(publisher->queue_url(), "https://sqs.example/from-envelope");
}

TEST(AgentServiceTest, DownloadVersionRequiresArtifactStore)
{
	TemporaryTree tree;
	auto service = make_service(tree);

	const auto result = service.handle(envelope(Commands::kDownloadVersion, R"({"release_id":"rel_1","artifacts":[]})"));

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("artifact store"), std::string::npos);
}

TEST(AgentMessageTest, SerializeParseRoundTrip)
{
	AgentMessage message;
	message.command = Commands::kDownloadVersion;
	message.payload = R"({"release_id":"rel_1"})";
	message.reply_queue_url = "https://sqs.example/result";

	const auto restored = parse_agent_message(serialize_agent_message(message));

	ASSERT_TRUE(restored.has_value()) << (restored.has_value() ? "" : restored.error());
	EXPECT_EQ(restored.value().command, message.command);
	EXPECT_EQ(restored.value().reply_queue_url, message.reply_queue_url);
	EXPECT_NE(restored.value().payload.find("rel_1"), std::string::npos);
}

// payload 가 없으면 빈 객체로 채워 핸들러가 항상 같은 형태를 본다
TEST(AgentMessageTest, MissingPayloadBecomesEmptyObject)
{
	const auto parsed = parse_agent_message(R"({"command":"current_status"})");

	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(parsed.value().payload, "{}");
}

// 처리 결과가 결과 큐로 보고되어야 한다
TEST(AgentServiceTest, ReportsOutcomeToResultQueue)
{
	TemporaryTree tree;
	auto publisher = std::make_shared<RecordingPublisher>();

	AgentOptions options;
	options.device_id = "pc-001";
	options.group = "kiosk";
	options.version_root = tree.version_root();
	options.service_root = tree.service_root();
	options.result_queue_url = "https://sqs.example/results";

	AgentService service(options, nullptr, publisher);

	ASSERT_TRUE(service.handle(envelope(Commands::kCurrentStatus)).has_value());

	EXPECT_EQ(publisher->count(), 1);
	EXPECT_EQ(publisher->queue_url(), "https://sqs.example/results");
	EXPECT_NE(publisher->body().find("\"success\":true"), std::string::npos);
	EXPECT_NE(publisher->body().find("current_status"), std::string::npos);
	EXPECT_NE(publisher->body().find("pc-001"), std::string::npos);
}

// 실패도 보고되어야 한다 — 발행 측이 결과를 알아야 다음 판단을 한다
// 결과 큐가 없으면 보고하지 않는다 (발행자 없이도 동작해야 한다)
TEST(AgentServiceTest, SkipsReportWhenResultQueueIsNotConfigured)
{
	TemporaryTree tree;
	auto publisher = std::make_shared<RecordingPublisher>();

	AgentOptions options;
	options.version_root = tree.version_root();

	AgentService service(options, nullptr, publisher);

	ASSERT_TRUE(service.handle(envelope(Commands::kCurrentStatus)).has_value());

	EXPECT_EQ(publisher->count(), 0);
}
