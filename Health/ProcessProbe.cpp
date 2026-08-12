#include "ProcessProbe.h"

#include <format>

using namespace Process;

namespace Health
{
	ProcessProbe::ProcessProbe(std::shared_ptr<IProcessSupervisor> supervisor, const ProcessHandle& handle) : supervisor_(std::move(supervisor)), handle_(handle) {}

	auto ProcessProbe::probe(void) -> std::expected<void, std::string>
	{
		if (supervisor_ == nullptr)
		{
			return std::unexpected("process supervisor is not configured");
		}

		auto status = supervisor_->status(handle_);
		if (!status)
		{
			return std::unexpected(status.error());
		}

		switch (status.value().state)
		{
		case ProcessState::Running:
			return {};

		case ProcessState::Exited:
			return std::unexpected(std::format("process {} exited with code {}", handle_.id, status.value().exit_code.value_or(-1)));

		case ProcessState::Signaled:
			return std::unexpected(std::format("process {} terminated by signal {}", handle_.id, status.value().signal.value_or(-1)));

		case ProcessState::Unknown:
			break;
		}

		return std::unexpected(std::format("process {} state is unknown", handle_.id));
	}
}
