#pragma once

#include <expected>
#include <string>

namespace YirangAgent
{
	// 큐로 오가는 봉투. command 로 핸들러를 찾고 payload 는 각 핸들러가 해석한다.
	// 핸들러마다 필드가 다르므로 여기서는 payload 를 원문 그대로 들고 전달한다.
	struct AgentMessage
	{
		std::string command;
		std::string payload;

		// 결과를 어느 큐로 되돌릴지. 비어 있으면 설정의 기본 결과 큐를 쓴다.
		std::string reply_queue_url;
	};

	// 지원 명령. 문자열 오타를 컴파일 시점에 잡기 위해 상수로 둔다.
	namespace Commands
	{
		// 새 버전이든 이전 버전이든 "릴리스를 version_root 로 내려받는다"는 동작이 같으므로 하나로 둔다.
		inline constexpr auto kDownloadVersion = "download_version";
		inline constexpr auto kApplyVersion = "apply_version";
		inline constexpr auto kCurrentStatus = "current_status";
		inline constexpr auto kCleanOldVersion = "clean_old_version";
		inline constexpr auto kRollbackVersion = "rollback_version";
	} // namespace Commands

	auto parse_agent_message(const std::string& text) -> std::expected<AgentMessage, std::string>;
	auto serialize_agent_message(const AgentMessage& message) -> std::string;
} // namespace YirangAgent
