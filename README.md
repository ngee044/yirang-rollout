# yirang-rollout

> **S3와 메시지 큐만으로 도는 경량 배포 도구.**

`yirang-rollout`은 개발자가 CLI를 한 번 실행하면 로컬 파일 묶음을 S3에 올리고, 지정한 SQS 큐들에 배포 티켓을 발행해, 각 PC의 Agent가 **프로세스를 중단하고 새 버전으로 교체한 뒤 재시작**하게 하는 도구입니다.

**자체 운영 서버가 없습니다.** 중앙 Control Plane도, gRPC 제어 채널도, 데이터베이스도 두지 않습니다. 필요한 외부 인프라는 S3(또는 MinIO)와 SQS 뿐입니다.

배포가 실패하면 **활성 포인터를 되돌려 이전 버전으로 복구**합니다.

## 동작

```text
개발자 PC                      클라우드                 타겟 PC (N대)
┌───────────────┐          ┌──────────┐         ┌──────────────────────┐
│ yirang CLI    │──업로드──▶│    S3    │◀──GET───│ yirang-agent (상주)   │
│               │          └──────────┘ presigned│  ├ SQS consume        │
│ upload_file_  │          ┌──────────┐         │  ├ group 매칭          │
│  list[]       │──발행───▶│ SQS #1   │─consume─▶│  ├ SHA-256 검증        │
│ publish_queues│──발행───▶│ SQS #2   │         │  ├ 프로세스 중단        │
│  [] 로 fanout │──발행───▶│ SQS #N   │         │  ├ current 포인터 교체  │
└───────────────┘          └──────────┘         │  └ 재시작·헬스체크·롤백  │
                                                 └──────────────────────┘
```

CLI가 fanout 주체입니다. 설정에 등록된 큐 목록으로 같은 티켓을 순회 발행하므로 SNS 같은 별도 fanout 인프라가 필요 없고, 어디로 나가는지가 설정 파일에 그대로 보입니다.

## 핵심 원칙

- **인바운드 연결 불필요** — Agent가 SQS를 폴링하고 S3에 아웃바운드로 접근합니다. NAT·방화벽 뒤에서 동작합니다.
- **타겟 PC에 비밀값을 두지 않음** — CLI가 발급한 pre-signed URL로 내려받습니다. PC에 S3 자격증명이 없습니다.
- **검증 전에 실행하지 않음** — SHA-256이 매니페스트와 다르면 설치하지 않고 중단합니다.
- **제자리 덮어쓰기 금지** — 버전 디렉터리에 풀고 `current` 포인터만 교체합니다. 롤백이 포인터 되돌리기로 끝나고, Windows 파일 잠금 문제도 피합니다.
- **멱등** — 같은 `release_id` 티켓을 다시 받아도 결과가 같습니다.

## 설치 레이아웃

```text
<install_root>/
├── releases/
│   ├── rel_20260808_1/     이전 릴리스 (keep_previous_releases 만큼 보관)
│   └── rel_20260808_2/     신규 릴리스
└── current ───────────────▶ 활성 릴리스를 가리키는 포인터
```

## CLI 설정

```json
{
  "upload_file_list": [
    "C:/build/Release/app.exe",
    "C:/build/Release/config.json"
  ],
  "publish_queues": [
    { "name": "kiosk-seoul", "url": "https://sqs.ap-northeast-2.amazonaws.com/…/pc-001" },
    { "name": "kiosk-busan", "url": "https://sqs.ap-northeast-2.amazonaws.com/…/pc-002" }
  ],
  "target_group": "kiosk",
  "s3_bucket": "yirang-releases",
  "presigned_url_expiration_seconds": 3600,
  "install_root": "C:/Program Files/KioskApp"
}
```

## 개요

| 항목 | 내용 |
|------|------|
| 언어 표준 | C++23 (CLI · Agent 전부) |
| 빌드 시스템 | CMake 3.21+ (Ninja 권장) |
| 패키지 관리 | vcpkg (`~/vcpkg`) |
| 지원 플랫폼 | Windows x64 / Linux x64 (macOS는 개발용) |
| CppToolkit | submodule 포함 (`.cpptoolkit`) — Utilities · ThreadPool · AWSService |
| 외부 인프라 | S3(또는 MinIO) · SQS |
| 테스트 | Google Test + ctest |

## 구조

