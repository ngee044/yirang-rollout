#include <gtest/gtest.h>

#include "ArgumentParser.h"
#include "Commands.h"
#include "Configurations.h"
#include "Confirmation.h"
#include "IArtifactStore.h"
#include "ReleaseManifest.h"
#include "RestClient.h"

#include <boost/json.hpp>

#include <atomic>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace DeployCli;

namespace
{
	class RecordingStore : public Artifact::IArtifactStore
	{
	public:
		auto upload(const std::string& object_key, const std::string& local_path) -> std::expected<void, std::string> override
		{
			if (fail_uploads_ || (fail_manifest_ && object_key.starts_with("manifests/")))
			{
				return std::unexpected("upload refused");
			}

			uploaded_.insert({ object_key, local_path });
			std::ifstream source(local_path, std::ios::binary);
			std::ostringstream buffer;
			buffer << source.rdbuf();
			contents_[object_key] = buffer.str();
			order_.push_back(object_key);

			return {};
		}

		auto presign(const std::string&) -> std::expected<std::string, std::string> override { return std::unexpected("not used"); }

		auto download(const std::string& object_key, const std::string& local_path) -> std::expected<void, std::string> override
		{
			downloaded_path_ = local_path;
			const auto found = contents_.find(object_key);
			if (found == contents_.end())
			{
				return std::unexpected("object not found");
			}
			std::ofstream target(local_path, std::ios::binary);
			target << found->second;
			return {};
		}

		auto exists(const std::string&) -> std::expected<bool, std::string> override { return std::unexpected("not used"); }

		auto uploaded(void) const -> std::map<std::string, std::string> { return uploaded_; }

		auto fail_uploads(void) -> void { fail_uploads_ = true; }
		auto fail_manifest(void) -> void { fail_manifest_ = true; }
		auto contents(void) const -> std::map<std::string, std::string> { return contents_; }
		auto contents(const std::string& key, const std::string& body) -> void { contents_[key] = body; }
		auto order(void) const -> std::vector<std::string> { return order_; }
		auto downloaded_path(void) const -> std::string { return downloaded_path_; }

	private:
		std::map<std::string, std::string> uploaded_;
		std::map<std::string, std::string> contents_;
		std::vector<std::string> order_;
		std::string downloaded_path_;
		bool fail_uploads_{ false };
		bool fail_manifest_{ false };
	};

	class PublicationServer
	{
	public:
		explicit PublicationServer(const std::string& body)
		{
			listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = 0;
			EXPECT_GE(listen_fd_, 0);
			EXPECT_EQ(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
			EXPECT_EQ(::listen(listen_fd_, 1), 0);
			socklen_t length = sizeof(address);
			EXPECT_EQ(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length), 0);
			port_ = ntohs(address.sin_port);
			worker_ = std::thread(&PublicationServer::serve, this, body);
		}

		~PublicationServer(void)
		{
			stopping_ = true;
			if (worker_.joinable())
			{
				worker_.join();
			}
			::close(listen_fd_);
		}

		auto url(void) const -> std::string { return std::format("http://127.0.0.1:{}", port_); }

		auto request(void) -> std::string
		{
			std::scoped_lock guard(mutex_);
			return request_;
		}

	private:
		auto serve(const std::string& body) -> void
		{
			while (!stopping_)
			{
				pollfd descriptor{ listen_fd_, POLLIN, 0 };
				if (::poll(&descriptor, 1, 50) <= 0)
				{
					continue;
				}
				const int client = ::accept(listen_fd_, nullptr, nullptr);
				if (client < 0)
				{
					return;
				}
				timeval timeout{ 2, 0 };
				::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#ifdef __APPLE__
				int no_sigpipe = 1;
				::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
				std::string request;
				char buffer[4096];
				while (request.size() < 1024 * 1024)
				{
					const auto received = ::recv(client, buffer, sizeof(buffer), 0);
					if (received <= 0)
					{
						break;
					}
					request.append(buffer, static_cast<size_t>(received));
					const auto header_end = request.find("\r\n\r\n");
					const auto length_start = request.find("Content-Length: ");
					if (header_end != std::string::npos && length_start != std::string::npos)
					{
						size_t content_length = 0;
						const auto parsed = std::from_chars(request.data() + length_start + 16, request.data() + header_end, content_length);
						if (parsed.ec == std::errc() && request.size() >= header_end + 4 + content_length)
						{
							break;
						}
					}
				}
				{
					std::scoped_lock guard(mutex_);
					request_ = request;
				}

				const auto response = std::format("HTTP/1.1 202 Accepted\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.size(), body);
				size_t offset = 0;
				while (offset < response.size())
				{
#ifdef MSG_NOSIGNAL
					const auto sent = ::send(client, response.data() + offset, response.size() - offset, MSG_NOSIGNAL);
#else
					const auto sent = ::send(client, response.data() + offset, response.size() - offset, 0);
#endif
					if (sent <= 0)
					{
						break;
					}
					offset += static_cast<size_t>(sent);
				}
				::close(client);
				return;
			}
		}

		int listen_fd_{ -1 };
		uint16_t port_{ 0 };
		std::atomic<bool> stopping_{ false };
		std::thread worker_;
		std::mutex mutex_;
		std::string request_;
	};

