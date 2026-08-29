#!/bin/bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
WORK_DIR="$BUILD_DIR/e2e-workspace"

CLI="$BUILD_DIR/out/yirang"
AGENT="$BUILD_DIR/out/yirang-agent"

API_PORT="${YIRANG_E2E_API_PORT:-18099}"
API_URL="http://127.0.0.1:$API_PORT"

CLI_CONFIG="$WORK_DIR/cli.json"
REPORTS="$WORK_DIR/reports.jsonl"

API_PID=""
AGENT_PC001_PID=""
AGENT_PC002_PID=""

PASSED=()
FAILED=()

unset AWS_SESSION_TOKEN AWS_PROFILE AWS_DEFAULT_PROFILE 2>/dev/null || true
export AWS_ACCESS_KEY_ID=test
export AWS_SECRET_ACCESS_KEY=test
export AWS_REGION=us-east-1
export AWS_EC2_METADATA_DISABLED=true

note() { echo "[run_scenarios.sh] $*"; }
abort() { echo "[run_scenarios.sh] $*" >&2; exit 1; }

require_port_free() {
	if lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1; then
		abort "포트 $1 이 이미 사용 중입니다($2). 정리한 뒤 다시 실행하거나 YIRANG_E2E_${3}_PORT 로 다른 포트를 지정하십시오."
	fi
}

preflight() {
	[ -x "$CLI" ] || abort "$CLI 가 없습니다. ./build.sh 를 먼저 실행하십시오."
	[ -x "$AGENT" ] || abort "$AGENT 가 없습니다. ./build.sh 를 먼저 실행하십시오."

	local tool
	for tool in jq go curl lsof docker; do
		command -v "$tool" >/dev/null || abort "$tool 이 필요합니다."
	done

	if pgrep -f "$WORK_DIR" >/dev/null 2>&1; then
		abort "이전 실행의 프로세스가 남아 있습니다. pkill -f '$WORK_DIR' 로 정리한 뒤 다시 실행하십시오."
	fi

	require_port_free "$API_PORT" "RestAPI" API
}

purge_queue() {
	if ! curl -sf -G -o /dev/null --max-time 5 \
		--data-urlencode "QueueUrl=$1" \
		"$SQS_ENDPOINT/?Action=PurgeQueue&Version=2012-11-05"; then
		abort "큐를 비우지 못했습니다: $1 — 이전 실행의 명령·보고가 남으면 판정이 오염됩니다."
	fi
}

stop_process() {
	local pid="$1"
	[ -n "$pid" ] || return 0
	kill -TERM "$pid" 2>/dev/null || true
	for _ in $(seq 1 20); do
		kill -0 "$pid" 2>/dev/null || return 0
		sleep 0.25
	done
	kill -9 "$pid" 2>/dev/null || true
}

teardown() {
	local status=$?
	set +e
	trap - EXIT INT TERM
	stop_process "$AGENT_PC001_PID"
	stop_process "$AGENT_PC002_PID"
	pkill -f "$WORK_DIR/devices/pc-00[12]/service/releases/" 2>/dev/null
	stop_process "$API_PID"
	exit $status
}

write_cli_config() {
	cat > "$CLI_CONFIG" <<CONFIG
{
	"app_title": "yirang-e2e-cli",
	"control_plane_url": "$API_URL",
	"api_token": "",
	"request_timeout_seconds": 30,
	"output_format": "table",
	"upload_file_list": [],
	"target_group": "",
	"s3_bucket": "$BUCKET",
	"s3_region": "us-east-1",
	"s3_endpoint": "$S3_ENDPOINT",
	"allow_insecure_tls": false,
	"log_root_path": "$WORK_DIR/logs/",
	"write_console_log": 0,
	"write_file_log": 4,
	"write_interval": 100
}
CONFIG
}

