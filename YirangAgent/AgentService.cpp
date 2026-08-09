#include "AgentService.h"

#include "ArtifactKey.h"
#include "Logger.h"
#include "ReleaseManifest.h"

#include <boost/json.hpp>

#include <filesystem>
#include <thread>
#include <format>

using namespace Utilities;

namespace YirangAgent
{
	namespace
	{
		auto read_string(const boost::json::object& object, const char* key) -> std::expected<std::string, std::string>
		{
			if (!object.contains(key) || !object.at(key).is_string())
			{
				return std::unexpected(std::format("payload field '{}' is missing or not a string", key));
			}

			return std::string(object.at(key).as_string().c_str());
		}

		auto parse_object(const std::string& payload) -> std::expected<boost::json::object, std::string>
		{
			boost::json::value parsed;
			try
			{
				parsed = boost::json::parse(payload);
			}
			catch (const std::exception& exception)
			{
				return std::unexpected(std::format("cannot parse payload: {}", exception.what()));
			}

			if (!parsed.is_object())
			{
				return std::unexpected("payload is not a JSON object");
			}

			return parsed.as_object();
		}

		// 다운로드가 중도 실패하면 받아둔 파일과 버전 디렉터리를 되돌린다.
		class PartialDownloadGuard
		{
		public:
			explicit PartialDownloadGuard(std::string directory) : directory_(std::move(directory)), keep_(false) {}

			~PartialDownloadGuard(void)
			{
				if (keep_)
				{
					return;
				}

				std::error_code ignored;
				std::filesystem::remove_all(directory_, ignored);
			}

			PartialDownloadGuard(const PartialDownloadGuard&) = delete;
			auto operator=(const PartialDownloadGuard&) -> PartialDownloadGuard& = delete;

			auto keep(void) -> void { keep_ = true; }

		private:
			std::string directory_;
			bool keep_;
		};

		// 재시도해도 결과가 같은 명령. 릴리스 설치기(R-008)가 없어 구조적으로 항상 실패하므로
		// 실패를 큐로 되돌리면 영구 재배달만 남는다. R-008 구현 시 이 목록에서 제거한다.
		auto retry_is_pointless(const std::string& command) -> bool { return command == Commands::kApplyVersion || command == Commands::kRollbackVersion; }
	}

	AgentService::AgentService(const AgentOptions& options, std::shared_ptr<Artifact::IArtifactStore> store, std::shared_ptr<Messaging::IMessagePublisher> publisher)
		: options_(options), store_(std::move(store)), publisher_(std::move(publisher)), messages_(), last_report_()
	{
		// 지원 명령을 생성자에서 한 번에 등록해 목록이 한눈에 보이게 한다.
		messages_.insert({ Commands::kDownloadVersion, std::bind(&AgentService::download_version, this, std::placeholders::_1) });
		messages_.insert({ Commands::kApplyVersion, std::bind(&AgentService::apply_version, this, std::placeholders::_1) });
		messages_.insert({ Commands::kCurrentStatus, std::bind(&AgentService::current_status, this, std::placeholders::_1) });
		messages_.insert({ Commands::kCleanOldVersion, std::bind(&AgentService::clean_old_version, this, std::placeholders::_1) });
		messages_.insert({ Commands::kRollbackVersion, std::bind(&AgentService::rollback_version, this, std::placeholders::_1) });
	}

	auto AgentService::commands(void) const -> std::vector<std::string>
	{
		std::vector<std::string> names;
		names.reserve(messages_.size());

		for (const auto& [name, handler] : messages_)
		{
			names.push_back(name);
		}

		return names;
	}

	auto AgentService::last_report(void) const -> std::string { return last_report_; }