	const std::string published_response
		= R"({"success":true,"data":{"command":"download_version","targeted":1,"published":1,"deliveries":[{"queue":"pc-001","message_id":"message-1"}]}})";
	const std::string partial_response
		= R"({"success":true,"data":{"command":"download_version","targeted":2,"published":1,"deliveries":[{"queue":"pc-001","message_id":"message-1"},{"queue":"pc-002","error":"AccessDenied"}]}})";

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
	ASSERT_EQ(uploaded.size(), 3u);

	int matched = 0;
	for (const auto& [key, source] : uploaded)
	{
		if (key.starts_with("manifests/"))
		{
			EXPECT_FALSE(std::filesystem::exists(source));
			continue;
		}
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

TEST(DeployCliTest, PublicationRequiresEveryDeviceToSucceed)
{
	for (const auto& body : { published_response, partial_response })
	{
		PublicationServer server(body);
		auto configurations = make_configurations({ "--control_plane_url", server.url() });
		auto client = std::make_shared<RestClient>(server.url(), "", std::chrono::seconds(3));
		Commands commands(*configurations, nullptr, client);
		const auto result = commands.run("command", { "current_status" });
		if (body == published_response)
		{
			EXPECT_TRUE(result.has_value()) << (result ? "" : result.error());
		}
		else
		{
			ASSERT_FALSE(result.has_value());
			EXPECT_NE(result.error().find("publication failed for 1 of 2"), std::string::npos);
		}
		EXPECT_TRUE(server.request().starts_with("POST /api/v1/commands "));
	}
}

TEST(DeployCliTest, RejectsMalformedPublicationResponses)
{
	for (const auto& body : { R"({"success":true})", R"({"success":true,"data":{"targeted":1,"published":1,"deliveries":[{"error":"denied"}]}})",
							  R"({"success":true,"data":{"targeted":2,"published":2,"deliveries":[{}]}})" })
	{
		PublicationServer server(body);
		auto configurations = make_configurations({ "--control_plane_url", server.url() });
		auto client = std::make_shared<RestClient>(server.url(), "", std::chrono::seconds(3));
		Commands commands(*configurations, nullptr, client);
		const auto result = commands.run("command", { "current_status" });
		ASSERT_FALSE(result.has_value());
		EXPECT_NE(result.error().find("invalid publication response"), std::string::npos) << result.error();
	}
}

TEST(DeployCliTest, DeployReportsPartialPublicationFailure)
{
	TemporaryTree tree;
	const auto file = tree.write("app.exe", "binary");
	PublicationServer server(partial_response);
	auto configurations = make_configurations({ "--control_plane_url", server.url(), "--s3_bucket", "b", "--upload_file_list", file });
	auto client = std::make_shared<RestClient>(server.url(), "", std::chrono::seconds(3));
	Commands commands(*configurations, std::make_shared<RecordingStore>(), client);
	const auto result = commands.run("deploy", {});
	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("publication failed for 1 of 2"), std::string::npos);
	EXPECT_TRUE(server.request().starts_with("POST /api/v1/deployments "));
}

TEST(DeployCliTest, DeployManifestCanReissueTheSameDownload)
{
	TemporaryTree tree;
	const auto file = tree.write("app.exe", "binary");
	auto store = std::make_shared<RecordingStore>();
	boost::json::object deployment;
	{
		PublicationServer server(published_response);
		auto configurations = make_configurations({ "--control_plane_url", server.url(), "--s3_bucket", "b", "--upload_file_list", file });
		auto client = std::make_shared<RestClient>(server.url(), "", std::chrono::seconds(3));
		Commands commands(*configurations, store, client);
		const auto result = commands.run("deploy", {});
		ASSERT_TRUE(result.has_value()) << (result ? "" : result.error());
		const auto request = server.request();
		deployment = boost::json::parse(request.substr(request.find("\r\n\r\n") + 4)).as_object();
	}
	const auto release_id = std::string(deployment.at("release_id").as_string());
	const auto manifest_key = std::format("manifests/{}.json", release_id);
	ASSERT_EQ(store->order().size(), 2u);
	EXPECT_EQ(store->order().back(), manifest_key);
	const auto manifest = Release::ReleaseManifest::parse(store->contents().at(manifest_key));
	ASSERT_TRUE(manifest.has_value());
	EXPECT_EQ(manifest->release_id, release_id);
	EXPECT_FALSE(std::filesystem::exists(store->uploaded().at(manifest_key)));

	PublicationServer server(published_response);
	auto configurations = make_configurations({ "--control_plane_url", server.url(), "--s3_bucket", "b" });
	auto client = std::make_shared<RestClient>(server.url(), "", std::chrono::seconds(3));
	Commands commands(*configurations, store, client);
	const auto result = commands.run("command", { "download_version", release_id });
	ASSERT_TRUE(result.has_value()) << (result ? "" : result.error());
	const auto request = server.request();
	EXPECT_TRUE(request.starts_with("POST /api/v1/commands "));
	const auto command = boost::json::parse(request.substr(request.find("\r\n\r\n") + 4)).as_object();
	EXPECT_EQ(command.at("command"), "download_version");
	EXPECT_EQ(command.at("payload").as_object().at("release_id"), deployment.at("release_id"));
	EXPECT_EQ(command.at("payload").as_object().at("artifacts"), deployment.at("artifacts"));
	EXPECT_FALSE(std::filesystem::exists(store->downloaded_path()));
}

TEST(DeployCliTest, ManifestUploadFailurePreventsPublication)
{
	TemporaryTree tree;
	const auto file = tree.write("app.exe", "binary");
	PublicationServer server(published_response);
	auto configurations = make_configurations({ "--control_plane_url", server.url(), "--s3_bucket", "b", "--upload_file_list", file });
	auto client = std::make_shared<RestClient>(server.url(), "", std::chrono::seconds(3));
	auto store = std::make_shared<RecordingStore>();
	store->fail_manifest();
	Commands commands(*configurations, store, client);
	const auto result = commands.run("deploy", {});
	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("cannot upload manifest"), std::string::npos);
	EXPECT_TRUE(server.request().empty());
	EXPECT_EQ(store->uploaded().size(), 1u);
}

TEST(DeployCliTest, DownloadRejectsMissingOrInvalidManifestBeforePublication)
{
	PublicationServer server(published_response);
	auto configurations = make_configurations({ "--control_plane_url", server.url(), "--s3_bucket", "b" });
	auto client = std::make_shared<RestClient>(server.url(), "", std::chrono::seconds(3));
	auto store = std::make_shared<RecordingStore>();
	Commands commands(*configurations, store, client);
	const auto missing = commands.run("command", { "download_version", "rel_1" });
	ASSERT_FALSE(missing.has_value());
	EXPECT_NE(missing.error().find("cannot download manifest"), std::string::npos);
	const std::map<std::string, std::string> invalid_manifests{
		{ "not json", "cannot parse manifest" },
		{ R"({"release_id":"rel_other","created_at":"2026-11-07T00:00:00Z","artifacts":[]})", "does not match" },
		{ R"({"release_id":"rel_1","created_at":"2026-11-07T00:00:00Z","artifacts":[]})", "artifacts must not be empty" },
		{ R"({"release_id":"rel_1","created_at":"2026-11-07T00:00:00Z","artifacts":[{"install_path":"../outside","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size_bytes":1}]})",
		  "must not contain '..'" }
	};
	for (const auto& [body, reason] : invalid_manifests)
	{
		store->contents("manifests/rel_1.json", body);
		const auto result = commands.run("command", { "download_version", "rel_1" });
		ASSERT_FALSE(result.has_value()) << body;
		EXPECT_NE(result.error().find(reason), std::string::npos) << result.error();
		EXPECT_FALSE(std::filesystem::exists(store->downloaded_path()));
	}
	EXPECT_TRUE(server.request().empty());
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

	const auto restart = commands.run("command", { "restart_service" });
	ASSERT_FALSE(restart.has_value());
	EXPECT_NE(restart.error().find("cannot reach"), std::string::npos) << restart.error();
}

TEST(DeployCliTest, UpdateConfigurationNeedsAPathAndValuesBeforeTheRestStage)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1" });
	Commands commands(*configurations, nullptr, unreachable_client());

