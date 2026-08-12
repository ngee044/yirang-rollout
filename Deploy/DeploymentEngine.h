#pragma once

#include "DeployTypes.h"

#include "HealthTypes.h"
#include "IHealthProbe.h"
#include "IProcessSupervisor.h"
#include "ReleaseInstaller.h"

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace Deploy
{
	class DeploymentEngine
	{
	public:
		DeploymentEngine(std::shared_ptr<Install::ReleaseInstaller> installer,
						 std::shared_ptr<Process::IProcessSupervisor> supervisor,
						 const ServiceSpec& service,
						 const Health::HealthCheckSpec& health);

		auto apply(const std::string& release_id, const std::string& source_directory) -> std::expected<void, std::string>;

		auto rollback_to(const std::string& release_id) -> std::expected<void, std::string>;

		auto start_active(void) -> std::expected<void, std::string>;

		auto stop(void) -> std::expected<void, std::string>;

		auto running(void) -> bool;

		auto last_detail(void) const -> std::string;

	private:
		auto launch(const std::string& release_id) -> std::expected<void, std::string>;
		auto await_ready(void) -> std::expected<void, std::string>;
		auto make_probe(void) -> std::shared_ptr<Health::IHealthProbe>;

		auto recover(const std::string& reason) -> std::expected<void, std::string>;

		std::shared_ptr<Install::ReleaseInstaller> installer_;
		std::shared_ptr<Process::IProcessSupervisor> supervisor_;

		ServiceSpec service_;
		Health::HealthCheckSpec health_;

		std::optional<Process::ProcessHandle> handle_;

		std::string last_detail_;
	};
}
