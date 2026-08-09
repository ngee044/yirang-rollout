#include <gtest/gtest.h>

#include "ReleaseManifest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <random>

using namespace Release;

namespace
{
	// 알려진 SHA-256 값과 대조해 해시 구현 자체를 검증한다.
	constexpr auto kEmptySha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
	constexpr auto kHelloSha256 = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

	auto fixed_time(void) -> std::chrono::system_clock::time_point
	{
		// 2023-11-14T22:13:20Z
		return std::chrono::system_clock::from_time_t(1700000000);
	}

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
			root_ = std::filesystem::temp_directory_path() / ("yirang-release-test-" + token + "-" + std::to_string(++sequence));

			std::error_code error;
			std::filesystem::create_directories(root_, error);
		}

		~TemporaryTree(void)
		{
			std::error_code error;
			std::filesystem::remove_all(root_, error);
		}

		auto write(const std::string& relative, const std::string& contents) -> std::string
		{
			const auto path = root_ / relative;

			std::error_code error;
			std::filesystem::create_directories(path.parent_path(), error);

			std::ofstream stream(path, std::ios::binary);
			stream << contents;
			stream.close();

			return path.string();
		}

		auto path(const std::string& relative) const -> std::string { return (root_ / relative).string(); }

	private:
		std::filesystem::path root_;
	};
}

TEST(ReleaseManifestTest, FileSha256MatchesKnownValues)
{
	TemporaryTree tree;

	EXPECT_EQ(ReleaseManifest::file_sha256(tree.write("empty.bin", "")).value_or(""), kEmptySha256);
	EXPECT_EQ(ReleaseManifest::file_sha256(tree.write("hello.txt", "hello")).value_or(""), kHelloSha256);
}

TEST(ReleaseManifestTest, FileSha256FailsForMissingFile)
{
	TemporaryTree tree;

	const auto result = ReleaseManifest::file_sha256(tree.path("absent.bin"));

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("absent.bin"), std::string::npos);
}

TEST(ReleaseManifestTest, BuildProducesEntryPerUploadFile)
{
	TemporaryTree tree;
	const std::vector<std::string> files{ tree.write("bin/app.exe", "hello"), tree.write("bin/config.json", "") };

	const auto manifest = ReleaseManifest::build(files, "rel_20231114_221320", fixed_time());
	ASSERT_TRUE(manifest.has_value()) << (manifest.has_value() ? "" : manifest.error());

	ASSERT_EQ(manifest.value().artifacts.size(), 2u);
	EXPECT_EQ(manifest.value().release_id, "rel_20231114_221320");
	EXPECT_EQ(manifest.value().created_at, "2023-11-14T22:13:20Z");

	EXPECT_EQ(manifest.value().artifacts[0].install_path, "app.exe");
	EXPECT_EQ(manifest.value().artifacts[0].sha256, kHelloSha256);
	EXPECT_EQ(manifest.value().artifacts[0].size_bytes, 5u);

	EXPECT_EQ(manifest.value().artifacts[1].install_path, "config.json");
	EXPECT_EQ(manifest.value().artifacts[1].sha256, kEmptySha256);
	EXPECT_EQ(manifest.value().artifacts[1].size_bytes, 0u);
}

TEST(ReleaseManifestTest, BuildRejectsMissingFileAndEmptyInput)
{
	TemporaryTree tree;

	const auto missing = ReleaseManifest::build({ tree.path("absent.exe") }, "rel_1", fixed_time());
	ASSERT_FALSE(missing.has_value());
	EXPECT_NE(missing.error().find("does not exist"), std::string::npos);

	const auto empty = ReleaseManifest::build({}, "rel_1", fixed_time());
	ASSERT_FALSE(empty.has_value());

	const auto no_id = ReleaseManifest::build({ tree.write("a.txt", "a") }, "", fixed_time());
	ASSERT_FALSE(no_id.has_value());
}