	const auto without_path = commands.run("command", { "update_configuration" });
	ASSERT_FALSE(without_path.has_value());
	EXPECT_NE(without_path.error().find("configuration path"), std::string::npos) << without_path.error();

	const auto without_values = commands.run("command", { "update_configuration", "config/app.json" });
	ASSERT_FALSE(without_values.has_value());
	EXPECT_NE(without_values.error().find("--config_values"), std::string::npos) << without_values.error();
}

TEST(DeployCliTest, UpdateConfigurationRejectsValuesThatAreNotAJsonObject)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--config_values", "[1,2,3]" });
	Commands commands(*configurations, nullptr, unreachable_client());

	const auto result = commands.run("command", { "update_configuration", "config/app.json" });

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("--config_values"), std::string::npos) << result.error();
}

TEST(DeployCliTest, RebootDeviceRequiresConfirmationLikeCleanOldVersion)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--target_group", "kiosk" });

	std::istringstream input("n\n");
	std::ostringstream output;
	auto confirmation = std::make_shared<Confirmation>("", true, input, output);

	Commands commands(*configurations, nullptr, unreachable_client(), confirmation);

	const auto result = commands.run("command", { "reboot_device" });

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("reboot_device"), std::string::npos) << result.error();
	EXPECT_NE(output.str().find("reboot_device"), std::string::npos) << output.str();
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

