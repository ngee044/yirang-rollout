#pragma once

#include "HealthTypes.h"
#include "IHealthProbe.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Health
{
	// 연속 성공/실패를 세어 상태를 확정한다 (FR-HLT-02, 아키텍처 §22.3).
	// 임계값에 도달하기 전에는 직전 판정을 유지해 일시적 흔들림으로 롤백이 걸리지 않게 한다.
	class HealthChecker
	{
	public:
		HealthChecker(const HealthCheckSpec& spec, std::shared_ptr<IHealthProbe> probe);

		// 한 번 시도하고 누적 결과로 갱신된 상태를 돌려준다.
		auto evaluate(void) -> HealthState;

		auto state(void) const -> HealthState;
		auto spec(void) const -> HealthCheckSpec;

		auto consecutive_successes(void) const -> int32_t;
		auto consecutive_failures(void) const -> int32_t;

		// 마지막 실패 사유. 성공 시 비워진다.
		auto last_error(void) const -> std::optional<std::string>;

		auto reset(void) -> void;

	private:
		HealthCheckSpec spec_;
		std::shared_ptr<IHealthProbe> probe_;

		HealthState state_;
		int32_t consecutive_successes_;
		int32_t consecutive_failures_;
		std::optional<std::string> last_error_;
	};
} // namespace Health