write_agent_config() {
	local device="$1" group="$2" queue="$3" root="$WORK_DIR/devices/$1"

	mkdir -p "$root/versions" "$root/service" "$root/logs"

	cat > "$root/agent.json" <<CONFIG
{
	"main_title": "yirang-agent-$device",
	"write_file_log": 4,
	"write_console_log": 4,
	"write_interval": 100,
	"log_root_path": "$root/logs/",

	"device_id": "$device",
	"group": "$group",

	"queue_url": "$queue",
	"result_queue_url": "$QUEUE_RESULTS",
	"poll_wait_seconds": 1,

	"version_root": "$root/versions",
	"service_root": "$root/service",
	"keep_previous_releases": 2,

	"s3_bucket": "$BUCKET",
	"s3_region": "us-east-1",
	"s3_endpoint": "$S3_ENDPOINT",
	"allow_insecure_tls": false,

	"queue_region": "us-east-1",
	"queue_endpoint": "$SQS_ENDPOINT",

	"service": {
		"executable": "app.sh",
		"arguments": ["$root/marker"],
		"working_directory": "",
		"stop_timeout_seconds": 5,
		"startup_timeout_seconds": 15
	},

	"health": {
		"kind": "process",
		"host": "127.0.0.1",
		"port": 0,
		"path": "/",
		"expected_status": 200,
		"timeout_ms": 500,
		"interval_ms": 300,
		"success_threshold": 3,
		"failure_threshold": 1
	}
}
CONFIG
}

start_api() {
	(cd "$ROOT_DIR/RestAPI" && go build -o "$WORK_DIR/yirang-api" ./cmd/api) || abort "RestAPI 빌드 실패"

	DEVICE_QUEUES="[{\"name\":\"pc-001\",\"url\":\"$QUEUE_PC001\",\"group\":\"kiosk\"},{\"name\":\"pc-002\",\"url\":\"$QUEUE_PC002\",\"group\":\"backoffice\"}]" \
	RESULT_QUEUE_URL="$QUEUE_RESULTS" \
	AWS_ENDPOINT_URL="$SQS_ENDPOINT" \
	BIND_ADDRESS=127.0.0.1 \
	PORT="$API_PORT" \
	"$WORK_DIR/yirang-api" > "$WORK_DIR/logs/restapi.log" 2>&1 &
	API_PID=$!

	local deadline=$((SECONDS + 20))
	while [ "$SECONDS" -lt "$deadline" ]; do
		if curl -sf -o /dev/null --max-time 2 "$API_URL/readyz"; then
			note "RestAPI 준비됨: $API_URL"
			return 0
		fi
		sleep 0.5
	done

	abort "RestAPI 가 뜨지 않았습니다. $WORK_DIR/logs/restapi.log 를 확인하십시오."
}

start_agents() {
	"$AGENT" --config_path "$WORK_DIR/devices/pc-001/agent.json" > "$WORK_DIR/logs/agent-pc-001.log" 2>&1 &
	AGENT_PC001_PID=$!

	"$AGENT" --config_path "$WORK_DIR/devices/pc-002/agent.json" > "$WORK_DIR/logs/agent-pc-002.log" 2>&1 &
	AGENT_PC002_PID=$!

	local deadline=$((SECONDS + 30))
	while [ "$SECONDS" -lt "$deadline" ]; do
		if grep -q "consuming" "$WORK_DIR/logs/agent-pc-001.log" 2>/dev/null \
			&& grep -q "consuming" "$WORK_DIR/logs/agent-pc-002.log" 2>/dev/null; then
			return 0
		fi
		sleep 0.5
	done

	abort "Agent 가 소비를 시작하지 못했습니다. $WORK_DIR/logs/agent-*.log 를 확인하십시오."
}

collect_results() {
	local output status=0

	output="$("$CLI" results --config_path "$CLI_CONFIG" --output_format json 2>&1)" || status=$?
	if [ "$status" -ne 0 ]; then
		echo "[run_scenarios.sh] 결과 조회 실패(exit $status): $output" >&2
		return 1
	fi

	[ -n "$output" ] || return 0

	if ! printf '%s' "$output" | jq -e '.success == true' >/dev/null 2>&1; then
		echo "[run_scenarios.sh] 결과 조회 응답이 성공 봉투가 아닙니다: $output" >&2
		return 1
	fi

	printf '%s' "$output" | jq -c '.data.reports[]?' >> "$REPORTS" || return 1
}

matched() {
	jq -e -s "map(select($1)) | length > 0" "$REPORTS" >/dev/null 2>&1
}

wait_for_report() {
	local filter="$1" label="$2" timeout="${3:-90}"
	local deadline=$((SECONDS + timeout))

	while [ "$SECONDS" -lt "$deadline" ]; do
		collect_results || return 1
		if matched "$filter"; then
			return 0
		fi
		sleep 1
	done

	echo "[run_scenarios.sh] 보고 대기 시간초과: $label" >&2
	echo "[run_scenarios.sh] 수집된 보고:" >&2
	jq -c . "$REPORTS" >&2 2>/dev/null || true
	return 1
}

