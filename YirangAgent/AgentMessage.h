#pragma once

#include <expected>
#include <string>

namespace YirangAgent
{
	struct AgentMessage
	{
		std::string command;
		std::string payload;

		std::string target_group;

		std::string reply_queue_url;
	};

	namespace Commands
	{
		inline constexpr auto kDownloadVersion = "download_version";
		inline constexpr auto kApplyVersion = "apply_version";
		inline constexpr auto kCurrentStatus = "current_status";
		inline constexpr auto kCleanOldVersion = "clean_old_version";
		inline constexpr auto kRollbackVersion = "rollback_version";
	}

	auto parse_agent_message(const std::string& text) -> std::expected<AgentMessage, std::string>;
	auto serialize_agent_message(const AgentMessage& message) -> std::string;
}
