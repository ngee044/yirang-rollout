#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace Health
{
	enum class CheckKind : uint8_t
	{
		Process,
		Tcp,
		Http
	};

	enum class CheckPurpose : uint8_t
	{
		Readiness,
		Liveness
	};

	enum class HealthState : uint8_t
	{
		Unknown,
		Healthy,
		Unhealthy
	};

	struct HealthCheckSpec
	{
		CheckKind kind{ CheckKind::Process };
		CheckPurpose purpose{ CheckPurpose::Liveness };

		std::string host{ "127.0.0.1" };
		uint16_t port{ 0 };

		std::string path{ "/" };
		int32_t expected_status{ 200 };

		std::string expected_body;

		std::chrono::milliseconds timeout{ 2000 };
		std::chrono::milliseconds interval{ 1000 };

		int32_t success_threshold{ 1 };
		int32_t failure_threshold{ 3 };
	};
}