	auto AgentService::handle(const std::string& raw_message) -> std::expected<void, std::string>
	{
		auto parsed = parse_agent_message(raw_message);
		if (!parsed)
		{
			// 같은 본문을 다시 파싱해도 결과가 같다. 보고만 남기고 메시지는 소비한다.
			Logger::handle().write(LogTypes::Error, std::format("cannot parse agent message: {}", parsed.error()));
			report(std::string(), std::string(), std::expected<void, std::string>(std::unexpect, parsed.error()));

			return {};
		}

		const auto& message = parsed.value();

		auto iter = messages_.find(message.command);
		if (iter == messages_.end())
		{
			// 조용히 무시하면 발행 측이 처리된 줄 안다. 로그와 결과 보고를 남긴다.
			// 재배달해도 없는 핸들러가 생기지는 않으므로 메시지는 소비한다.
			Logger::handle().write(LogTypes::Error, std::format("command is not found: {}", message.command));
			report(message.command, message.reply_queue_url, std::expected<void, std::string>(std::unexpect, std::format("command is not found: {}", message.command)));

			return {};
		}

		auto outcome = iter->second(message.payload);
		report(message.command, message.reply_queue_url, outcome);

		if (!outcome && retry_is_pointless(message.command))
		{
			Logger::handle().write(LogTypes::Error, std::format("'{}' failed and will not be retried: {}", message.command, outcome.error()));

			return {};
		}

		return outcome;
	}

	auto AgentService::report(const std::string& command, const std::string& reply_queue_url, const std::expected<void, std::string>& outcome) -> void
	{
		// 봉투가 결과 큐를 지정하면 그쪽이 우선이다. 설정의 기본 큐는 폴백일 뿐이므로,
		// 디바이스 설정이 비어 있어도 발행 측이 알려준 큐로 보고가 나간다.
		const auto& destination = reply_queue_url.empty() ? options_.result_queue_url : reply_queue_url;

		if (publisher_ == nullptr || destination.empty())
		{
			return;
		}

		boost::json::object body;
		body["device_id"] = options_.device_id;
		body["group"] = options_.group;
		body["command"] = command;
		body["success"] = outcome.has_value();
		body["detail"] = outcome.has_value() ? last_report_ : outcome.error();

		auto sent = publisher_->send(destination, boost::json::serialize(body));
		if (!sent)
		{
			// 보고 실패가 처리 결과를 덮어써서는 안 된다. 로그로만 남긴다.
			Logger::handle().write(LogTypes::Error, std::format("cannot report result for '{}': {}", command, sent.error()));
		}
	}

	auto AgentService::version_directory(const std::string& release_id) const -> std::string
	{
		return (std::filesystem::path(options_.version_root) / release_id).string();
	}

	auto AgentService::download_version(const std::string& message) -> std::expected<void, std::string>
	{
		if (store_ == nullptr)
		{
			return std::unexpected("artifact store is not configured");
		}

		if (options_.version_root.empty())
		{
			return std::unexpected("version_root is not configured");
		}

		auto object = parse_object(message);
		if (!object)
		{
			return std::unexpected(object.error());
		}

		auto release_id = read_string(object.value(), "release_id");
		if (!release_id)
		{
			return std::unexpected(release_id.error());
		}

		if (!object.value().contains("artifacts") || !object.value().at("artifacts").is_array())
		{
			return std::unexpected("payload field 'artifacts' is missing or not an array");
		}

		// 빈 목록을 성공으로 처리하면 빈 버전 디렉터리가 정상 릴리스로 보고된다.
		if (object.value().at("artifacts").as_array().empty())
		{
			return std::unexpected("payload field 'artifacts' must not be empty");
		}

		const auto target = version_directory(release_id.value());

		std::error_code error;
		std::filesystem::create_directories(target, error);
		if (error)
		{
			return std::unexpected(std::format("cannot create version directory '{}': {}", target, error.message()));
		}

		// 중도 실패한 다운로드를 남기면 손상된 릴리스가 current_status 에 정상 버전으로 잡히고
		// apply_version 의 디렉터리 존재 검사도 통과한다. 성공 시에만 해제한다.
		auto discard_on_failure = PartialDownloadGuard{ target };

		for (const auto& element : object.value().at("artifacts").as_array())
		{
			if (!element.is_object())
			{
				return std::unexpected("artifact entry is not an object");
			}

			const auto entry = element.as_object();

			auto install_path = read_string(entry, "install_path");
			if (!install_path)
			{
				return std::unexpected(install_path.error());
			}

			auto sha256 = read_string(entry, "sha256");
			if (!sha256)
			{
				return std::unexpected(sha256.error());
			}

			// 경로 순회를 여기서도 막는다. 저장소 키와 로컬 경로가 같은 규칙을 따라야 한다.
			auto key = Artifact::make_object_key(release_id.value(), install_path.value());
			if (!key)
			{
				return std::unexpected(key.error());
			}

			const auto destination = (std::filesystem::path(target) / install_path.value()).string();

			auto downloaded = store_->download(key.value(), destination);
			if (!downloaded)
			{
				return std::unexpected(downloaded.error());
			}

			// 검증 전 아티팩트를 쓰지 않는다. 불일치는 즉시 실패다.
			auto actual = Release::ReleaseManifest::file_sha256(destination);
			if (!actual)
			{
				return std::unexpected(actual.error());
			}

			if (actual.value() != sha256.value())
			{
				return std::unexpected(std::format("sha256 mismatch for '{}': expected {}, got {}", install_path.value(), sha256.value(), actual.value()));
			}
		}

		discard_on_failure.keep();

		last_report_ = std::format("downloaded release '{}' into '{}'", release_id.value(), target);
		Logger::handle().write(LogTypes::Information, last_report_);

		return {};
	}

