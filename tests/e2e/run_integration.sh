#!/bin/bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
LOG_FILE="$BUILD_DIR/e2e-integration.log"
JOBS="${YIRANG_E2E_JOBS:-8}"

if [ ! -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
	echo "[run_integration.sh] 빌드 결과가 없습니다. ./build.sh 를 먼저 실행하십시오." >&2
	exit 1
fi

"$SCRIPT_DIR/stack.sh" up

eval "$("$SCRIPT_DIR/stack.sh" env)"

echo "[run_integration.sh] S3  = $YIRANG_TEST_S3_ENDPOINT (bucket $YIRANG_TEST_S3_BUCKET)"
echo "[run_integration.sh] SQS = $YIRANG_TEST_SQS_QUEUE_URL"

if ! (cd "$BUILD_DIR" && ctest --output-on-failure -j "$JOBS") > "$LOG_FILE" 2>&1; then
	echo "[run_integration.sh] ctest 실패 — 마지막 60줄:" >&2
	tail -60 "$LOG_FILE" >&2
	exit 1
fi

if grep -q "tests did not run" "$LOG_FILE"; then
	echo "[run_integration.sh] 건너뛴 테스트가 남아 있습니다 — 통합 테스트가 환경변수를 받지 못했습니다." >&2
	sed -n '/tests did not run/,$p' "$LOG_FILE" >&2
	exit 1
fi

for name in ArtifactStoreIntegrationTest.RoundTripPreservesContent MessagingIntegrationTest.PublishedMessageReachesConsumer; do
	if ! grep -q "$name \.* *Passed" "$LOG_FILE"; then
		echo "[run_integration.sh] 통합 테스트가 통과 목록에 없습니다: $name" >&2
		exit 1
	fi
done

grep -E "tests passed|Total Test time" "$LOG_FILE"
echo "[run_integration.sh] E2E-09 통과 — 통합 테스트 2건이 실행되었고 건너뛴 테스트가 없습니다."
