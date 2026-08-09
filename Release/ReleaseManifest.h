#pragma once

#include "ReleaseTypes.h"

#include <chrono>
#include <expected>
#include <string>
#include <vector>

namespace Release
{
	// upload_file_list 를 릴리스 매니페스트로 바꾸고, 큐로 오갈 JSON 과 상호 변환한다.
	// 시각을 인자로 받는 이유는 생성 결과를 결정론적으로 검증하기 위해서다.
	class ReleaseManifest
	{
	public:
		static auto build(const std::vector<std::string>& upload_file_list, const std::string& release_id, const std::chrono::system_clock::time_point& created_at)
			-> std::expected<Manifest, std::string>;

		static auto serialize(const Manifest& manifest) -> std::string;
		static auto parse(const std::string& text) -> std::expected<Manifest, std::string>;

		// rel_YYYYMMDD_HHMMSS (UTC)
		static auto make_release_id(const std::chrono::system_clock::time_point& when) -> std::string;

		static auto file_sha256(const std::string& path) -> std::expected<std::string, std::string>;
	};
} // namespace Release
