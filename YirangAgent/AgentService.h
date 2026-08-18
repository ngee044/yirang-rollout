#pragma once

#include "AgentMessage.h"

#include "DeploymentEngine.h"

#include "IArtifactStore.h"
#include "IMessagePublisher.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace YirangAgent
{
	inline constexpr uint32_t kMaxTransientAttempts = 5;

	struct AgentOptions
	{
		std::string device_id;
		std::string group;

		std::string version_root;

		std::string service_root;

		std::string result_queue_url;
	};

	class AgentService
	{
	public:
		AgentService(const AgentOptions& options,
					 std::shared_ptr<Artifact::IArtifactStore> store,
					 std::shared_ptr<Messaging::IMessagePublisher> publisher = nullptr,
					 std::shared_ptr<Deploy::DeploymentEngine> engine = nullptr);

		auto handle(const std::string& raw_message) -> std::expected<void, std::string>;

		auto commands(void) const -> std::vector<std::string>;

		auto last_report(void) const -> std::string;

	private:
		auto download_version(const std::string& message) -> std::expected<void, std::string>;
		auto apply_version(const std::string& message) -> std::expected<void, std::string>;
		auto current_status(const std::string& message) -> std::expected<void, std::string>;
		auto clean_old_version(const std::string& message) -> std::expected<void, std::string>;
		auto rollback_version(const std::string& message) -> std::expected<void, std::string>;

		auto version_directory(const std::string& release_id) const -> std::string;

		auto report(const std::string& command, const std::string& reply_queue_url, const std::expected<void, std::string>& outcome) -> void;

		auto permanent(const std::string& reason) -> std::unexpected<std::string>;

		auto record_failure(const std::string& raw_message) -> uint32_t;
		auto forget_failure(void) -> void;

		AgentOptions options_;
		std::shared_ptr<Artifact::IArtifactStore> store_;
		std::shared_ptr<Messaging::IMessagePublisher> publisher_;
		std::shared_ptr<Deploy::DeploymentEngine> engine_;

		std::map<std::string, std::function<std::expected<void, std::string>(const std::string&)>> messages_;

		std::string last_report_;

		bool failure_is_permanent_;

		std::string failing_message_;
		uint32_t failing_attempts_;
	};
}