report_detail() {
	jq -r -s "map(select($1)) | last | .detail // \"\"" "$REPORTS" 2>/dev/null || printf ''
}

warm_up() {
	note "예열 — current_status 로 두 Agent 의 큐 도달을 확인합니다"

	send_command current_status "" "" || abort "예열 명령 발행 실패"
	wait_for_report ".device_id==\"pc-001\" and .command==\"current_status\"" "pc-001 예열 응답" 60 \
		|| abort "pc-001 이 명령을 받지 못했습니다(SQS 도달 실패는 Agent 로그에 남지 않습니다)."
	wait_for_report ".device_id==\"pc-002\" and .command==\"current_status\"" "pc-002 예열 응답" 60 \
		|| abort "pc-002 가 명령을 받지 못했습니다(SQS 도달 실패는 Agent 로그에 남지 않습니다)."

	: > "$REPORTS"
	note "Agent 2대 소비 확인됨 (pc-001/kiosk · pc-002/backoffice)"
}

deploy_release() {
	local payload="$1" group="$2" output id

	if ! output="$("$CLI" deploy --config_path "$CLI_CONFIG" --upload_file_list "$payload/app.sh" --target_group "$group" 2>&1)"; then
		echo "$output" >&2
		return 1
	fi

	if printf '%s' "$output" | grep -q "FAILED"; then
		echo "$output" >&2
		return 1
	fi

	id="$(printf '%s' "$output" | grep -oE 'rel_[0-9]{8}_[0-9]{6}' | head -1 || true)"
	if [ -z "$id" ]; then
		echo "$output" >&2
		return 1
	fi

	printf '%s' "$id"
}

send_command() {
	local name="$1" release_id="$2" group="$3" output
	local args=(command "$name")

	[ -n "$release_id" ] && args+=("$release_id")
	args+=(--config_path "$CLI_CONFIG")
	[ -n "$group" ] && args+=(--target_group "$group")

	if ! output="$("$CLI" "${args[@]}" 2>&1)"; then
		echo "$output" >&2
		return 1
	fi

	if printf '%s' "$output" | grep -q "FAILED"; then
		echo "$output" >&2
		return 1
	fi
}

state_field() {
	local file="$WORK_DIR/devices/$1/service/state.json"
	[ -f "$file" ] || { printf ''; return 0; }
	jq -r ".$2 // \"\"" "$file" 2>/dev/null || printf ''
}

runtime_pid() {
	local file="$WORK_DIR/devices/$1/service/runtime.json"
	[ -f "$file" ] || { printf ''; return 0; }
	jq -r '.process_id // ""' "$file" 2>/dev/null || printf ''
}

marker_of() {
	local file="$WORK_DIR/devices/$1/marker"
	[ -f "$file" ] || { printf ''; return 0; }
	cat "$file" 2>/dev/null || printf ''
}

alive() {
	[ -n "$1" ] || { printf 'no-pid'; return 0; }
	kill -0 "$1" 2>/dev/null && printf 'alive' || printf 'dead'
}

check() {
	local label="$1" actual="$2" expected="$3"
	if [ "$actual" = "$expected" ]; then
		note "  ok   $label"
		return 0
	fi
	echo "[run_scenarios.sh]   FAIL $label — 기대 '$expected', 실제 '$actual'" >&2
	return 1
}

check_contains() {
	local label="$1" haystack="$2" needle="$3"
	case "$haystack" in
		*"$needle"*)
			note "  ok   $label"
			return 0
			;;
	esac
	echo "[run_scenarios.sh]   FAIL $label — '$needle' 를 찾지 못함: '$haystack'" >&2
	return 1
}

check_differs() {
	local label="$1" left="$2" right="$3"
	if [ -z "$left" ] || [ -z "$right" ]; then
		echo "[run_scenarios.sh]   FAIL $label — 비교할 값이 비었습니다 ('$left' vs '$right')" >&2
		return 1
	fi
	check "$label" "$([ "$left" != "$right" ] && echo different || echo same)" "different"
}

record() {
	if [ "$2" -eq 0 ]; then
		PASSED+=("$1")
		note "$1 통과"
	else
		FAILED+=("$1")
		echo "[run_scenarios.sh] $1 실패" >&2
	fi
}

V1_ID=""
V2_ID=""
VBAD_ID=""
V1_PID=""
V2_PID=""

