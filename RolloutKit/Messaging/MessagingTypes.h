#pragma once

#include <string>

namespace Messaging
{
	struct QueueOptions
	{
		std::string queue_url;
		std::string region{ "us-east-1" };

		std::string access_key;
		std::string secret_key;

		std::string endpoint;

		bool allow_insecure_tls{ false };

		int wait_time_seconds{ 20 };

		int visibility_timeout_seconds{ 300 };

		int max_number_of_messages{ 0 };
	};
}
