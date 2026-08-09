#pragma once

#include "ProcessTypes.h"

#include <chrono>
#include <expected>
#include <string>

namespace Process
{
	// 플랫폼별 프로세스 제어를 이 경계 뒤에 둔다 (FR-PRC-03, 아키텍처 §17).
	// 배포 엔진은 이 인터페이스만 보고 작성하므로 Windows 구현이 추가되어도 엔진은 바뀌지 않는다.
	class IProcessSupervisor
	{
	public:
		virtual ~IProcessSupervisor(void) = default;

		virtual auto start(const ProcessStartOptions& options) -> std::expected<ProcessHandle, std::string> = 0;

		// graceful 종료 요청 후 timeout 안에 끝나지 않으면 강제 종료로 폴백한다 (FR-PRC-01).
		virtual auto stop(const ProcessHandle& handle, std::chrono::seconds timeout) -> std::expected<void, std::string> = 0;

		// 종료 상태는 한 번만 수확되므로 구현체가 캐시한다. 그래서 const 가 아니다
		// (아키텍처 §17.1 예시와 다른 지점 — exit code 를 잃지 않기 위한 의도적 차이).
		virtual auto status(const ProcessHandle& handle) -> std::expected<ProcessStatus, std::string> = 0;
	};
} // namespace Process
