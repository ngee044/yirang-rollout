# RestAPI — yirang-rollout 배포 REST API 서버

`DeployCLI`(C++)와 디바이스 큐 사이에 놓이는 발행 주체입니다. CLI는 큐 URL을 알지 못하며, 대상 디바이스 목록은 이 서버가 소유합니다.

```
yirang (CLI) ──REST──▶ RestAPI (Go) ──publish──▶ SQS ──▶ yirang-agent (C++, 상주)
                          ▲                                      │
                          └──────────── result queue ◀───────────┘
```

## 보안 모델 — 왜 인증이 없는가

**이 서버는 개발자 본인의 머신에서 도는 로컬 인터페이스다.** 팀이 공유하는 서비스가 아니다.

배포하는 개발자는 어차피 자기 머신에 AWS 자격증명을 두고 있어야 한다(S3 업로드·SQS 발행 모두 그 자격증명으로 나간다). 이 서버는 그 자격증명을 **감싸는 껍데기**이지 보호 장벽이 아니다. `127.0.0.1:8080`에 닿을 수 있는 사람은 이미 `~/.aws/credentials`도 읽을 수 있으므로, 그 앞에 토큰을 세워도 실질적으로 막는 것이 없다. 로컬 개발 서버·`docker.sock`·`aws configure`가 같은 전제를 쓴다.

이 전제가 **실제로 성립하도록** 두 가지를 강제한다.

| 강제 | 내용 |
|------|------|
| **기본 루프백 바인딩** | `BIND_ADDRESS` 기본값이 `127.0.0.1`이다. 노출은 명시적 선택이어야 하며, 루프백이 아니면 기동 시 경고를 남긴다 |
| **`Content-Type: application/json` 필수** | 무인증 로컬 API는 개발자가 방문한 악성 웹페이지의 cross-origin 폼 POST에 노출된다. JSON을 요구하면 브라우저가 preflight를 강제하고, 폼 POST(`text/plain`·`urlencoded`·`multipart`)는 preflight를 받지 못해 브라우저 단계에서 막힌다. 위반은 415 |

### 이 전제가 깨지는 경우 — 그때는 인증이 필요하다

- **`BIND_ADDRESS=0.0.0.0`으로 띄울 때.** 사무실·카페 WiFi의 누구나 `POST /api/v1/commands`로 `clean_old_version`을 플릿 전체에 쏠 수 있다. 이 명령은 각 키오스크의 `version_root` 하위를 전부 삭제한다.
- **Docker에서 `-p 8080:8080`으로 게시할 때.** 이 형식은 컨테이너 포트를 **호스트의 모든 인터페이스**에 연다. 반드시 `-p 127.0.0.1:8080:8080`을 쓴다.
- **팀이 공유하는 서버에 올릴 때.** 그 순간 "자격증명을 이미 가진 사람만 닿는다"가 거짓이 되므로 R-028(인증)이 선행되어야 한다.

무인증은 **로컬 단일 사용자 도구라는 전제 위에서만** 유효한 선택이다. 전제가 바뀌면 결론도 바뀐다.

## 요구 사항

- Go 1.23+ (`net/http.ServeMux`의 메서드 패턴 라우팅을 씁니다)
- SQS (AWS 또는 LocalStack 등 호환 엔드포인트)

외부 의존은 `aws-sdk-go-v2` 하나입니다. HTTP 계층은 표준 라이브러리만 씁니다.

## 실행

```bash
cd RestAPI

export DEVICE_QUEUES='[
  {"name":"kiosk-001","url":"https://sqs.ap-northeast-2.amazonaws.com/123456789012/yirang-kiosk-001","group":"kiosk"},
  {"name":"kiosk-002","url":"https://sqs.ap-northeast-2.amazonaws.com/123456789012/yirang-kiosk-002","group":"kiosk"}
]'
export AWS_REGION=ap-northeast-2
export RESULT_QUEUE_URL=https://sqs.ap-northeast-2.amazonaws.com/123456789012/yirang-results

go run ./cmd/api
```

LocalStack을 쓸 때는 `AWS_ENDPOINT_URL=http://localhost:4566`을 추가합니다.

