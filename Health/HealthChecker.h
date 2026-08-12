#pragma once

#include "HealthTypes.h"
#include "IHealthProbe.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Health
{
	class HealthChecker
	{
	public:
		HealthChecker(const HealthCheckSpec& spec, std::shared_ptr<IHealthProbe> probe);

		auto evaluate(void) -> HealthState;

		auto state(void) const -> HealthState;
		auto spec(void) const -> HealthCheckSpec;

		auto consecutive_successes(void) const -> int32_t;
		auto consecutive_failures(void) const -> int32_t;

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
}
