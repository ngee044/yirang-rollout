#include <gtest/gtest.h>

#include "ArgumentParser.h"
#include "Commands.h"
#include "Configurations.h"
#include "IArtifactStore.h"
#include "RestClient.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace DeployCli;

namespace
{
	class RecordingStore : public Artifact::IArtifactStore
	{
	public:
		auto upload(const std::string& object_key, const std::string& local_path) -> std::expected<void, std::string> override
		{
			if (fail_uploads_)
			{
				return std::unexpected("upload refused");
			}

			uploaded_.insert({ object_key, local_path });

			return {};
		}

		auto presign(const std::string&) -> std::expected<std::string, std::string> override { return std::unexpected("not used"); }

		auto download(const std::string&, const std::string&) -> std::expected<void, std::string> override { return std::unexpected("not used"); }

		auto exists(const std::string&) -> std::expected<bool, std::string> override { return std::unexpected("not used"); }

		auto uploaded(void) const -> std::map<std::string, std::string> { return uploaded_; }

		auto fail_uploads(void) -> void { fail_uploads_ = true; }

	private:
		std::map<std::string, std::string> uploaded_;
		bool fail_uploads_{ false };
	};

	class TemporaryTree
	{
	public:
		TemporaryTree(void)
		{
			static int sequence = 0;
			static const auto token = std::to_string(std::random_device{}());

			root_ = std::filesystem::temp_directory_path() / ("yirang-cli-test-" + token + "-" + std::to_string(++sequence));
			std::filesystem::create_directories(root_);
		}

		~TemporaryTree(void)
		{
			std::error_code ignored;
			std::filesystem::remove_all(root_, ignored);
		}

		TemporaryTree(const TemporaryTree&) = delete;
		auto operator=(const TemporaryTree&) -> TemporaryTree& = delete;

		auto write(const std::string& name, const std::string& content) const -> std::string
		{
			const auto path = root_ / name;

			std::ofstream stream(path, std::ios::binary);
			stream << content;
			stream.close();

			return path.string();
		}

		auto path(void) const -> std::string { return root_.string(); }

	private:
		std::filesystem::path root_;
	};

	auto make_configurations(const std::vector<std::string>& tokens) -> std::shared_ptr<Configurations>
	{
		std::vector<std::string> owned{ "yirang", "--config_path", "/nonexistent-yirang-cli.json" };
		owned.insert(owned.end(), tokens.begin(), tokens.end());

		std::vector<char*> argv;
		argv.reserve(owned.size());
		for (auto& token : owned)
		{
			argv.push_back(token.data());
		}

		return std::make_shared<Configurations>(Utilities::ArgumentParser((int32_t)argv.size(), argv.data()));
	}

	auto unreachable_client(void) -> std::shared_ptr<RestClient> { return std::make_shared<RestClient>("http://127.0.0.1:1", "", std::chrono::seconds(1)); }
}

TEST(DeployCliTest, RegistersEverySubcommand)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1" });
	Commands commands(*configurations, nullptr, unreachable_client());

	const auto names = commands.names();

	ASSERT_EQ(names.size(), 3u);
	for (const auto& expected : { "command", "deploy", "results" })
	{
		EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end()) << expected;
	}
}

TEST(DeployCliTest, RejectsUnknownSubcommand)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1" });
	Commands commands(*configurations, nullptr, unreachable_client());

	const auto result = commands.run("frobnicate", {});

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("frobnicate"), std::string::npos);
}

TEST(DeployCliTest, EveryCommandRequiresControlPlaneUrl)
{
	auto configurations = make_configurations({});
	Commands commands(*configurations, nullptr, unreachable_client());

	for (const auto& name : commands.names())
	{
		const auto result = commands.run(name, { "current_status" });

		ASSERT_FALSE(result.has_value()) << name;
		EXPECT_NE(result.error().find("control_plane_url"), std::string::npos) << name;
	}
}

TEST(DeployCliTest, DeployRequiresUploadListAndBucket)
{
	auto without_list = make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--s3_bucket", "b" });
	Commands no_list(*without_list, std::make_shared<RecordingStore>(), unreachable_client());

	const auto missing_list = no_list.run("deploy", {});
	ASSERT_FALSE(missing_list.has_value());
	EXPECT_NE(missing_list.error().find("upload_file_list"), std::string::npos);

	TemporaryTree tree;
	const auto file = tree.write("app.exe", "binary");

	auto without_bucket = make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--upload_file_list", file });
	Commands no_bucket(*without_bucket, std::make_shared<RecordingStore>(), unreachable_client());

	const auto missing_bucket = no_bucket.run("deploy", {});
	ASSERT_FALSE(missing_bucket.has_value());
	EXPECT_NE(missing_bucket.error().find("s3_bucket"), std::string::npos);
}

