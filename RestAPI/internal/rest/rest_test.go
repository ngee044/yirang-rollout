package rest

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/apierr"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/config"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/models"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/service"
)

type stubService struct {
	result *service.PublishResult
	batch  *service.ResultBatch
	err    error

	deployed *service.DeployRequest
	panics   bool
}

func (s *stubService) Deploy(_ context.Context, request service.DeployRequest) (*service.PublishResult, error) {
	if s.panics {
		panic("boom")
	}
	s.deployed = &request
	return s.result, s.err
}

func (s *stubService) Command(context.Context, service.CommandRequest) (*service.PublishResult, error) {
	return s.result, s.err
}

func (s *stubService) Results(context.Context) (*service.ResultBatch, error) {
	return s.batch, s.err
}

func newTestHandler(svc *stubService) http.Handler {
	cfg := &config.Config{
		DeviceQueues:   []config.DeviceQueue{{Name: "pc-001", URL: "https://sqs.test/pc-001", Group: "kiosk"}},
		ResultQueueURL: "https://sqs.test/results",
		BindAddress:    "127.0.0.1",
		RequestTimeout: 5 * 1e9,
	}
	return NewHandler(cfg, svc, slog.New(slog.NewTextHandler(io.Discard, nil)))
}

func call(t *testing.T, handler http.Handler, method, path, body string) *httptest.ResponseRecorder {
	t.Helper()

	request := httptest.NewRequest(method, path, strings.NewReader(body))
	if method == http.MethodPost {
		request.Header.Set("Content-Type", "application/json")
	}

	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)
	return recorder
}

func decodeEnvelope(t *testing.T, recorder *httptest.ResponseRecorder) envelope {
	t.Helper()

	var decoded envelope
	if err := json.Unmarshal(recorder.Body.Bytes(), &decoded); err != nil {
		t.Fatalf("response is not an envelope: %v (%s)", err, recorder.Body.String())
	}
	return decoded
}

func TestDeployAnswers202(t *testing.T) {
	svc := &stubService{result: &service.PublishResult{Command: models.CommandDownloadVersion, Targeted: 2, Published: 2}}

	recorder := call(t, newTestHandler(svc), http.MethodPost, "/api/v1/deployments",
		`{"release_id":"rel_1","group":"kiosk","artifacts":[{"install_path":"bin/app","sha256":"aa"}]}`)

	if recorder.Code != http.StatusAccepted {
		t.Fatalf("status = %d, want 202 (%s)", recorder.Code, recorder.Body.String())
	}
	if got := recorder.Header().Get("Content-Type"); !strings.HasPrefix(got, "application/json") {
		t.Errorf("Content-Type = %q", got)
	}
	if svc.deployed == nil || svc.deployed.ReleaseID != "rel_1" {
		t.Error("the request did not reach the service intact")
	}
}

func TestDeployRejectsUnknownField(t *testing.T) {
	recorder := call(t, newTestHandler(&stubService{}), http.MethodPost, "/api/v1/deployments",
		`{"release_id":"rel_1","artifact":[]}`)

	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", recorder.Code)
	}
	if decodeEnvelope(t, recorder).Error.Code != apierr.CodeBadRequest {
		t.Errorf("unexpected error body: %s", recorder.Body.String())
	}
}

func TestDeployRejectsMalformedAndEmptyBodies(t *testing.T) {
	tests := map[string]string{
		"not json":      `{ not json`,
		"empty body":    ``,
		"two documents": `{"release_id":"a"}{"release_id":"b"}`,
		"wrong type":    `{"release_id":123}`,
		"array not obj": `[]`,
	}

	for name, body := range tests {
		t.Run(name, func(t *testing.T) {
			recorder := call(t, newTestHandler(&stubService{}), http.MethodPost, "/api/v1/deployments", body)

			if recorder.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400 (%s)", recorder.Code, recorder.Body.String())
			}
		})
	}
}

func TestDeployRejectsOversizedBody(t *testing.T) {
	body := `{"release_id":"` + strings.Repeat("a", maxRequestBytes+1) + `"}`

	recorder := call(t, newTestHandler(&stubService{}), http.MethodPost, "/api/v1/deployments", body)

	if recorder.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("status = %d, want 413 (%s)", recorder.Code, recorder.Body.String())
	}
}

func TestServiceErrorKeepsItsStatusAndCode(t *testing.T) {
	tests := []struct {
		err    error
		status int
		code   string
	}{
		{apierr.Unsupported("no_such"), http.StatusBadRequest, apierr.CodeUnsupported},
		{apierr.NoTargets("warehouse"), http.StatusNotFound, apierr.CodeNoTargets},
		{apierr.NoResultQueue(), http.StatusServiceUnavailable, apierr.CodeNoResultQueue},
		{apierr.Internal(io.EOF, "queue unreachable"), http.StatusInternalServerError, apierr.CodeInternal},
	}

	for _, test := range tests {
		t.Run(test.code, func(t *testing.T) {
			recorder := call(t, newTestHandler(&stubService{err: test.err}), http.MethodPost, "/api/v1/commands",
				`{"command":"current_status"}`)

			if recorder.Code != test.status {
				t.Fatalf("status = %d, want %d", recorder.Code, test.status)
			}

			decoded := decodeEnvelope(t, recorder)
			if decoded.Success {
				t.Error("an error response must not claim success")
			}
			if decoded.Error.Code != test.code {
				t.Errorf("code = %q, want %q", decoded.Error.Code, test.code)
			}
		})
	}
}

