#pragma once

#include <expected>
#include <string>

namespace Artifact
{
	// 객체 키는 릴리스 단위로 격리한다: releases/<release_id>/<install_path>
	// 같은 release_id 를 다시 올리면 같은 키에 덮어써지므로 업로드 재시도가 멱등이 된다.
	//
	// install_path 는 배포 티켓을 통해 들어오는 외부 입력이다. 절대 경로·상위 경로 참조를 통과시키면
	// 저장소와 설치 디렉터리 밖으로 쓰기가 새어나가므로 여기서 막는다.
	auto make_object_key(const std::string& release_id, const std::string& install_path) -> std::expected<std::string, std::string>;
} // namespace Artifact