TEST(DeployCliTest, InsecureTlsDefaultsToFalseAndIsOptIn)
{
	const auto missing = make_configurations({});
	EXPECT_FALSE(missing->allow_insecure_tls());

	TemporaryTree tree;
	const auto path = tree.write("cli.json", R"({"allow_insecure_tls": true})");

	const auto from_file = make_configurations({ "--config_path", path });
	EXPECT_TRUE(from_file->allow_insecure_tls());

	const auto from_command_line = make_configurations({ "--config_path", path, "--allow_insecure_tls", "false" });
	EXPECT_FALSE(from_command_line->allow_insecure_tls());
}

TEST(ConfirmationTest, MatchingTokenSkipsThePrompt)
{
	std::istringstream input;
	std::ostringstream output;

	Confirmation confirmation("clean_old_version", true, input, output);

	EXPECT_TRUE(confirmation.require("clean_old_version", "group 'kiosk'").has_value());
	EXPECT_TRUE(output.str().empty());
}

TEST(ConfirmationTest, TokenNamingAnotherCommandIsRefused)
{
	std::istringstream input("clean_old_version\n");
	std::ostringstream output;

	Confirmation confirmation("rollback_version", true, input, output);

	const auto result = confirmation.require("clean_old_version", "group 'kiosk'");

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("--confirm clean_old_version"), std::string::npos) << result.error();
	EXPECT_TRUE(output.str().empty());
}

TEST(ConfirmationTest, NonInteractiveInputIsRefusedInsteadOfApproved)
{
	std::istringstream input("clean_old_version\n");
	std::ostringstream output;

	Confirmation confirmation("", false, input, output);

	const auto result = confirmation.require("clean_old_version", "every registered device");

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("--confirm clean_old_version"), std::string::npos) << result.error();
	EXPECT_NE(result.error().find("every registered device"), std::string::npos) << result.error();
	EXPECT_TRUE(output.str().empty());
}

TEST(ConfirmationTest, InteractiveApprovalNeedsTheExactCommandName)
{
	for (const auto& answer : { "clean_old_version\n", "  clean_old_version  \n", "clean_old_version\r\n" })
	{
		std::istringstream input(answer);
		std::ostringstream output;
		Confirmation confirmation("", true, input, output);

		EXPECT_TRUE(confirmation.require("clean_old_version", "every registered device").has_value()) << answer;
	}

	for (const auto& answer : { "y\n", "yes\n", "\n", "", "clean_old\n" })
	{
		std::istringstream input(answer);
		std::ostringstream output;
		Confirmation confirmation("", true, input, output);

		const auto result = confirmation.require("clean_old_version", "every registered device");

		ASSERT_FALSE(result.has_value()) << answer;
		EXPECT_NE(result.error().find("was not confirmed"), std::string::npos) << answer;
	}
}

TEST(ConfirmationTest, PromptNamesTheCommandAndTheScope)
{
	std::istringstream input("clean_old_version\n");
	std::ostringstream output;

	Confirmation confirmation("", true, input, output);

	ASSERT_TRUE(confirmation.require("clean_old_version", "every registered device").has_value());
	EXPECT_NE(output.str().find("clean_old_version"), std::string::npos) << output.str();
	EXPECT_NE(output.str().find("every registered device"), std::string::npos) << output.str();
}