```text
yirang-rollout/
├── CMakeLists.txt          루트 빌드 정의 (플랫폼 분기, 모듈 등록)
├── build.sh                macOS / Linux 빌드
├── vcpkg.json              의존성 매니페스트 (builtin-baseline 고정)
│
├── Configuration/          설정 로딩 · 검증          → 타겟 Configuration
├── Process/                프로세스 기동 · 중지 · 상태 → 타겟 Process
├── Health/                 프로세스 · TCP · HTTP 헬스체크 → 타겟 Health
├── Release/                릴리스 매니페스트 · SHA-256  → 타겟 Release
├── DeployCLI/              yirang CLI                → 타겟 DeployCLI (실행 파일 yirang)
├── Main/                   Agent 진입점               → 타겟 yirang-rollout
│
├── tests/                  gtest (32건)
├── scripts/                부가 스크립트 (build.bat)
├── custom-triplets/        macOS SDK 전달용 vcpkg triplet
├── .cpptoolkit/            CppToolkit 서브모듈
├── docs/                   설계·검증 문서 (로컬 전용, .gitignore 대상)
└── .github/                PR 템플릿
```

모듈은 **최상위 디렉터리 1개 = CMake 타겟 1개**로 배치합니다. 소비 측은 경로 접두어 없이 `#include "ReleaseManifest.h"` 처럼 포함합니다(flat include).

## 빌드

### macOS / Ubuntu

```bash
git submodule update --init --recursive

./build.sh                       # Release
./build.sh --debug               # Debug
./build.sh --clean               # build/ 삭제 후 재구성
./build.sh --target yirang -j 8
```

환경 변수: `VCPKG_ROOT`(기본 `~/vcpkg`), `BUILD_TYPE`, `CXX`(컴파일러 — macOS SDK 프로브에도 사용), `SDKROOT`(macOS SDK 강제 지정)

### Windows

```bat
scripts\build.bat Release
scripts\build.bat Debug --clean
```

환경 변수: `VCPKG_ROOT`(기본 `%USERPROFILE%\vcpkg`)

`build.bat`은 생성기를 지정하지 않으므로 Visual Studio multi-config 생성기가 선택됩니다. 이 경우 산출물 경로와 테스트 명령이 config 하위로 갈라집니다.

> Windows 빌드는 아직 통과하지 않습니다. `Process/` 모듈의 Windows 구현(`CreateProcessW`·Job Objects)이 없어 `CMakeLists.txt`가 `WIN32`에서 명시적으로 실패합니다.

### 산출물

| 경로 | 내용 |
|------|------|
| `build/out/yirang` | CLI |
| `build/out/yirang-rollout` | Agent |
| `build/lib/` | 모듈 정적 라이브러리 |

Windows multi-config 생성기에서는 `build\out\<Config>\`, `build\lib\<Config>\` 하위로 갈라집니다.

## 테스트

```bash
cd build && ctest --output-on-failure
```

Windows(multi-config 생성기)에서는 config를 명시합니다.

```bat
cd build && ctest -C Release --output-on-failure
```

## 진행 상황

| 모듈 | 상태 |
|------|------|
| 설정 로딩 (`Configuration/`) | ✅ 완료 |
| 프로세스 제어 (`Process/`) | ✅ 완료 (POSIX — Windows 미구현) |
| 헬스체크 (`Health/`) | ✅ 완료 |
| 릴리스 매니페스트 (`Release/`) | ✅ 완료 |
| CLI 골격 (`DeployCLI/`) | 🚧 진행중 (서브커맨드 미구현) |
| S3 · SQS · 설치기 · 배포 실행기 · Agent 데몬 | 미착수 |

세부 계획은 `docs/task_list.md`(로컬 전용)를 참조합니다.

## 코드 규약

- 포맷: `.clang-format` (GNU 기반, 탭 들여쓰기, 170 컬럼) — 커밋 전 `clang-format -i <파일>`
- 오류는 예외가 아니라 `std::expected<T, std::string>` 반환값으로 전파합니다.
- 명명·수명 관리·모듈 배치 규약: `docs/CODING_CONVENTION.md` (로컬 전용)

## 문서

`docs/` 하위에 설계·검증 문서를 둡니다(로컬 전용, `.gitignore` 대상). 범위의 단일 출처는 `docs/project_concept.md`입니다.

> 루트의 `yirang-rollout-architecture.md`는 **초기 설계 문서**입니다. Control Plane·gRPC·Site Relay를 갖춘 플릿 관리 플랫폼을 기술하고 있으며, 2026-08-08 재기획으로 대부분이 현재 범위를 벗어났습니다. 플랫폼 추상화(§17)·릴리스 디렉터리(§18)·헬스체크(§22) 등 일부만 유효합니다.