	auto AgentService::clean_old_version(const std::string& message) -> std::expected<void, std::string>
	{
		(void)message;

		if (options_.version_root.empty())
		{
			return std::unexpected("version_root is not configured");
		}

		std::error_code error;
		if (!std::filesystem::is_directory(options_.version_root, error))
		{
			// exists() 로만 보면 일반 파일도 통과하고, 그 뒤 순회가 0회 돌아 "0개 삭제 성공"이 된다.
			last_report_ = std::format("version_root '{}' is not a directory — nothing to clean", options_.version_root);

			return {};
		}

		// 서비스 실행 경로를 지우면 가동 중인 앱이 사라진다. 두 경로가 같으면 거부한다.
		if (!options_.service_root.empty())
		{
			const auto version_root = std::filesystem::weakly_canonical(options_.version_root, error);
			const auto service_root = std::filesystem::weakly_canonical(options_.service_root, error);

			if (version_root == service_root)
			{
				return std::unexpected("version_root must not be the same as service_root");
			}
		}

		std::filesystem::directory_iterator entry(options_.version_root, error);
		if (error)
		{
			// 검사하지 않으면 이터레이터가 곧바로 end() 가 되어 "0개 삭제 성공"으로 보고된다.
			return std::unexpected(std::format("cannot list '{}': {}", options_.version_root, error.message()));
		}

		uint32_t removed = 0;
		const std::filesystem::directory_iterator last;
		while (entry != last)
		{
			std::error_code remove_error;
			std::filesystem::remove_all(entry->path(), remove_error);
			if (remove_error)
			{
				return std::unexpected(std::format("cannot remove '{}': {}", entry->path().string(), remove_error.message()));
			}

			++removed;

			// range-for 의 증가는 error_code 를 받지 않는 오버로드라 실패 시 예외를 던진다.
			entry.increment(error);
			if (error)
			{
				return std::unexpected(std::format("cannot walk '{}': {}", options_.version_root, error.message()));
			}
		}

		last_report_ = std::format("removed {} version directories from '{}'", removed, options_.version_root);
		Logger::handle().write(LogTypes::Information, last_report_);

		return {};
	}

