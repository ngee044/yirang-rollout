#!/bin/bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

S3_ENDPOINT="${YIRANG_E2E_S3_ENDPOINT:-http://127.0.0.1:8333}"
SQS_ENDPOINT="${YIRANG_E2E_SQS_ENDPOINT:-http://127.0.0.1:9324}"
BUCKET="${YIRANG_E2E_BUCKET:-yirang-e2e}"
INTEGRATION_QUEUE="yirang-integration"
DEVICE_QUEUES=(yirang-pc-001 yirang-pc-002)
RESULT_QUEUE="yirang-results"
DEAD_LETTER_QUEUE="yirang-dead-letters"
READY_TIMEOUT_SECONDS="${YIRANG_E2E_READY_TIMEOUT:-90}"

usage() {
	cat <<'USAGE'
사용법: tests/e2e/stack.sh <command>

  up          스택을 기동하고 준비될 때까지 기다린 뒤 버킷을 만든다
  down        스택을 내리고 데이터를 지운다
  provision   버킷을 만든다 (스택이 이미 떠 있을 때)
  env         테스트가 쓸 환경변수를 export 구문으로 출력한다
  status      컨테이너 상태와 엔드포인트 도달 여부를 보여준다

  eval "$(tests/e2e/stack.sh env)" 로 환경변수를 현재 셸에 적용한다.
USAGE
}

compose() {
	docker compose -f "$SCRIPT_DIR/docker-compose.yml" "$@"
}

require_docker() {
	if ! docker info >/dev/null 2>&1; then
		echo "[stack.sh] docker 데몬에 연결하지 못했습니다. Docker 를 먼저 실행하십시오." >&2
		exit 1
	fi
}

wait_for() {
	local label="$1" url="$2" deadline=$((SECONDS + READY_TIMEOUT_SECONDS)) code

	while [ "$SECONDS" -lt "$deadline" ]; do
		code="$(curl -s -o /dev/null --max-time 3 -w '%{http_code}' "$url" || true)"
		if [ -n "$code" ] && [ "$code" != "000" ]; then
			echo "[stack.sh] $label 준비됨: $url (HTTP $code)"
			return 0
		fi
		sleep 1
	done

	echo "[stack.sh] $label 이 ${READY_TIMEOUT_SECONDS}초 안에 응답하지 않았습니다: $url" >&2
	compose logs --tail 40 >&2
	return 1
}

queue_url() {
	curl -sf --max-time 5 "$SQS_ENDPOINT/?Action=GetQueueUrl&QueueName=$1&Version=2012-11-05" \
		| sed -n 's:.*<QueueUrl>\(.*\)</QueueUrl>.*:\1:p'
}

require_queues() {
	local name url
	for name in "${DEVICE_QUEUES[@]}" "$RESULT_QUEUE" "$DEAD_LETTER_QUEUE" "$INTEGRATION_QUEUE"; do
		url="$(queue_url "$name" || true)"
		if [ -z "$url" ]; then
			echo "[stack.sh] 큐를 찾지 못했습니다: $name (elasticmq.conf 확인)" >&2
			return 1
		fi
	done

	echo "[stack.sh] 큐 준비됨: ${DEVICE_QUEUES[*]} $RESULT_QUEUE $DEAD_LETTER_QUEUE $INTEGRATION_QUEUE"
}

weed_shell() {
	printf '%s\n' "$1" | compose exec -T storage weed shell 2>/dev/null || true
}

provision() {
	local deadline=$((SECONDS + READY_TIMEOUT_SECONDS))

	while [ "$SECONDS" -lt "$deadline" ]; do
		if weed_shell "s3.bucket.list" | grep -qE "[[:space:]]$BUCKET[[:space:]]"; then
			echo "[stack.sh] 버킷 준비됨: $BUCKET"
			return 0
		fi

		weed_shell "s3.bucket.create -name $BUCKET" >/dev/null
		sleep 1
	done

	echo "[stack.sh] 버킷을 만들지 못했습니다: $BUCKET" >&2
	return 1
}

env_exports() {
	local integration
	integration="$(queue_url "$INTEGRATION_QUEUE" || true)"

	if [ -z "$integration" ]; then
		echo "[stack.sh] 큐 URL 을 읽지 못했습니다. tests/e2e/stack.sh up 을 먼저 실행하십시오." >&2
		return 1
	fi

	echo "export YIRANG_TEST_S3_ENDPOINT='$S3_ENDPOINT'"
	echo "export YIRANG_TEST_S3_BUCKET='$BUCKET'"
	echo "export YIRANG_TEST_S3_ACCESS_KEY='test'"
	echo "export YIRANG_TEST_S3_SECRET_KEY='test'"
	echo "export YIRANG_TEST_SQS_ENDPOINT='$SQS_ENDPOINT'"
	echo "export YIRANG_TEST_SQS_QUEUE_URL='$integration'"
}

case "${1:-}" in
	up)
		require_docker
		compose up -d
		wait_for "S3(SeaweedFS)" "$S3_ENDPOINT/"
		wait_for "SQS(ElasticMQ)" "$SQS_ENDPOINT/?Action=ListQueues&Version=2012-11-05"
		require_queues
		provision
		;;
	down)
		require_docker
		compose down -v
		;;
	provision)
		provision
		;;
	env)
		env_exports
		;;
	status)
		require_docker
		compose ps
		curl -sf -o /dev/null --max-time 3 "$S3_ENDPOINT/" && echo "[stack.sh] S3 도달 가능: $S3_ENDPOINT" || echo "[stack.sh] S3 도달 불가: $S3_ENDPOINT"
		curl -sf -o /dev/null --max-time 3 "$SQS_ENDPOINT/?Action=ListQueues&Version=2012-11-05" && echo "[stack.sh] SQS 도달 가능: $SQS_ENDPOINT" || echo "[stack.sh] SQS 도달 불가: $SQS_ENDPOINT"
		;;
	*)
		usage
		exit 1
		;;
esac