## 설정 (환경변수)

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `DEVICE_QUEUES` | **필수** | `[{"name","url","group"}]` JSON 배열. 이름·URL 중복은 기동 거부 |
| `BIND_ADDRESS` | `127.0.0.1` | 수신 인터페이스. **기본은 로컬 전용**이며 `0.0.0.0`은 명시적 노출 선택이다(위 보안 모델 참조) |
| `PORT` | `8080` | HTTP 수신 포트 |
| `AWS_REGION` | `us-east-1` | SQS 리전 |
| `AWS_ENDPOINT_URL` | (없음) | LocalStack 등 대체 엔드포인트 |
| `RESULT_QUEUE_URL` | (없음) | 결과 큐. 없으면 `GET /api/v1/results`가 `NO_RESULT_QUEUE` |
| `RESULT_BATCH_SIZE` | `10` | 결과 조회 1회당 수신 개수 (SQS 상한 10) |
| `RESULT_WAIT_SECONDS` | `1` | 결과 큐 long-poll 대기. 이 시간만큼 클라이언트가 대기하므로 짧게 둡니다 |
| `REQUEST_TIMEOUT_SECONDS` | `20` | 핸들러 처리 예산. 초과 시 `TIMEOUT`(504) |
| `READ_TIMEOUT_SECONDS` | `15` | |
| `WRITE_TIMEOUT_SECONDS` | `30` | |
| `SHUTDOWN_TIMEOUT_SECONDS` | `10` | graceful shutdown 대기 |
| `LOG_LEVEL` | `info` | `debug`/`info`/`warn`/`error` |

숫자 변수에 숫자가 아닌 값이 오면 **기본값으로 넘어가지 않고 기동에 실패**합니다. 오타가 운영 중에 드러나지 않게 하기 위해서입니다.

AWS 자격증명은 SDK 기본 체인(환경변수·프로파일·인스턴스 역할)을 따릅니다.

## 엔드포인트

| 메서드 | 경로 | 설명 |
|--------|------|------|
| `GET` | `/healthz` | liveness |
| `GET` | `/readyz` | readiness — 등록 큐 개수·결과 큐 설정 여부 (SQS를 호출하지 않음) |
| `POST` | `/api/v1/deployments` | 릴리스 배포 요청 → `download_version` 발행 |
| `POST` | `/api/v1/commands` | 임의 명령 발행 (5종) |
| `GET` | `/api/v1/commands` | Agent가 처리하는 명령 목록 |
| `GET` | `/api/v1/results` | 결과 큐를 drain해 디바이스 보고 반환 |

응답은 404·405를 포함해 **모두** `{"success": bool, "data"|"error": ...}` 봉투입니다.

### `POST /api/v1/deployments`

```json
{
  "release_id": "rel_20260809_120000",
  "group": "kiosk",
  "artifacts": [
    { "install_path": "bin/app", "sha256": "<64 hex>", "size_bytes": 10485760 }
  ]
}
```

`group`이 비어 있으면 등록된 전체 큐로 브로드캐스트합니다. 응답은 `202 Accepted`이며(큐가 받았을 뿐 디바이스는 아직 적용 전), 큐별 발행 결과가 설정 순서 그대로 `deliveries`에 담깁니다.

```json
{
  "success": true,
  "data": {
    "command": "download_version",
    "targeted": 2,
    "published": 1,
    "deliveries": [
      { "queue": "kiosk-001", "queue_url": "...", "message_id": "..." },
      { "queue": "kiosk-002", "queue_url": "...", "error": "..." }
    ]
  }
}
```

**부분 실패는 치명적으로 다루지 않습니다.** 한 큐가 실패해도 나머지는 발행되고, 실패한 큐가 응답에 그대로 남습니다.

요청은 Agent가 거부할 것을 미리 거부합니다 — `install_path`의 `..`·절대 경로·역슬래시, `release_id`의 경로 구분자, 64자 hex가 아닌 `sha256`. 규칙은 `Artifact/ArtifactKey.cpp`의 `make_object_key`를 그대로 옮긴 것입니다. 대문자 해시는 소문자로 정규화해 발행합니다(Agent가 소문자 다이제스트와 비교합니다).

### `POST /api/v1/commands`

```json
{ "command": "current_status", "group": "kiosk", "payload": {} }
```

지원 명령: `download_version` · `apply_version` · `current_status` · `clean_old_version` · `rollback_version`.

`payload`는 손대지 않고 그대로 전달합니다(재직렬화하면 큰 정수가 부동소수점으로 뭉개집니다). `apply_version`·`rollback_version`은 Agent가 `payload.release_id`를 읽으므로 없으면 400입니다.

