#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace Deploy
{
	struct ServiceSpec
	{
		std::string executable;

		std::vector<std::string> arguments;

		std::string working_directory;

		std::chrono::seconds stop_timeout{ 30 };

		std::chrono::seconds startup_timeout{ 60 };
	};
}