func TestInternalErrorDoesNotLeakItsCause(t *testing.T) {
	svc := &stubService{err: apierr.Internal(io.EOF, "cannot reach sqs at https://internal.host/queue")}

	recorder := call(t, newTestHandler(svc), http.MethodGet, "/api/v1/results", "")

	if strings.Contains(recorder.Body.String(), "internal.host") {
		t.Errorf("the response leaked an internal detail: %s", recorder.Body.String())
	}
}

func TestCommandsListsWhatTheAgentHandles(t *testing.T) {
	recorder := call(t, newTestHandler(&stubService{}), http.MethodGet, "/api/v1/commands", "")

	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", recorder.Code)
	}
	for _, command := range models.SupportedCommands() {
		if !strings.Contains(recorder.Body.String(), command) {
			t.Errorf("%q is missing from %s", command, recorder.Body.String())
		}
	}
}

func TestResultsReturnsTheBatch(t *testing.T) {
	svc := &stubService{batch: &service.ResultBatch{
		Count:   1,
		Reports: []models.Report{{DeviceID: "pc-001", Success: true}},
	}}

	recorder := call(t, newTestHandler(svc), http.MethodGet, "/api/v1/results", "")

	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", recorder.Code)
	}
	if !strings.Contains(recorder.Body.String(), "pc-001") {
		t.Errorf("the report is missing from %s", recorder.Body.String())
	}
}

func TestHealthEndpoints(t *testing.T) {
	handler := newTestHandler(&stubService{})

	for _, path := range []string{"/healthz", "/readyz"} {
		t.Run(path, func(t *testing.T) {
			recorder := call(t, handler, http.MethodGet, path, "")

			if recorder.Code != http.StatusOK {
				t.Fatalf("status = %d, want 200", recorder.Code)
			}
			if !decodeEnvelope(t, recorder).Success {
				t.Error("a healthy probe should report success")
			}
		})
	}
}

func TestWrongMethodIsRejected(t *testing.T) {
	recorder := call(t, newTestHandler(&stubService{}), http.MethodDelete, "/api/v1/deployments", "")

	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want 405", recorder.Code)
	}
}

func TestUnknownPathIsJSON(t *testing.T) {
	recorder := call(t, newTestHandler(&stubService{}), http.MethodGet, "/api/v1/nope", "")

	if recorder.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", recorder.Code)
	}
	if decodeEnvelope(t, recorder).Error.Code != "NOT_FOUND" {
		t.Errorf("unexpected body: %s", recorder.Body.String())
	}
}

func TestPanicBecomesA500(t *testing.T) {
	recorder := call(t, newTestHandler(&stubService{panics: true}), http.MethodPost, "/api/v1/deployments", `{"release_id":"a"}`)

	if recorder.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500", recorder.Code)
	}
	if decodeEnvelope(t, recorder).Success {
		t.Error("a recovered panic must not report success")
	}
}

func TestWriteEndpointsRejectBrowserFormPosts(t *testing.T) {
	handler := newTestHandler(&stubService{})

	for _, contentType := range []string{"text/plain", "application/x-www-form-urlencoded", "multipart/form-data; boundary=x", ""} {
		name := contentType
		if name == "" {
			name = "(none)"
		}

		t.Run(name, func(t *testing.T) {
			request := httptest.NewRequest(http.MethodPost, "/api/v1/commands", strings.NewReader(`{"command":"clean_old_version"}`))
			if contentType != "" {
				request.Header.Set("Content-Type", contentType)
			}

			recorder := httptest.NewRecorder()
			handler.ServeHTTP(recorder, request)

			if recorder.Code != http.StatusUnsupportedMediaType {
				t.Fatalf("status = %d, want 415 (%s)", recorder.Code, recorder.Body.String())
			}
			if decodeEnvelope(t, recorder).Error.Code != apierr.CodeUnsupportedMedia {
				t.Errorf("unexpected body: %s", recorder.Body.String())
			}
		})
	}
}

func TestJSONContentTypeAcceptsParameters(t *testing.T) {
	request := httptest.NewRequest(http.MethodPost, "/api/v1/commands", strings.NewReader(`{"command":"current_status"}`))
	request.Header.Set("Content-Type", "application/json; charset=utf-8")

	recorder := httptest.NewRecorder()
	newTestHandler(&stubService{result: &service.PublishResult{}}).ServeHTTP(recorder, request)

	if recorder.Code != http.StatusAccepted {
		t.Fatalf("status = %d, want 202 (%s)", recorder.Code, recorder.Body.String())
	}
}

func TestPathNormalizationRedirectKeepsItsLocation(t *testing.T) {
	handler := newTestHandler(&stubService{})

	request := httptest.NewRequest(http.MethodGet, "/api/v1//results", nil)
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)

	if recorder.Code < 300 || recorder.Code >= 400 {
		t.Skipf("이 Go 버전은 %d 로 답한다 — 리다이렉트 경로가 아니다", recorder.Code)
	}
	if recorder.Header().Get("Location") == "" {
		t.Fatalf("리다이렉트에 Location 이 없다: %d %s", recorder.Code, recorder.Body.String())
	}
}
