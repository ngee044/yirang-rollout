#pragma once

#include <expected>
#include <string>

namespace Health
{
	// 단일 시도. 임계값 누적은 HealthChecker 가 맡고, 여기서는 한 번의 판정만 한다.
	// I/O 를 이 경계 뒤에 가두어 임계값 로직을 네트워크 없이 검증할 수 있게 한다.
	class IHealthProbe
	{
	public:
		virtual ~IHealthProbe(void) = default;

		virtual auto probe(void) -> std::expected<void, std::string> = 0;
	};
} // namespace Health
