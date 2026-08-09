#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace Health
{
	// 아키텍처 §22.1. Command 체크(FR-HLT-03)는 MVP 2 이후 범위이므로 여기 없다.
	enum class CheckKind : uint8_t
	{
		Process,
		Tcp,
		Http
	};

	// §22.2 — readiness 는 트래픽 전환 가능 여부를, liveness 는 재시작·롤백 여부를 판단한다.
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

		// Tcp·Http 전용. 로컬 프로세스를 대상으로 하므로 기본은 루프백이다.
		std::string host{ "127.0.0.1" };
		uint16_t port{ 0 };

		// Http 전용.
		std::string path{ "/" };
		int32_t expected_status{ 200 };

		// 비어 있으면 본문을 검사하지 않는다. 채워지면 부분 문자열로 확인한다 (§22.1).
		std::string expected_body;

		std::chrono::milliseconds timeout{ 2000 };
		std::chrono::milliseconds interval{ 1000 };

		// §22.3 — 연속 성공/실패 횟수로 상태를 확정한다 (FR-HLT-02).
		int32_t success_threshold{ 1 };
		int32_t failure_threshold{ 3 };
	};
} // namespace Health