scenario_01() {
	note "TC-E2E-01 배포 해피 패스 — deploy → 발행 → 다운로드 → apply → 신규 버전 동작"

	V1_ID="$(deploy_release "$SCRIPT_DIR/releases/v1" kiosk)" || return 1
	note "  release_id = $V1_ID"

	wait_for_report ".device_id==\"pc-001\" and .command==\"download_version\" and .success==true and (.detail|contains(\"$V1_ID\"))" "pc-001 download_version($V1_ID)" || return 1
	send_command apply_version "$V1_ID" kiosk || return 1
	wait_for_report ".device_id==\"pc-001\" and .command==\"apply_version\" and .success==true and (.detail|contains(\"$V1_ID\"))" "pc-001 apply_version($V1_ID)" || return 1

	V1_PID="$(runtime_pid pc-001)"

	check "state.json active 가 배포한 릴리스" "$(state_field pc-001 active)" "$V1_ID" || return 1
	check "runtime.json 의 pid 가 살아 있음" "$(alive "$V1_PID")" "alive" || return 1
	check "기동한 프로세스가 v1" "$(marker_of pc-001 | cut -d' ' -f1)" "v1" || return 1
	check "marker 의 pid 가 runtime.json 과 일치" "$(marker_of pc-001 | cut -d' ' -f2)" "$V1_PID" || return 1
	check "그룹이 다른 pc-002 에는 apply 가 가지 않음" "$(matched ".device_id==\"pc-002\" and .command==\"apply_version\"" && echo delivered || echo absent)" "absent" || return 1
	check "pc-002 에는 활성 릴리스가 없음" "$(state_field pc-002 active)" "" || return 1
}

scenario_09() {
	note "TC-E2E-09 버전 교체 — v1 → v2, pid 변경과 state.json 전환"

	sleep 2
	V2_ID="$(deploy_release "$SCRIPT_DIR/releases/v2" kiosk)" || return 1
	note "  release_id = $V2_ID"

	check_differs "release_id 가 v1 과 다름" "$V2_ID" "$V1_ID" || return 1

	wait_for_report ".device_id==\"pc-001\" and .command==\"download_version\" and .success==true and (.detail|contains(\"$V2_ID\"))" "pc-001 download_version($V2_ID)" || return 1
	send_command apply_version "$V2_ID" kiosk || return 1
	wait_for_report ".device_id==\"pc-001\" and .command==\"apply_version\" and .success==true and (.detail|contains(\"$V2_ID\"))" "pc-001 apply_version($V2_ID)" || return 1

	V2_PID="$(runtime_pid pc-001)"

	check "state.json active = v2" "$(state_field pc-001 active)" "$V2_ID" || return 1
	check "state.json previous = v1" "$(state_field pc-001 previous)" "$V1_ID" || return 1
	check_differs "pid 가 교체됨" "$V2_PID" "$V1_PID" || return 1
	check "이전 프로세스는 종료됨" "$(alive "$V1_PID")" "dead" || return 1
	check "새 프로세스가 살아 있음" "$(alive "$V2_PID")" "alive" || return 1
	check "기동한 프로세스가 v2" "$(marker_of pc-001 | cut -d' ' -f1)" "v2" || return 1
	check "marker 의 pid 가 새 pid" "$(marker_of pc-001 | cut -d' ' -f2)" "$V2_PID" || return 1
}