	auto AgentService::current_status(const std::string& message) -> std::expected<void, std::string>
	{
		(void)message;

		std::error_code error;

		// 갓 설치된 기기는 version_root 가 아직 없다. 상태 조회는 그때가 가장 필요하므로,
		// 존재하는 가장 가까운 상위 경로로 올라가 디스크 용량을 잰다.
		std::filesystem::path target = options_.version_root.empty() ? std::filesystem::current_path(error) : std::filesystem::path(options_.version_root);
		while (!target.empty() && !std::filesystem::exists(target, error) && target.has_parent_path() && target.parent_path() != target)
		{
			target = target.parent_path();
		}

		if (target.empty() || !std::filesystem::exists(target, error))
		{
			target = std::filesystem::current_path(error);
		}

		const auto space = std::filesystem::space(target, error);
		if (error)
		{
			return std::unexpected(std::format("cannot query disk space for '{}': {}", target.string(), error.message()));
		}

		boost::json::object report;
		report["device_id"] = options_.device_id;
		report["group"] = options_.group;
		report["version_root"] = options_.version_root;
		report["disk_probe_path"] = target.string();
		report["disk_capacity_bytes"] = static_cast<uint64_t>(space.capacity);
		report["disk_free_bytes"] = static_cast<uint64_t>(space.free);
		report["disk_available_bytes"] = static_cast<uint64_t>(space.available);
		report["hardware_concurrency"] = static_cast<uint64_t>(std::thread::hardware_concurrency());

		boost::json::array versions;
		if (!options_.version_root.empty() && std::filesystem::is_directory(options_.version_root, error))
		{
			std::error_code walk_error;
			std::filesystem::directory_iterator entry(options_.version_root, walk_error);

			const std::filesystem::directory_iterator last;
			while (!walk_error && entry != last)
			{
				if (entry->is_directory(error))
				{
					versions.push_back(boost::json::value(entry->path().filename().string()));
				}

				entry.increment(walk_error);
			}

			if (walk_error)
			{
				// 상태 조회는 실패시키지 않는다. 다만 목록이 불완전함을 보고에 남긴다.
				report["downloaded_versions_error"] = walk_error.message();
			}
		}
		report["downloaded_versions"] = versions;

		last_report_ = boost::json::serialize(report);
		Logger::handle().write(LogTypes::Information, last_report_);

		return {};
	}

	auto AgentService::apply_version(const std::string& message) -> std::expected<void, std::string>
	{
		auto object = parse_object(message);
		if (!object)
		{
			return std::unexpected(object.error());
		}

		auto release_id = read_string(object.value(), "release_id");
		if (!release_id)
		{
			return std::unexpected(release_id.error());
		}

		const auto source = version_directory(release_id.value());

		std::error_code error;
		if (!std::filesystem::is_directory(source, error))
		{
			return std::unexpected(std::format("version '{}' is not downloaded (expected '{}')", release_id.value(), source));
		}

		// 서비스 중단 → 교체 → 재시작은 릴리스 설치기(R-008)가 담당한다. 아직 없으므로
		// 여기서 임의로 파일을 옮기지 않는다. 반쯤 교체된 상태가 가장 위험하다.
		return std::unexpected(
			std::format("apply_version requires the release installer (R-008); version '{}' is downloaded and ready at '{}'", release_id.value(), source));
	}

	auto AgentService::rollback_version(const std::string& message) -> std::expected<void, std::string>
	{
		auto object = parse_object(message);
		if (!object)
		{
			return std::unexpected(object.error());
		}

		auto release_id = read_string(object.value(), "release_id");
		if (!release_id)
		{
			return std::unexpected(release_id.error());
		}

		const auto source = version_directory(release_id.value());

		// 요구대로 폴더에 파일이 있으면 진행하고, 없으면 실패를 되돌린다.
		std::error_code error;
		if (!std::filesystem::is_directory(source, error))
		{
			return std::unexpected(std::format("cannot roll back to '{}': version directory '{}' does not exist", release_id.value(), source));
		}

		if (std::filesystem::is_empty(source, error))
		{
			return std::unexpected(std::format("cannot roll back to '{}': version directory '{}' is empty", release_id.value(), source));
		}

		return std::unexpected(std::format("rollback_version requires the release installer (R-008); version '{}' is present at '{}'", release_id.value(), source));
	}
} // namespace YirangAgent
