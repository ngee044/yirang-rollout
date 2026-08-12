#include "DeploymentEngine.h"

#include "HealthChecker.h"
#include "NetworkProbe.h"
#include "ProcessProbe.h"

#include <filesystem>
#include <format>
#include <thread>

namespace Deploy
{
	DeploymentEngine::DeploymentEngine(std::shared_ptr<Install::ReleaseInstaller> installer,
									   std::shared_ptr<Process::IProcessSupervisor> supervisor,
									   const ServiceSpec& service,
									   const Health::HealthCheckSpec& health)
		: installer_(std::move(installer)), supervisor_(std::move(supervisor)), service_(service), health_(health), handle_(std::nullopt), last_detail_()
	{
	}

	auto DeploymentEngine::last_detail(void) const -> std::string { return last_detail_; }

	auto DeploymentEngine::running(void) -> bool
	{
		if (handle_ == std::nullopt || supervisor_ == nullptr)
		{
			return false;
		}

		auto status = supervisor_->status(handle_.value());

		return status && status.value().state == Process::ProcessState::Running;
	}

	auto DeploymentEngine::apply(const std::string& release_id, const std::string& source_directory) -> std::expected<void, std::string>
	{
		if (installer_ == nullptr || supervisor_ == nullptr)
		{
			return std::unexpected("deployment engine is not configured");
		}

		if (service_.executable.empty())
		{
			return std::unexpected("service.executable is not configured — the agent does not know what to run");
		}

		auto installed = installer_->install(release_id, source_directory);
		if (!installed)
		{
			return std::unexpected(installed.error());
		}

		auto before = installer_->state();
		if (!before)
		{
			return std::unexpected(before.error());
		}

		auto stopped = stop();
		if (!stopped)
		{
			return std::unexpected(std::format("cannot stop the running service: {}", stopped.error()));
		}

		auto activated = installer_->activate(release_id);
		if (!activated)
		{
			auto restored = start_active();

			return std::unexpected(std::format("cannot activate '{}': {}{}", release_id, activated.error(),
											   restored ? " (previous release restarted)" : " (previous release did not restart)"));
		}

		auto launched = launch(release_id);
		if (!launched)
		{
			return recover(std::format("cannot start '{}': {}", release_id, launched.error()));
		}

		auto ready = await_ready();
		if (!ready)
		{
			return recover(std::format("'{}' did not become ready: {}", release_id, ready.error()));
		}

		auto pruned = installer_->prune();

		last_detail_ = std::format("applied release '{}'{}", release_id, pruned ? std::format(" (pruned {} old release(s))", pruned.value()) : std::string());

		return {};
	}

	auto DeploymentEngine::rollback_to(const std::string& release_id) -> std::expected<void, std::string>
	{
		if (installer_ == nullptr || supervisor_ == nullptr)
		{
			return std::unexpected("deployment engine is not configured");
		}

		std::error_code error;
		if (!std::filesystem::is_directory(installer_->release_directory(release_id), error))
		{
			return std::unexpected(std::format("cannot roll back to '{}': it is not installed", release_id));
		}

		auto stopped = stop();
		if (!stopped)
		{
			return std::unexpected(std::format("cannot stop the running service: {}", stopped.error()));
		}

		auto activated = installer_->activate(release_id);
		if (!activated)
		{
			return std::unexpected(activated.error());
		}

		auto launched = launch(release_id);
		if (!launched)
		{
			return std::unexpected(std::format("rolled back to '{}' but it did not start: {}", release_id, launched.error()));
		}

		last_detail_ = std::format("rolled back to release '{}'", release_id);

		return {};
	}

	auto DeploymentEngine::start_active(void) -> std::expected<void, std::string>
	{
		if (installer_ == nullptr || supervisor_ == nullptr)
		{
			return std::unexpected("deployment engine is not configured");
		}

		auto current = installer_->state();
		if (!current)
		{
			return std::unexpected(current.error());
		}

		if (current.value().active.empty())
		{
			return std::unexpected("no active release to start");
		}

		return launch(current.value().active);
	}

	auto DeploymentEngine::stop(void) -> std::expected<void, std::string>
	{
		if (handle_ == std::nullopt)
		{
			return {};
		}

		auto stopped = supervisor_->stop(handle_.value(), service_.stop_timeout);
		handle_ = std::nullopt;

		if (!stopped)
		{
			return std::unexpected(stopped.error());
		}

		return {};
	}

	auto DeploymentEngine::launch(const std::string& release_id) -> std::expected<void, std::string>
	{
		const auto release_root = std::filesystem::path(installer_->release_directory(release_id));
		const auto executable = release_root / service_.executable;

		std::error_code error;
		if (!std::filesystem::exists(executable, error))
		{
			return std::unexpected(std::format("executable '{}' is missing from the release", service_.executable));
		}

		const auto mode = std::filesystem::status(executable, error).permissions();
		if (!error && (mode & std::filesystem::perms::owner_exec) == std::filesystem::perms::none)
		{
			std::filesystem::permissions(executable, mode | std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec, error);
			if (error)
			{
				return std::unexpected(std::format("cannot make '{}' executable: {}", service_.executable, error.message()));
			}
		}

		error.clear();

		Process::ProcessStartOptions options;
		options.executable_path = executable.string();
		options.arguments = service_.arguments;
		options.working_directory = service_.working_directory.empty() ? release_root.string() : service_.working_directory;

		auto started = supervisor_->start(options);
		if (!started)
		{
			return std::unexpected(started.error());
		}

		handle_ = started.value();

		return {};
	}

	auto DeploymentEngine::make_probe(void) -> std::shared_ptr<Health::IHealthProbe>
	{
		if (health_.kind == Health::CheckKind::Process)
		{
			if (handle_ == std::nullopt)
			{
				return nullptr;
			}

			return std::make_shared<Health::ProcessProbe>(supervisor_, handle_.value());
		}

		return std::make_shared<Health::NetworkProbe>(health_);
	}

	auto DeploymentEngine::await_ready(void) -> std::expected<void, std::string>
	{
		auto probe = make_probe();
		if (probe == nullptr)
		{
			return std::unexpected("cannot build a health probe for the started process");
		}

		auto spec = health_;
		spec.purpose = Health::CheckPurpose::Readiness;

		Health::HealthChecker checker(spec, probe);

		const auto deadline = std::chrono::steady_clock::now() + service_.startup_timeout;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (checker.evaluate() == Health::HealthState::Healthy)
			{
				return {};
			}

			if (checker.state() == Health::HealthState::Unhealthy)
			{
				return std::unexpected(checker.last_error().value_or("health check failed"));
			}

			std::this_thread::sleep_for(spec.interval);
		}

		return std::unexpected(
			std::format("readiness not confirmed within {}s: {}", service_.startup_timeout.count(), checker.last_error().value_or("no successful probe")));
	}

	auto DeploymentEngine::recover(const std::string& reason) -> std::expected<void, std::string>
	{
		auto stopped = stop();
		if (!stopped)
		{
			return std::unexpected(std::format("{}; and the failed release did not stop: {}", reason, stopped.error()));
		}

		auto reverted = installer_->rollback();
		if (!reverted)
		{
			return std::unexpected(std::format("{}; and no rollback target exists: {}", reason, reverted.error()));
		}

		auto launched = launch(reverted.value());
		if (!launched)
		{
			return std::unexpected(std::format("{}; rolled back to '{}' but it did not start: {}", reason, reverted.value(), launched.error()));
		}

		last_detail_ = std::format("{}; rolled back to '{}'", reason, reverted.value());

		return std::unexpected(last_detail_);
	}
}
