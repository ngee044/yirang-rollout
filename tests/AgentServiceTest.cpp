#include <gtest/gtest.h>

#include "AgentMessage.h"
#include "AgentService.h"
#include "IArtifactStore.h"
#include "PosixProcessSupervisor.h"
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

TEST(AgentServiceTest, ApplyAndRollbackAreConsumedWhenTheEngineIsNotConfigured)
{
	TemporaryTree tree;
	auto publisher = std::make_shared<RecordingPublisher>();

	AgentOptions options;
	options.version_root = tree.version_root();
	options.result_queue_url = "https://sqs.example/results";

	AgentService service(options, nullptr, publisher);

	EXPECT_TRUE(service.handle(envelope(Commands::kRollbackVersion, R"({"release_id":"rel_absent"})")).has_value());
	EXPECT_NE(publisher->body().find("deployment engine is not configured"), std::string::npos) << publisher->body();

	EXPECT_TRUE(service.handle(envelope(Commands::kApplyVersion, R"({"release_id":"rel_absent"})")).has_value());
	EXPECT_NE(publisher->body().find("deployment engine is not configured"), std::string::npos) << publisher->body();
	EXPECT_EQ(publisher->count(), 2);
	EXPECT_NE(publisher->body().find("\"success\":false"), std::string::npos);
}

TEST(AgentServiceTest, ApplyRejectsAVersionThatWasNotDownloaded)
{
	TemporaryTree tree;
	auto publisher = std::make_shared<RecordingPublisher>();

	Install::InstallOptions install_options;
	install_options.service_root = tree.service_root();

	Deploy::ServiceSpec spec;
	spec.executable = "app.exe";

	auto engine = std::make_shared<Deploy::DeploymentEngine>(std::make_shared<Install::ReleaseInstaller>(install_options),
															 std::make_shared<Process::PosixProcessSupervisor>(), spec, Health::HealthCheckSpec{});

	AgentOptions options;
	options.version_root = tree.version_root();
	options.service_root = tree.service_root();
	options.result_queue_url = "https://sqs.example/results";

	AgentService service(options, nullptr, publisher, engine);

	EXPECT_TRUE(service.handle(envelope(Commands::kApplyVersion, R"({"release_id":"rel_absent"})")).has_value());
	EXPECT_NE(publisher->body().find("not downloaded"), std::string::npos) << publisher->body();
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(tree.service_root()) / "state.json"));
}

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

TEST(AgentMessageTest, MissingPayloadBecomesEmptyObject)
{
	const auto parsed = parse_agent_message(R"({"command":"current_status"})");

	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(parsed.value().payload, "{}");
}

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