TEST(DeployCliTest, DestructiveCommandIsNotPublishedWhenTheOperatorDeclines)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--target_group", "kiosk" });

	std::istringstream input("n\n");
	std::ostringstream output;
	auto confirmation = std::make_shared<Confirmation>("", true, input, output);

	Commands commands(*configurations, nullptr, unreachable_client(), confirmation);

	const auto result = commands.run("command", { "clean_old_version" });

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("was not confirmed"), std::string::npos) << result.error();
	EXPECT_EQ(result.error().find("cannot reach"), std::string::npos) << result.error();
	EXPECT_NE(output.str().find("group 'kiosk'"), std::string::npos) << output.str();
}

TEST(DeployCliTest, DestructiveCommandWithoutATargetGroupSaysTheWholeFleetIsHit)
{
	TemporaryTree tree;
	const auto path = tree.write("fleet.json", R"({"control_plane_url":"http://127.0.0.1:1","target_group":""})");

	auto configurations = make_configurations({ "--config_path", path });

	std::istringstream input("n\n");
	std::ostringstream output;
	auto confirmation = std::make_shared<Confirmation>("", true, input, output);

	Commands commands(*configurations, nullptr, unreachable_client(), confirmation);

	const auto result = commands.run("command", { "clean_old_version" });

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(output.str().find("every registered device"), std::string::npos) << output.str();
}

TEST(DeployCliTest, ConfirmedDestructiveCommandReachesTheRestStage)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--target_group", "kiosk", "--confirm", "clean_old_version" });

	ASSERT_EQ(configurations->confirm_token(), "clean_old_version");

	std::istringstream input;
	std::ostringstream output;
	auto confirmation = std::make_shared<Confirmation>(configurations->confirm_token(), false, input, output);

	Commands commands(*configurations, nullptr, unreachable_client(), confirmation);

	const auto result = commands.run("command", { "clean_old_version" });

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("cannot reach"), std::string::npos) << result.error();
	EXPECT_TRUE(output.str().empty());
}

TEST(DeployCliTest, NonDestructiveCommandsAreNeverHeldForConfirmation)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1" });

	std::istringstream input;
	std::ostringstream output;
	auto confirmation = std::make_shared<Confirmation>("", false, input, output);

	Commands commands(*configurations, nullptr, unreachable_client(), confirmation);

	for (const auto& name : { "apply_version", "current_status", "rollback_version" })
	{
		const auto result = commands.run("command", { name });

		ASSERT_FALSE(result.has_value()) << name;
		EXPECT_NE(result.error().find("cannot reach"), std::string::npos) << name << " — " << result.error();
	}

	EXPECT_TRUE(output.str().empty());
}

TEST(DeployCliTest, DestructiveCommandIsRefusedWhenNoConfirmationIsWired)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--target_group", "kiosk" });

	Commands commands(*configurations, nullptr, unreachable_client());

	const auto result = commands.run("command", { "clean_old_version" });

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("confirmation is not available"), std::string::npos) << result.error();
	EXPECT_EQ(result.error().find("cannot reach"), std::string::npos) << result.error();
}

TEST(DeployCliTest, DestructiveCommandIsRefusedWhenTheScopeIsASilentDefault)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1" });

	ASSERT_TRUE(configurations->load_warning() != std::nullopt);
	ASSERT_TRUE(configurations->target_group().empty());

	std::istringstream input;
	std::ostringstream output;
	auto confirmation = std::make_shared<Confirmation>("clean_old_version", false, input, output);

	Commands commands(*configurations, nullptr, unreachable_client(), confirmation);

	const auto result = commands.run("command", { "clean_old_version" });

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("--target_group"), std::string::npos) << result.error();
	EXPECT_EQ(result.error().find("cannot reach"), std::string::npos) << result.error();
}

TEST(DeployCliTest, DestructiveCommandRunsWithAnExplicitGroupEvenWithoutAConfigFile)
{
	auto configurations = make_configurations({ "--control_plane_url", "http://127.0.0.1:1", "--target_group", "kiosk", "--confirm", "clean_old_version" });

	ASSERT_TRUE(configurations->load_warning() != std::nullopt);

	std::istringstream input;
	std::ostringstream output;
	auto confirmation = std::make_shared<Confirmation>(configurations->confirm_token(), false, input, output);

	Commands commands(*configurations, nullptr, unreachable_client(), confirmation);

	const auto result = commands.run("command", { "clean_old_version" });

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("cannot reach"), std::string::npos) << result.error();
}
