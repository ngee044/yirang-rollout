#pragma once

#include "AgentMessage.h"

#include "IArtifactStore.h"
#include "IMessagePublisher.h"

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace YirangAgent
{
	// 받아둔 버전 저장 경로와 서비스 실행 경로는 서로 다르다. 다운로드는 version_root 에만 쌓이고,
	// 실제 서비스 교체(apply/rollback)만 service_root 를 건드린다.
	struct AgentOptions
	{
		std::string device_id;
		std::string group;

		// 전달받은 버전들이 <version_root>/<release_id>/ 로 쌓이는 곳.
		std::string version_root;

		// 서비스로 동작 중인 앱의 경로.
		std::string service_root;

		// 처리 결과를 되돌릴 큐. 비어 있으면 보고하지 않는다.
		std::string result_queue_url;
	};

	// 큐 메시지를 명령별 핸들러로 라우팅한다.
	// 조건 분기 대신 핸들러 맵을 쓰는 이유와 규약은 docs/CODING_CONVENTION.md 의 메시지 디스패치 절을 따른다.
	class AgentService
	{
	public:
		AgentService(const AgentOptions& options, std::shared_ptr<Artifact::IArtifactStore> store, std::shared_ptr<Messaging::IMessagePublisher> publisher = nullptr);

		// 큐에서 받은 원문 한 건을 처리한다.
		//
		// 반환값은 처리 성공 여부가 아니라 **메시지를 소비했는지**다. 소비자는 핸들러가 성공을
		// 반환할 때만 DeleteMessage 를 호출하므로, 재시도해도 결과가 같은 실패(파싱 불가·미등록
		// 명령·설치기 부재)까지 실패로 되돌리면 가시성 타임아웃마다 영구 재배달되고 결과 큐에
		// 같은 실패 보고가 무한히 쌓인다. 처리 성공/실패는 반환값과 별개로 언제나 결과 큐에 보고한다.
		auto handle(const std::string& raw_message) -> std::expected<void, std::string>;

		// 지원 명령 목록. 등록 누락을 테스트·진단에서 잡기 위해 노출한다.
		auto commands(void) const -> std::vector<std::string>;

		// 결과 보고용으로 마지막 처리 결과를 남긴다. 결과 큐 발행(R-018)이 붙기 전까지의 관찰 수단이다.
		auto last_report(void) const -> std::string;

	private:
		auto download_version(const std::string& message) -> std::expected<void, std::string>;
		auto apply_version(const std::string& message) -> std::expected<void, std::string>;
		auto current_status(const std::string& message) -> std::expected<void, std::string>;
		auto clean_old_version(const std::string& message) -> std::expected<void, std::string>;
		auto rollback_version(const std::string& message) -> std::expected<void, std::string>;

		auto version_directory(const std::string& release_id) const -> std::string;

		// 처리 결과를 결과 큐로 보고한다. 발행 실패가 처리 결과를 덮지 않도록 로그만 남긴다.
		// reply_queue_url 이 비어 있을 때만 설정의 기본 결과 큐로 폴백한다 (AgentMessage.h 의 봉투 계약).
		auto report(const std::string& command, const std::string& reply_queue_url, const std::expected<void, std::string>& outcome) -> void;

		AgentOptions options_;
		std::shared_ptr<Artifact::IArtifactStore> store_;
		std::shared_ptr<Messaging::IMessagePublisher> publisher_;

		std::map<std::string, std::function<std::expected<void, std::string>(const std::string&)>> messages_;

		std::string last_report_;
	};
} // namespace YirangAgent