scenario_02() {
	note "TC-E2E-02 자동 롤백 — 즉시 종료하는 릴리스 적용 시 이전 버전으로 복귀"

	sleep 2
	VBAD_ID="$(deploy_release "$SCRIPT_DIR/releases/vbad" kiosk)" || return 1
	note "  release_id = $VBAD_ID"

	check_differs "release_id 가 v2 와 다름" "$VBAD_ID" "$V2_ID" || return 1

	wait_for_report ".device_id==\"pc-001\" and .command==\"download_version\" and .success==true and (.detail|contains(\"$VBAD_ID\"))" "pc-001 download_version($VBAD_ID)" || return 1
	send_command apply_version "$VBAD_ID" kiosk || return 1
	wait_for_report ".device_id==\"pc-001\" and .command==\"apply_version\" and .success==false and (.detail|contains(\"$VBAD_ID\"))" "pc-001 apply_version($VBAD_ID) 실패 보고" || return 1

	local detail rolled_pid
	detail="$(report_detail ".device_id==\"pc-001\" and .command==\"apply_version\" and .success==false and (.detail|contains(\"$VBAD_ID\"))")"
	rolled_pid="$(runtime_pid pc-001)"

	check_contains "실패 사유가 readiness 미확정" "$detail" "did not become ready" || return 1
	check_contains "롤백 대상이 보고에 남음" "$detail" "rolled back to '$V2_ID'" || return 1
	check "state.json active 가 v2 로 복귀" "$(state_field pc-001 active)" "$V2_ID" || return 1
	check "state.json previous 가 실패한 릴리스" "$(state_field pc-001 previous)" "$VBAD_ID" || return 1
	check "롤백 후 pid 가 살아 있음" "$(alive "$rolled_pid")" "alive" || return 1
	check_differs "롤백으로 프로세스가 재기동됨" "$rolled_pid" "$V2_PID" || return 1
	check "재기동한 프로세스가 v2" "$(marker_of pc-001 | cut -d' ' -f1)" "v2" || return 1
	check "marker 의 pid 가 재기동한 pid" "$(marker_of pc-001 | cut -d' ' -f2)" "$rolled_pid" || return 1
}

scenario_10() {
	note "TC-E2E-10 수동 롤백 — rollback_version 으로 v1 복귀"

	local before after
	before="$(runtime_pid pc-001)"

	send_command rollback_version "$V1_ID" kiosk || return 1
	wait_for_report ".device_id==\"pc-001\" and .command==\"rollback_version\" and .success==true and (.detail|contains(\"$V1_ID\"))" "pc-001 rollback_version($V1_ID)" || return 1

	after="$(runtime_pid pc-001)"

	check "state.json active = v1" "$(state_field pc-001 active)" "$V1_ID" || return 1
	check_differs "pid 가 교체됨" "$after" "$before" || return 1
	check "이전 프로세스는 종료됨" "$(alive "$before")" "dead" || return 1
	check "롤백된 프로세스가 살아 있음" "$(alive "$after")" "alive" || return 1
	check "기동한 프로세스가 v1" "$(marker_of pc-001 | cut -d' ' -f1)" "v1" || return 1
	check "marker 의 pid 가 새 pid" "$(marker_of pc-001 | cut -d' ' -f2)" "$after" || return 1
}

main() {
	preflight

	rm -rf "$WORK_DIR"
	mkdir -p "$WORK_DIR/logs"
	: > "$REPORTS"

	"$SCRIPT_DIR/stack.sh" up

	eval "$("$SCRIPT_DIR/stack.sh" env)"

	S3_ENDPOINT="$YIRANG_E2E_S3_ENDPOINT"
	SQS_ENDPOINT="$YIRANG_E2E_SQS_ENDPOINT"
	BUCKET="$YIRANG_E2E_BUCKET"
	QUEUE_PC001="$YIRANG_E2E_QUEUE_PC001"
	QUEUE_PC002="$YIRANG_E2E_QUEUE_PC002"
	QUEUE_RESULTS="$YIRANG_E2E_QUEUE_RESULTS"

	note "명령·결과 큐 비우는 중"
	purge_queue "$QUEUE_PC001"
	purge_queue "$QUEUE_PC002"
	purge_queue "$QUEUE_RESULTS"

	write_cli_config
	write_agent_config pc-001 kiosk "$QUEUE_PC001"
	write_agent_config pc-002 backoffice "$QUEUE_PC002"

	trap teardown EXIT INT TERM

	start_api
	start_agents
	warm_up

	local status=0
	scenario_01 || status=1
	record TC-E2E-01 "$status"

	if [ "$status" -eq 0 ]; then
		status=0; scenario_09 || status=1; record TC-E2E-09 "$status"
		status=0; scenario_02 || status=1; record TC-E2E-02 "$status"
		status=0; scenario_10 || status=1; record TC-E2E-10 "$status"
	else
		note "TC-E2E-01 이 실패해 후속 시나리오를 건너뜁니다(선행 릴리스가 없습니다)."
	fi

	echo
	note "통과 ${#PASSED[@]}건: ${PASSED[*]:-없음}"
	if [ "${#FAILED[@]}" -gt 0 ]; then
		note "실패 ${#FAILED[@]}건: ${FAILED[*]}"
		note "로그: $WORK_DIR/logs/"
		return 1
	fi

	note "Phase B 시나리오 4건 전부 통과"
}

main "$@"
