#pragma once

#include <chrono>
#include <string>

namespace Artifact
{
	struct StoreOptions
	{
		std::string bucket;
		std::string region{ "us-east-1" };

		std::string access_key;
		std::string secret_key;

		std::string endpoint;

		std::chrono::seconds presigned_url_expiration{ 3600 };
	};
}
