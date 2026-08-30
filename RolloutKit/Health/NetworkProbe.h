#pragma once

#include "HealthTypes.h"
#include "IHealthProbe.h"

namespace Health
{
	class NetworkProbe : public IHealthProbe
	{
	public:
		explicit NetworkProbe(const HealthCheckSpec& spec);

		auto probe(void) -> std::expected<void, std::string> override;

	private:
		auto probe_tcp(void) -> std::expected<void, std::string>;
		auto probe_http(void) -> std::expected<void, std::string>;

		HealthCheckSpec spec_;
	};
}
