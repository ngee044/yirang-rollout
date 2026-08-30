#pragma once

#include "IHealthProbe.h"

#include "IProcessSupervisor.h"

#include <memory>

namespace Health
{
	class ProcessProbe : public IHealthProbe
	{
	public:
		ProcessProbe(std::shared_ptr<Process::IProcessSupervisor> supervisor, const Process::ProcessHandle& handle);

		auto probe(void) -> std::expected<void, std::string> override;

	private:
		std::shared_ptr<Process::IProcessSupervisor> supervisor_;
		Process::ProcessHandle handle_;
	};
}