// 서로 다른 디렉터리의 같은 파일명은 설치 시 서로를 덮어쓴다
TEST(ReleaseManifestTest, BuildRejectsDuplicateInstallPath)
{
	TemporaryTree tree;
	const std::vector<std::string> files{ tree.write("debug/app.exe", "one"), tree.write("release/app.exe", "two") };

	const auto manifest = ReleaseManifest::build(files, "rel_1", fixed_time());

	ASSERT_FALSE(manifest.has_value());
	EXPECT_NE(manifest.error().find("duplicate install path"), std::string::npos);
}

TEST(ReleaseManifestTest, SerializeParseRoundTrip)
{
	TemporaryTree tree;
	const std::vector<std::string> files{ tree.write("app.exe", "hello"), tree.write("config.json", "") };

	const auto original = ReleaseManifest::build(files, "rel_20231114_221320", fixed_time());
	ASSERT_TRUE(original.has_value());

	const auto restored = ReleaseManifest::parse(ReleaseManifest::serialize(original.value()));
	ASSERT_TRUE(restored.has_value()) << (restored.has_value() ? "" : restored.error());

	EXPECT_EQ(restored.value().release_id, original.value().release_id);
	EXPECT_EQ(restored.value().created_at, original.value().created_at);
	ASSERT_EQ(restored.value().artifacts.size(), original.value().artifacts.size());

	for (size_t index = 0; index < restored.value().artifacts.size(); ++index)
	{
		EXPECT_EQ(restored.value().artifacts[index].install_path, original.value().artifacts[index].install_path);
		EXPECT_EQ(restored.value().artifacts[index].sha256, original.value().artifacts[index].sha256);
		EXPECT_EQ(restored.value().artifacts[index].size_bytes, original.value().artifacts[index].size_bytes);
	}
}

TEST(ReleaseManifestTest, ParseRejectsMissingRequiredFields)
{
	EXPECT_FALSE(ReleaseManifest::parse("{ not json").has_value());
	EXPECT_FALSE(ReleaseManifest::parse(R"([1,2,3])").has_value());
	EXPECT_FALSE(ReleaseManifest::parse(R"({"created_at":"x","artifacts":[]})").has_value());
	EXPECT_FALSE(ReleaseManifest::parse(R"({"release_id":"","created_at":"x","artifacts":[]})").has_value());
	EXPECT_FALSE(ReleaseManifest::parse(R"({"release_id":"r","created_at":"x"})").has_value());
	EXPECT_FALSE(ReleaseManifest::parse(R"({"release_id":"r","created_at":"x","artifacts":[{"sha256":"aa"}]})").has_value());
}

// 형식이 깨진 해시를 통과시키면 설치 전 검증이 무력화된다
TEST(ReleaseManifestTest, ParseRejectsMalformedSha256)
{
	const auto too_short = ReleaseManifest::parse(R"({"release_id":"r","created_at":"x","artifacts":[{"install_path":"a.exe","sha256":"abc"}]})");
	ASSERT_FALSE(too_short.has_value());
	EXPECT_NE(too_short.error().find("malformed sha256"), std::string::npos);

	const std::string non_hex(64, 'z');
	EXPECT_FALSE(ReleaseManifest::parse(R"({"release_id":"r","created_at":"x","artifacts":[{"install_path":"a.exe","sha256":")" + non_hex + R"("}]})").has_value());
}

// 계약이 확장되어도 구버전 Agent 가 죽지 않아야 한다
TEST(ReleaseManifestTest, ParseIgnoresUnknownFields)
{
	const std::string text = R"({"release_id":"r","created_at":"x","future_field":42,"artifacts":[{"install_path":"a.exe","sha256":")" + std::string(kHelloSha256)
							 + R"(","size_bytes":5,"future_entry_field":"ignored"}]})";

	const auto manifest = ReleaseManifest::parse(text);

	ASSERT_TRUE(manifest.has_value()) << (manifest.has_value() ? "" : manifest.error());
	ASSERT_EQ(manifest.value().artifacts.size(), 1u);
	EXPECT_EQ(manifest.value().artifacts[0].size_bytes, 5u);
}

TEST(ReleaseManifestTest, MakeReleaseIdUsesUtcTimestamp) { EXPECT_EQ(ReleaseManifest::make_release_id(fixed_time()), "rel_20231114_221320"); }
