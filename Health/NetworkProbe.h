#pragma once

#include "HealthTypes.h"
#include "IHealthProbe.h"

namespace Health
{
	// §22.1 TCP check · HTTP check. 두 경우 모두 libcurl 로 처리해 플랫폼별 소켓 코드를 두지 않는다
	// (TCP 는 CONNECT_ONLY 로 연결만 확인한다).
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
} // namespace Health
