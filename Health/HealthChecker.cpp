#include "HealthChecker.h"

namespace Health
{
	HealthChecker::HealthChecker(const HealthCheckSpec& spec, std::shared_ptr<IHealthProbe> probe)
		: spec_(spec), probe_(std::move(probe)), state_(HealthState::Unknown), consecutive_successes_(0), consecutive_failures_(0), last_error_(std::nullopt)
	{
		if (spec_.success_threshold <= 0)
		{
			spec_.success_threshold = 1;
		}

		if (spec_.failure_threshold <= 0)
		{
			spec_.failure_threshold = 1;
		}
	}

	auto HealthChecker::evaluate(void) -> HealthState
	{
		if (probe_ == nullptr)
		{
			consecutive_successes_ = 0;
			++consecutive_failures_;
			last_error_ = "health probe is not configured";

			if (consecutive_failures_ >= spec_.failure_threshold)
			{
				state_ = HealthState::Unhealthy;
			}

			return state_;
		}

		auto result = probe_->probe();
		if (result)
		{
			++consecutive_successes_;
			consecutive_failures_ = 0;
			last_error_ = std::nullopt;

			if (consecutive_successes_ >= spec_.success_threshold)
			{
				state_ = HealthState::Healthy;
			}

			return state_;
		}

		consecutive_successes_ = 0;
		++consecutive_failures_;
		last_error_ = result.error();

		if (consecutive_failures_ >= spec_.failure_threshold)
		{
			state_ = HealthState::Unhealthy;
		}

		return state_;
	}

	auto HealthChecker::state(void) const -> HealthState { return state_; }

	auto HealthChecker::spec(void) const -> HealthCheckSpec { return spec_; }

	auto HealthChecker::consecutive_successes(void) const -> int32_t { return consecutive_successes_; }

	auto HealthChecker::consecutive_failures(void) const -> int32_t { return consecutive_failures_; }

	auto HealthChecker::last_error(void) const -> std::optional<std::string> { return last_error_; }

	auto HealthChecker::reset(void) -> void
	{
		state_ = HealthState::Unknown;
		consecutive_successes_ = 0;
		consecutive_failures_ = 0;
		last_error_ = std::nullopt;
	}
}
