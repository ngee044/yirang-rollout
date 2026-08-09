#include <gtest/gtest.h>

#include "ArtifactKey.h"
#include "ReleaseManifest.h"
#include "S3ArtifactStore.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <random>

using namespace Artifact;

namespace
{
	// LocalStack·MinIO 가 없는 환경에서 실패하지 않도록 통합 테스트는 환경변수로 켠다.
	//   YIRANG_TEST_S3_ENDPOINT=http://localhost:4566 YIRANG_TEST_S3_BUCKET=yirang-test ctest
	auto environment(const char* name) -> std::string
	{
		const char* value = std::getenv(name);

		return (value == nullptr) ? std::string() : std::string(value);
	}

	auto integration_options(void) -> StoreOptions
	{
		StoreOptions options;
		options.endpoint = environment("YIRANG_TEST_S3_ENDPOINT");
		options.bucket = environment("YIRANG_TEST_S3_BUCKET");
		options.access_key = environment("YIRANG_TEST_S3_ACCESS_KEY");
		options.secret_key = environment("YIRANG_TEST_S3_SECRET_KEY");

		if (options.access_key.empty())
		{
			options.access_key = "test";
			options.secret_key = "test";
		}

		if (options.bucket.empty())
		{
			options.bucket = "yirang-test";
		}

		return options;
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
			root_ = std::filesystem::temp_directory_path() / ("yirang-artifact-test-" + token + "-" + std::to_string(++sequence));

			std::error_code error;
			std::filesystem::create_directories(root_, error);
		}

		~TemporaryTree(void)
		{
			std::error_code error;
			std::filesystem::remove_all(root_, error);
		}

		auto write(const std::string& name, const std::string& contents) -> std::string
		{
			const auto path = root_ / name;

			std::ofstream stream(path, std::ios::binary);
			stream << contents;
			stream.close();

			return path.string();
		}

		auto path(const std::string& name) const -> std::string { return (root_ / name).string(); }

	private:
		std::filesystem::path root_;
	};
}

// 객체 키는 릴리스 단위로 격리된다
TEST(ArtifactKeyTest, BuildsReleaseScopedKey)
{
	const auto key = make_object_key("rel_20260808_1", "bin/app.exe");

	ASSERT_TRUE(key.has_value()) << (key.has_value() ? "" : key.error());
	EXPECT_EQ(key.value(), "releases/rel_20260808_1/bin/app.exe");
}

TEST(ArtifactKeyTest, RejectsEmptyInput)
{
	EXPECT_FALSE(make_object_key("", "app.exe").has_value());
	EXPECT_FALSE(make_object_key("rel_1", "").has_value());
}

// release_id 에 구분자가 있으면 다른 릴리스 영역을 침범할 수 있다
TEST(ArtifactKeyTest, RejectsSeparatorInReleaseId)
{
	EXPECT_FALSE(make_object_key("rel/1", "app.exe").has_value());
	EXPECT_FALSE(make_object_key("rel\\1", "app.exe").has_value());
}

// install_path 는 배포 티켓으로 들어오는 외부 입력이다 — 경로 순회를 막아야 한다
TEST(ArtifactKeyTest, RejectsPathTraversal)
{
	const auto parent = make_object_key("rel_1", "../secret.txt");
	ASSERT_FALSE(parent.has_value());
	EXPECT_NE(parent.error().find(".."), std::string::npos);

	EXPECT_FALSE(make_object_key("rel_1", "bin/../../etc/passwd").has_value());
	EXPECT_FALSE(make_object_key("rel_1", "a/../b").has_value());
}

TEST(ArtifactKeyTest, RejectsAbsolutePath)
{
	EXPECT_FALSE(make_object_key("rel_1", "/etc/passwd").has_value());
	EXPECT_FALSE(make_object_key("rel_1", "C:/Windows/system32/app.exe").has_value());
}

// 역슬래시를 허용하면 같은 파일이 서로 다른 키로 저장된다
TEST(ArtifactKeyTest, RejectsBackslashSeparator) { EXPECT_FALSE(make_object_key("rel_1", "bin\\app.exe").has_value()); }

// '..' 를 포함하는 파일명 자체는 정상이다 (경로 세그먼트가 아니므로)
TEST(ArtifactKeyTest, AllowsDotsInsideFileName)
{
	const auto key = make_object_key("rel_1", "app..v2.exe");

	ASSERT_TRUE(key.has_value()) << (key.has_value() ? "" : key.error());
	EXPECT_EQ(key.value(), "releases/rel_1/app..v2.exe");
}

TEST(ArtifactStoreTest, RejectsOperationsWithoutBucket)
{
	StoreOptions options;
	options.endpoint = "http://127.0.0.1:1";
	options.access_key = "test";
	options.secret_key = "test";

	S3ArtifactStore store(options);

	EXPECT_FALSE(store.presign("releases/rel_1/app.exe").has_value());
	EXPECT_FALSE(store.exists("releases/rel_1/app.exe").has_value());
}

TEST(ArtifactStoreTest, RejectsUploadOfMissingSource)
{
	TemporaryTree tree;

	StoreOptions options;
	options.bucket = "yirang-test";
	options.endpoint = "http://127.0.0.1:1";
	options.access_key = "test";
	options.secret_key = "test";

	S3ArtifactStore store(options);

	const auto result = store.upload("releases/rel_1/app.exe", tree.path("absent.exe"));

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("not a regular file"), std::string::npos);
}

TEST(ArtifactStoreTest, RejectsNonPositivePresignExpiration)
{
	StoreOptions options;
	options.bucket = "yirang-test";
	options.endpoint = "http://127.0.0.1:1";
	options.presigned_url_expiration = std::chrono::seconds(0);

	S3ArtifactStore store(options);

	const auto result = store.presign("releases/rel_1/app.exe");

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("expiration"), std::string::npos);
}

// 업로드 → presign → 다운로드 → SHA-256 일치. LocalStack/MinIO 가 있을 때만 실행된다.
TEST(ArtifactStoreIntegrationTest, RoundTripPreservesContent)
{
	if (environment("YIRANG_TEST_S3_ENDPOINT").empty())
	{
		GTEST_SKIP() << "YIRANG_TEST_S3_ENDPOINT 가 없어 건너뜁니다 (LocalStack/MinIO 필요)";
	}

	TemporaryTree tree;
	const auto source = tree.write("app.exe", "yirang-rollout artifact payload");
	const auto expected = Release::ReleaseManifest::file_sha256(source);
	ASSERT_TRUE(expected.has_value());

	S3ArtifactStore store(integration_options());

	const auto key = make_object_key("rel_integration", "app.exe");
	ASSERT_TRUE(key.has_value());

	const auto uploaded = store.upload(key.value(), source);
	ASSERT_TRUE(uploaded.has_value()) << (uploaded.has_value() ? "" : uploaded.error());

	const auto present = store.exists(key.value());
	ASSERT_TRUE(present.has_value()) << (present.has_value() ? "" : present.error());
	EXPECT_TRUE(present.value());

	const auto url = store.presign(key.value());
	ASSERT_TRUE(url.has_value()) << (url.has_value() ? "" : url.error());
	EXPECT_NE(url.value().find(key.value()), std::string::npos);

	const auto destination = tree.path("downloaded/app.exe");
	const auto downloaded = store.download(key.value(), destination);
	ASSERT_TRUE(downloaded.has_value()) << (downloaded.has_value() ? "" : downloaded.error());

	const auto actual = Release::ReleaseManifest::file_sha256(destination);
	ASSERT_TRUE(actual.has_value());
	EXPECT_EQ(actual.value(), expected.value());
}