### 요청 본문 규칙

- `Content-Type: application/json` 필수 (`UNSUPPORTED_MEDIA_TYPE`)
- 최대 1 MiB (`REQUEST_TOO_LARGE`)
- **모르는 필드는 거부**합니다. `install_path`를 `installpath`로 잘못 쓰면 조용히 무시되는 대신 400이 됩니다
- 본문에 JSON 문서가 둘 이상이면 거부합니다

### 오류 코드

| 코드 | 상태 | 의미 |
|------|------|------|
| `BAD_REQUEST` | 400 | 본문 파싱 실패, 모르는 필드, 필수 필드 누락, 경로/해시 규칙 위반 |
| `UNSUPPORTED_COMMAND` | 400 | Agent에 해당 핸들러가 없음 |
| `UNSUPPORTED_MEDIA_TYPE` | 415 | `Content-Type`이 `application/json`이 아님 |
| `REQUEST_TOO_LARGE` | 413 | 본문이 1 MiB 초과 |
| `NO_TARGETS` | 404 | 지정 `group`에 해당하는 큐가 없음 |
| `NO_RESULT_QUEUE` | 503 | `RESULT_QUEUE_URL` 미설정 |
| `TIMEOUT` | 504 | 처리 예산 초과 |
| `INTERNAL` | 500 | 그 외. **원인은 로그에만 남고 응답에는 나가지 않습니다** |
| `NOT_FOUND` / `METHOD_NOT_ALLOWED` | 404 / 405 | 라우팅 실패 (405는 `Allow` 헤더 포함) |

## 검증

```bash
gofmt -l .
go vet ./...
go build ./...
go test -race ./...     # 84건
```

## 구조

```
RestAPI/
├── cmd/api/              # 진입점 — 설정 로드, 의존성 조립, graceful shutdown
└── internal/
    ├── config/           # 환경변수 로딩·검증, group → 큐 조회
    ├── apierr/           # 오류 클래스 (코드 + HTTP 상태 + 로그 전용 cause)
    ├── models/           # Agent와 공유하는 메시지 봉투·명령 상수
    ├── service/          # 요청 검증, 병렬 fanout 발행, 결과 수집     ← 규칙은 전부 여기
    ├── rest/             # 라우팅·미들웨어·JSON 인코딩              ← 규칙 없음
    └── queue/            # SQS Send/Receive/Delete                ← 정책 없음
```

계층은 `rest → service → queue` 단방향입니다. 인터페이스는 **쓰는 쪽**이 선언합니다 — `service`가 필요한 `Publisher`/`Receiver`를, `rest`가 필요한 서비스 모양을 각자 정의하므로, 하위 계층을 바꿔도 상위가 따라 바뀌지 않습니다.

`internal/models/message.go`의 봉투와 명령 상수는 C++ `YirangAgent/AgentMessage.h`와 짝을 이룹니다. 한쪽을 바꾸면 반드시 다른 쪽도 함께 바꿔야 합니다.

## 설계 메모

- **`Receive`는 삭제하지 않습니다.** 수신 직후 건별로 삭제하면 중간에 삭제가 실패했을 때 이미 읽은 메시지까지 잃습니다. 수신 → 디코딩 → 배치 삭제 순서이고, 삭제 실패는 로그만 남깁니다(at-least-once — 잃는 것보다 중복이 낫습니다).
- **fanout은 최대 8개 동시**입니다. 수백 대 플릿에 순차 발행하면 요청 하나가 왕복 지연 × N이 되고, 무제한 병렬은 커넥션을 그만큼 엽니다.
- **`readyz`는 SQS를 호출하지 않습니다.** 프로브는 수 초마다 도는데 큐 장애는 발행 응답의 `deliveries`에서 큐 단위로 이미 드러납니다.

## 미구현

- **인증 (R-028)** — 무인증은 로컬 전용 사용을 전제로 한 **의도된 선택**입니다(위 보안 모델). 공유 서버에 올리거나 루프백 밖으로 노출하려면 먼저 구현해야 합니다.
- **결과 영속화 (R-027)** — `GET /api/v1/results`는 큐를 drain해 그 시점의 메시지만 돌려주며, 디바이스별 최근 상태를 축적하지 않습니다.
- **멱등 키 (R-026)** — 요청 단위 중복 제거 키가 봉투에 없습니다. Agent 측 계약(R-005)과 함께 확정해야 합니다.
