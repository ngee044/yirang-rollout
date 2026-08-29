#include "S3ArtifactStore.h"

#include "AWSS3Client.h"

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>

#include <filesystem>
#include <format>
#include <mutex>

using namespace AWSService;

namespace Artifact
{
	namespace
	{
		class AwsRuntime
		{
		public:
			AwsRuntime(void) { Aws::InitAPI(options_); }
			~AwsRuntime(void) { Aws::ShutdownAPI(options_); }

		private:
			Aws::SDKOptions options_;
		};

		auto ensure_runtime(void) -> void
		{
			static AwsRuntime runtime;
			(void)runtime;
		}

		auto make_configuration(const StoreOptions& options) -> Aws::Client::ClientConfiguration
		{
			Aws::Client::ClientConfiguration configuration;
			configuration.region = options.region.c_str();

			if (!options.endpoint.empty())
			{
				configuration.endpointOverride = options.endpoint.c_str();
			}

			if (options.allow_insecure_tls)
			{
				configuration.verifySSL = false;
			}

			return configuration;
		}

		auto make_client(const StoreOptions& options) -> std::unique_ptr<AWSS3Client>
		{
			const auto configuration = make_configuration(options);

			const bool use_virtual_addressing = options.endpoint.empty();

			if (!options.access_key.empty())
			{
				return std::make_unique<AWSS3Client>(Aws::String(options.access_key.c_str()), Aws::String(options.secret_key.c_str()), configuration,
													 use_virtual_addressing);
			}

			return std::make_unique<AWSS3Client>(configuration, use_virtual_addressing);
		}
	}

	S3ArtifactStore::S3ArtifactStore(const StoreOptions& options) : options_(options)
	{
		ensure_runtime();
		client_ = make_client(options_);
	}

	S3ArtifactStore::~S3ArtifactStore(void) = default;

	auto S3ArtifactStore::options(void) const -> StoreOptions { return options_; }

	auto S3ArtifactStore::upload(const std::string& object_key, const std::string& local_path) -> std::expected<void, std::string>
	{
		if (options_.bucket.empty())
		{
			return std::unexpected("bucket is required");
		}

		std::error_code error;
		if (!std::filesystem::is_regular_file(local_path, error))
		{
			return std::unexpected(std::format("upload source is not a regular file: {}", local_path));
		}

		auto result = client_->upload_file(Aws::String(options_.bucket.c_str()), Aws::String(object_key.c_str()), Aws::String(local_path.c_str()));
		if (!result)
		{
			return std::unexpected(std::format("cannot upload '{}' to '{}': {}", local_path, object_key, result.error()));
		}

		return {};
	}

	auto S3ArtifactStore::presign(const std::string& object_key) -> std::expected<std::string, std::string>
	{
		if (options_.bucket.empty())
		{
			return std::unexpected("bucket is required");
		}

		const auto expiration = static_cast<int>(options_.presigned_url_expiration.count());
		if (expiration <= 0)
		{
			return std::unexpected("presigned_url_expiration must be positive");
		}

		auto result = client_->generate_presigned_url(Aws::String(options_.bucket.c_str()), Aws::String(object_key.c_str()), expiration);
		if (!result)
		{
			return std::unexpected(std::format("cannot presign '{}': {}", object_key, result.error()));
		}

		return result.value();
	}

	auto S3ArtifactStore::download(const std::string& object_key, const std::string& local_path) -> std::expected<void, std::string>
	{
		if (options_.bucket.empty())
		{
			return std::unexpected("bucket is required");
		}

		std::error_code error;
		const auto parent = std::filesystem::path(local_path).parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, error);
			if (error)
			{
				return std::unexpected(std::format("cannot create directory '{}': {}", parent.string(), error.message()));
			}
		}

		auto result = client_->download_file(Aws::String(options_.bucket.c_str()), Aws::String(object_key.c_str()), Aws::String(local_path.c_str()));
		if (!result)
		{
			return std::unexpected(std::format("cannot download '{}' to '{}': {}", object_key, local_path, result.error()));
		}

		return {};
	}

	auto S3ArtifactStore::exists(const std::string& object_key) -> std::expected<bool, std::string>
	{
		if (options_.bucket.empty())
		{
			return std::unexpected("bucket is required");
		}

		auto result = client_->file_exists(Aws::String(options_.bucket.c_str()), Aws::String(object_key.c_str()));
		if (!result)
		{
			return std::unexpected(std::format("cannot query '{}': {}", object_key, result.error()));
		}

		return result.value();
	}
}
