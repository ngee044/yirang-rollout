#include "DeploymentEngine.h"

#include "HealthChecker.h"
#include "NetworkProbe.h"
#include "ProcessIdentity.h"
#include "ProcessProbe.h"

#include <boost/json.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
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
			return std::unexpected(kNoActiveRelease);
		}

		auto resumed = resume(current.value().active);
		if (!resumed)
		{
			return std::unexpected(resumed.error());
		}

		if (handle_ != std::nullopt)
		{
			return {};
		}

		return launch(current.value().active);
	}

	auto DeploymentEngine::resume(const std::string& release_id) -> std::expected<void, std::string>
	{
		auto recorded = record();
		if (!recorded)
		{
			return std::unexpected(recorded.error());
		}

		if (recorded.value().process_id <= 0)
		{
			return {};
		}

		auto identity = Process::boot_identity();
		if (!identity)
		{
			return std::unexpected(identity.error());
		}

		if (identity.value() != recorded.value().boot_identity)
		{
			last_detail_ = std::format("the recorded instance of '{}' belongs to an earlier boot", recorded.value().release_id);

			return clear_record();
		}

		auto taken = supervisor_->adopt(Process::ProcessHandle{ recorded.value().process_id, recorded.value().start_token });
		if (!taken)
		{
			last_detail_ = std::format("the recorded instance of '{}' is gone: {}", recorded.value().release_id, taken.error());

			return clear_record();
		}

		handle_ = taken.value();

		if (recorded.value().release_id == release_id)
		{
			last_detail_ = std::format("adopted the running instance of '{}' (pid {})", release_id, taken.value().id);

			return {};
		}

		auto stopped = stop();
		if (!stopped)
		{
			return std::unexpected(std::format("cannot stop the adopted instance of '{}': {}", recorded.value().release_id, stopped.error()));
		}

		last_detail_ = std::format("stopped the adopted instance of '{}' before starting '{}'", recorded.value().release_id, release_id);

		return {};
	}

	auto DeploymentEngine::stop(void) -> std::expected<void, std::string>
	{
		if (handle_ == std::nullopt)
		{
			return {};
		}

		auto stopped = supervisor_->stop(handle_.value(), service_.stop_timeout);
		if (!stopped)
		{
			return std::unexpected(stopped.error());
		}

		handle_ = std::nullopt;

		return clear_record();
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

		auto identity = Process::boot_identity();
		if (!identity)
		{
			return std::unexpected(identity.error());
		}

		RuntimeRecord next;
		next.release_id = release_id;
		next.process_id = started.value().id;
		next.start_token = started.value().start_token;
		next.boot_identity = identity.value();

		auto written = write_record(next);
		if (!written)
		{
			return std::unexpected(std::format("started '{}' but cannot record the running instance: {}", release_id, written.error()));
		}

		return {};
	}

	auto DeploymentEngine::record_path(void) const -> std::string { return (std::filesystem::path(installer_->options().service_root) / "runtime.json").string(); }

	auto DeploymentEngine::record(void) -> std::expected<RuntimeRecord, std::string>
	{
		std::error_code error;
		if (!std::filesystem::exists(record_path(), error))
		{
			return RuntimeRecord{};
		}

		std::ifstream source(record_path(), std::ios::binary);
		if (!source.is_open())
		{
			return std::unexpected(std::format("cannot open '{}'", record_path()));
		}

		std::ostringstream buffer;
		buffer << source.rdbuf();
		source.close();

		boost::json::value parsed;
		try
		{
			parsed = boost::json::parse(buffer.str());
		}
		catch (const std::exception& exception)
		{
			return unreadable_record(std::format("cannot parse '{}': {}", record_path(), exception.what()));
		}

		if (!parsed.is_object())
		{
			return unreadable_record(std::format("'{}' root is not a JSON object", record_path()));
		}

		const auto root = parsed.as_object();

		RuntimeRecord loaded;
		if (root.contains("release_id") && root.at("release_id").is_string())
		{
			loaded.release_id = root.at("release_id").as_string().c_str();
		}

		if (root.contains("process_id") && root.at("process_id").is_int64())
		{
			loaded.process_id = root.at("process_id").as_int64();
		}

		if (root.contains("start_token") && root.at("start_token").is_int64())
		{
			loaded.start_token = static_cast<uint64_t>(root.at("start_token").as_int64());
		}

		if (root.contains("boot_identity") && root.at("boot_identity").is_string())
		{
			loaded.boot_identity = root.at("boot_identity").as_string().c_str();
		}

		return loaded;
	}

	auto DeploymentEngine::unreadable_record(const std::string& reason) -> std::expected<RuntimeRecord, std::string>
	{
		last_detail_ = reason;

		auto cleared = clear_record();
		if (!cleared)
		{
			return std::unexpected(std::format("{} and {}", reason, cleared.error()));
		}

		return RuntimeRecord{};
	}

	auto DeploymentEngine::write_record(const RuntimeRecord& next) const -> std::expected<void, std::string>
	{
		boost::json::object root;
		root["release_id"] = next.release_id;
		root["process_id"] = next.process_id;
		root["start_token"] = static_cast<int64_t>(next.start_token);
		root["boot_identity"] = next.boot_identity;

		const auto temporary = record_path() + ".tmp";
		{
			std::ofstream sink(temporary, std::ios::binary | std::ios::trunc);
			if (!sink.is_open())
			{
				return std::unexpected(std::format("cannot write '{}'", temporary));
			}

			sink << boost::json::serialize(root);
			sink.flush();
			if (!sink.good())
			{
				return std::unexpected(std::format("cannot write '{}'", temporary));
			}
		}

		std::error_code error;
		std::filesystem::rename(temporary, record_path(), error);
		if (error)
		{
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);

			return std::unexpected(std::format("cannot publish '{}': {}", record_path(), error.message()));
		}

		return {};
	}

	auto DeploymentEngine::clear_record(void) const -> std::expected<void, std::string>
	{
		std::error_code error;
		std::filesystem::remove(record_path(), error);
		if (error)
		{
			return std::unexpected(std::format("cannot remove '{}': {}", record_path(), error.message()));
		}

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
