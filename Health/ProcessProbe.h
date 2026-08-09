#pragma once

#include "IHealthProbe.h"

#include "IProcessSupervisor.h"

#include <memory>

namespace Health
{
	// §22.1 Process check — 관리 프로세스가 살아 있는지만 확인한다.
	class ProcessProbe : public IHealthProbe
	{
	public:
		ProcessProbe(std::shared_ptr<Process::IProcessSupervisor> supervisor, const Process::ProcessHandle& handle);

		auto probe(void) -> std::expected<void, std::string> override;

	private:
		std::shared_ptr<Process::IProcessSupervisor> supervisor_;
		Process::ProcessHandle handle_;
	};
} // namespace Health
