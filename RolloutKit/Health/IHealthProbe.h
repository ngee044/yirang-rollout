#pragma once

#include <expected>
#include <string>

namespace Health
{
	class IHealthProbe
	{
	public:
		virtual ~IHealthProbe(void) = default;

		virtual auto probe(void) -> std::expected<void, std::string> = 0;
	};
}