TEST(DeployCliTest, UploadListSplitsOnComma)
{
	TemporaryTree tree;
	const auto first = tree.write("app.exe", "binary");
	const auto second = tree.write("config.json", "{}");

	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--s3_bucket", "b", "--upload_file_list", first + "," + second });

	const auto list = configurations->upload_file_list();

	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(list.at(0), first);
	EXPECT_EQ(list.at(1), second);
}

TEST(DeployCliTest, DeployUploadsEveryFileUnderTheReleaseKey)
{
	TemporaryTree tree;
	const auto first = tree.write("app.exe", "binary");
	const auto second = tree.write("config.json", "{}");

	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--s3_bucket", "b", "--upload_file_list", first + "," + second });

	auto store = std::make_shared<RecordingStore>();
	Commands commands(*configurations, store, unreachable_client());

	const auto result = commands.run("deploy", {});
	ASSERT_FALSE(result.has_value());

	const auto uploaded = store->uploaded();
	ASSERT_EQ(uploaded.size(), 2u);

	int matched = 0;
	for (const auto& [key, source] : uploaded)
	{
		EXPECT_EQ(key.rfind("releases/rel_", 0), 0u) << key;

		if (key.ends_with("/app.exe"))
		{
			EXPECT_EQ(source, first);
			++matched;
		}
		if (key.ends_with("/config.json"))
		{
			EXPECT_EQ(source, second);
			++matched;
		}
	}
	EXPECT_EQ(matched, 2);
}

TEST(DeployCliTest, DeployStopsWhenAnUploadFails)
{
	TemporaryTree tree;
	const auto first = tree.write("app.exe", "binary");
	const auto second = tree.write("config.json", "{}");

	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--s3_bucket", "b", "--upload_file_list", first + "," + second });

	auto store = std::make_shared<RecordingStore>();
	Commands commands(*configurations, store, unreachable_client());

	store->fail_uploads();
	const auto result = commands.run("deploy", {});

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("cannot upload"), std::string::npos) << result.error();
	EXPECT_EQ(result.error().find("cannot reach"), std::string::npos) << result.error();
	EXPECT_TRUE(store->uploaded().empty());
}

TEST(DeployCliTest, DeployRejectsMissingUploadFile)
{
	auto configurations
		= make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--s3_bucket", "b", "--upload_file_list", "/nonexistent-yirang-upload.bin" });

	auto store = std::make_shared<RecordingStore>();
	Commands commands(*configurations, store, unreachable_client());

	const auto result = commands.run("deploy", {});

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("does not exist"), std::string::npos);
	EXPECT_TRUE(store->uploaded().empty());
}

TEST(DeployCliTest, CommandRejectsNamesTheAgentDoesNotHandle)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1" });
	Commands commands(*configurations, nullptr, unreachable_client());

	const auto unknown = commands.run("command", { "restart_everything" });
	ASSERT_FALSE(unknown.has_value());
	EXPECT_NE(unknown.error().find("is not an agent command"), std::string::npos);

	const auto missing = commands.run("command", {});
	ASSERT_FALSE(missing.has_value());
	EXPECT_NE(missing.error().find("command name is required"), std::string::npos);
}

TEST(DeployCliTest, CommandReachesTheRestStageForSupportedNames)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1" });
	Commands commands(*configurations, nullptr, unreachable_client());

	const auto result = commands.run("command", { "current_status" });

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("cannot reach"), std::string::npos) << result.error();
}

TEST(RestClientTest, TrimsTrailingSlashFromBaseUrl)
{
	const RestClient client("http://127.0.0.1:8080///", "", std::chrono::seconds(1));

	EXPECT_EQ(client.base_url(), "http://127.0.0.1:8080");
}

TEST(RestClientTest, RefusesAnEmptyBaseUrl)
{
	const RestClient client("", "", std::chrono::seconds(1));

	const auto result = client.get("/healthz");

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("control_plane_url"), std::string::npos);
}

TEST(RestClientTest, ReportsUnreachableHostAsFailure)
{
	const RestClient client("http://127.0.0.1:1", "", std::chrono::seconds(1));

	const auto result = client.post("/api/v1/commands", "{}");

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("cannot reach"), std::string::npos);
}
