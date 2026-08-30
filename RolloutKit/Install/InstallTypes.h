#pragma once

#include <cstdint>
#include <string>

namespace Install
{
	struct InstallOptions
	{
		std::string service_root;

		int keep_previous_releases{ 2 };
	};

	struct ActivationState
	{
		std::string active;
		std::string previous;

		std::string updated_at;
	};
}
