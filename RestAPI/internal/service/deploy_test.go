package service

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"strings"
	"sync"
	"testing"

	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/apierr"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/config"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/models"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/queue"
)

const validHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

type fakePublisher struct {
	mu sync.Mutex

	sent    map[string]string
	failFor map[string]error

	inFlight     int
	peakInFlight int
}

func newFakePublisher() *fakePublisher {
	return &fakePublisher{sent: map[string]string{}, failFor: map[string]error{}}
}

func (p *fakePublisher) Send(_ context.Context, queueURL, body string) (string, error) {
	p.mu.Lock()
	p.inFlight++
	p.peakInFlight = max(p.peakInFlight, p.inFlight)
	p.mu.Unlock()

	defer func() {
		p.mu.Lock()
		p.inFlight--
		p.mu.Unlock()
	}()

	p.mu.Lock()
	defer p.mu.Unlock()

	if err, failing := p.failFor[queueURL]; failing {
		return "", err
	}
	p.sent[queueURL] = body
	return "msg-" + queueURL, nil
}

// anyBody returns one published body; every target receives the same envelope.
func (p *fakePublisher) anyBody(t *testing.T) string {
	t.Helper()

	p.mu.Lock()
	defer p.mu.Unlock()

	for _, body := range p.sent {
		return body
	}
	t.Fatal("nothing was published")
	return ""
}

type fakeReceiver struct {
	messages   []queue.Message
	receiveErr error
	deleteErr  error
	deleted    []queue.Message
}

func (r *fakeReceiver) Receive(context.Context, string, int32, int32) ([]queue.Message, error) {
	return r.messages, r.receiveErr
}

func (r *fakeReceiver) Delete(_ context.Context, _ string, messages []queue.Message) error {
	r.deleted = messages
	return r.deleteErr
}

func newService(t *testing.T, cfg *config.Config) (*Service, *fakePublisher, *fakeReceiver) {
	t.Helper()

	publisher := newFakePublisher()
	receiver := &fakeReceiver{}
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))

	return New(cfg, publisher, receiver, logger), publisher, receiver
}

func fleet(count int) *config.Config {
	queues := make([]config.DeviceQueue, 0, count)
	for index := range count {
		queues = append(queues, config.DeviceQueue{
			Name:  fmt.Sprintf("pc-%03d", index),
			URL:   fmt.Sprintf("https://sqs.test/pc-%03d", index),
			Group: "kiosk",
		})
	}
	return &config.Config{DeviceQueues: queues, ResultQueueURL: "https://sqs.test/results", ResultBatchSize: 10}
}

func validDeploy() DeployRequest {
	return DeployRequest{
		ReleaseID: "rel_20260809",
		Group:     "kiosk",
		Artifacts: []models.Artifact{{InstallPath: "bin/app", SHA256: validHash, SizeBytes: 10}},
	}
}

func TestDeployFansOutToTheWholeGroup(t *testing.T) {
	svc, publisher, _ := newService(t, fleet(3))

	result, err := svc.Deploy(context.Background(), validDeploy())
	if err != nil {
		t.Fatalf("Deploy: %v", err)
	}

	if result.Targeted != 3 || result.Published != 3 {
		t.Fatalf("targeted/published = %d/%d, want 3/3", result.Targeted, result.Published)
	}
	if result.Command != models.CommandDownloadVersion {
		t.Errorf("command = %q, want %q", result.Command, models.CommandDownloadVersion)
	}
	if len(publisher.sent) != 3 {
		t.Errorf("published to %d queues, want 3", len(publisher.sent))
	}
}

// A large fleet must not open one connection per device at once.
func TestDeployBoundsConcurrency(t *testing.T) {
	svc, publisher, _ := newService(t, fleet(64))

	if _, err := svc.Deploy(context.Background(), validDeploy()); err != nil {
		t.Fatalf("Deploy: %v", err)
	}

	if publisher.peakInFlight > maxConcurrentSends {
		t.Errorf("peak concurrent sends = %d, want at most %d", publisher.peakInFlight, maxConcurrentSends)
	}
}

// Deliveries must stay in configuration order so the response is diffable.
func TestDeployKeepsDeliveryOrder(t *testing.T) {
	cfg := fleet(5)
	svc, _, _ := newService(t, cfg)

	result, err := svc.Deploy(context.Background(), validDeploy())
	if err != nil {
		t.Fatalf("Deploy: %v", err)
	}

	for index, delivery := range result.Deliveries {
		if delivery.Queue != cfg.DeviceQueues[index].Name {
			t.Fatalf("deliveries[%d] = %q, want %q", index, delivery.Queue, cfg.DeviceQueues[index].Name)
		}
	}
}

// One unreachable queue must not stop the rest of the fan-out.
func TestDeployReportsPartialFailure(t *testing.T) {
	cfg := fleet(3)
	svc, publisher, _ := newService(t, cfg)
	publisher.failFor[cfg.DeviceQueues[1].URL] = errors.New("queue does not exist")

	result, err := svc.Deploy(context.Background(), validDeploy())
	if err != nil {
		t.Fatalf("Deploy should report a partial failure, not fail: %v", err)
	}

	if result.Published != 2 {
		t.Fatalf("published = %d, want 2", result.Published)
	}
	if result.Deliveries[1].Error == "" {
		t.Error("the failing delivery should carry its reason")
	}
	if result.Deliveries[0].Error != "" || result.Deliveries[2].Error != "" {
		t.Error("the healthy deliveries should be clean")
	}
}

// Every rule here mirrors Artifact::make_object_key, so a request the agent
// would reject never reaches a queue.
func TestDeployRejectsInvalidRequests(t *testing.T) {
	tests := map[string]func(*DeployRequest){
		"no release_id":        func(r *DeployRequest) { r.ReleaseID = "" },
		"release_id separator": func(r *DeployRequest) { r.ReleaseID = "rel/1" },
		"no artifacts":         func(r *DeployRequest) { r.Artifacts = nil },
		"no install_path":      func(r *DeployRequest) { r.Artifacts[0].InstallPath = "" },
		"absolute path":        func(r *DeployRequest) { r.Artifacts[0].InstallPath = "/etc/passwd" },
		"drive letter":         func(r *DeployRequest) { r.Artifacts[0].InstallPath = "C:/app.exe" },
		"backslash":            func(r *DeployRequest) { r.Artifacts[0].InstallPath = `bin\app` },
		"parent reference":     func(r *DeployRequest) { r.Artifacts[0].InstallPath = "bin/../../etc/passwd" },
		"short hash":           func(r *DeployRequest) { r.Artifacts[0].SHA256 = "abc123" },
		"non-hex hash":         func(r *DeployRequest) { r.Artifacts[0].SHA256 = strings.Repeat("z", 64) },
	}

	for name, corrupt := range tests {
		t.Run(name, func(t *testing.T) {
			svc, publisher, _ := newService(t, fleet(2))

			request := validDeploy()
			corrupt(&request)

			if _, err := svc.Deploy(context.Background(), request); err == nil {
				t.Fatal("expected the request to be rejected")
			} else if !errors.Is(err, apierr.ErrBadRequest) {
				t.Fatalf("want a bad-request error, got %v", err)
			}

			if len(publisher.sent) != 0 {
				t.Error("a rejected request must not reach any queue")
			}
		})
	}
}

// The agent compares against a lowercase digest.
func TestDeployNormalizesHashCase(t *testing.T) {
	svc, publisher, _ := newService(t, fleet(1))

	request := validDeploy()
	request.Artifacts[0].SHA256 = strings.ToUpper(validHash)

	if _, err := svc.Deploy(context.Background(), request); err != nil {
		t.Fatalf("Deploy: %v", err)
	}

	if !strings.Contains(publisher.anyBody(t), validHash) {
		t.Errorf("published body should carry the lowercase digest, got %s", publisher.anyBody(t))
	}
}

func TestDeployRejectsUnknownGroup(t *testing.T) {
	svc, _, _ := newService(t, fleet(2))

	request := validDeploy()
	request.Group = "warehouse"

	_, err := svc.Deploy(context.Background(), request)
	if !errors.Is(err, apierr.ErrNoTargets) {
		t.Fatalf("want a no-targets error, got %v", err)
	}
}

func TestCommandRejectsUnsupported(t *testing.T) {
	svc, publisher, _ := newService(t, fleet(1))

	_, err := svc.Command(context.Background(), CommandRequest{Command: "restart_everything"})
	if !errors.Is(err, apierr.ErrUnsupported) {
		t.Fatalf("want an unsupported-command error, got %v", err)
	}
	if len(publisher.sent) != 0 {
		t.Error("an unsupported command must not reach any queue")
	}
}

// apply_version and rollback_version read release_id out of the payload.
func TestCommandRequiresReleaseIDWhereTheAgentReadsIt(t *testing.T) {
	for _, command := range []string{models.CommandApplyVersion, models.CommandRollbackVersion} {
		t.Run(command, func(t *testing.T) {
			svc, _, _ := newService(t, fleet(1))

			_, err := svc.Command(context.Background(), CommandRequest{Command: command, Payload: json.RawMessage(`{}`)})
			if !errors.Is(err, apierr.ErrBadRequest) {
				t.Fatalf("want a bad-request error, got %v", err)
			}

			_, err = svc.Command(context.Background(), CommandRequest{Command: command, Payload: json.RawMessage(`{"release_id":"rel_1"}`)})
			if err != nil {
				t.Fatalf("a payload with release_id should be accepted: %v", err)
			}
		})
	}
}

// current_status carries nothing; the agent expects an object, not null.
func TestCommandDefaultsEmptyPayloadToAnObject(t *testing.T) {
	svc, publisher, _ := newService(t, fleet(1))

	if _, err := svc.Command(context.Background(), CommandRequest{Command: models.CommandCurrentStatus}); err != nil {
		t.Fatalf("Command: %v", err)
	}

	var envelope models.AgentMessage
	if err := json.Unmarshal([]byte(publisher.anyBody(t)), &envelope); err != nil {
		t.Fatalf("published body is not an envelope: %v", err)
	}
	if string(envelope.Payload) != "{}" {
		t.Errorf("payload = %s, want {}", envelope.Payload)
	}
}

// Re-encoding through map[string]any would turn this into 1.2345678901234e+13.
func TestCommandForwardsPayloadVerbatim(t *testing.T) {
	svc, publisher, _ := newService(t, fleet(1))

	_, err := svc.Command(context.Background(), CommandRequest{
		Command: models.CommandCurrentStatus,
		Payload: json.RawMessage(`{"size_bytes":12345678901234,"note":"z"}`),
	})
	if err != nil {
		t.Fatalf("Command: %v", err)
	}

	if body := publisher.anyBody(t); !strings.Contains(body, "12345678901234") {
		t.Errorf("large integer lost its precision in %s", body)
	}
}

func TestCommandRejectsMalformedPayload(t *testing.T) {
	svc, _, _ := newService(t, fleet(1))

	_, err := svc.Command(context.Background(), CommandRequest{
		Command: models.CommandCurrentStatus,
		Payload: json.RawMessage(`{"broken"`),
	})
	if !errors.Is(err, apierr.ErrBadRequest) {
		t.Fatalf("want a bad-request error, got %v", err)
	}
}

// The envelope must stay byte-compatible with YirangAgent/AgentMessage.cpp.
func TestPublishedEnvelopeMatchesTheAgentContract(t *testing.T) {
	svc, publisher, _ := newService(t, fleet(1))

	if _, err := svc.Deploy(context.Background(), validDeploy()); err != nil {
		t.Fatalf("Deploy: %v", err)
	}

	var envelope struct {
		Command       string `json:"command"`
		Payload       json.RawMessage
		ReplyQueueURL string `json:"reply_queue_url"`
	}
	if err := json.Unmarshal([]byte(publisher.anyBody(t)), &envelope); err != nil {
		t.Fatalf("cannot decode the envelope: %v", err)
	}

	if envelope.Command != models.CommandDownloadVersion {
		t.Errorf("command = %q", envelope.Command)
	}
	if envelope.ReplyQueueURL != "https://sqs.test/results" {
		t.Errorf("reply_queue_url = %q, the agent would have nowhere to report", envelope.ReplyQueueURL)
	}
}

func TestResultsDecodesReports(t *testing.T) {
	svc, _, receiver := newService(t, fleet(1))
	receiver.messages = []queue.Message{
		{ID: "1", Body: `{"device_id":"pc-001","group":"kiosk","command":"current_status","success":true,"detail":"ok"}`},
		{ID: "2", Body: "this is not json"},
	}

	batch, err := svc.Results(context.Background())
	if err != nil {
		t.Fatalf("Results: %v", err)
	}

	if batch.Count != 1 || len(batch.Reports) != 1 {
		t.Fatalf("count = %d, want 1", batch.Count)
	}
	if batch.Skipped != 1 {
		t.Errorf("skipped = %d, want 1", batch.Skipped)
	}
	if batch.Reports[0].DeviceID != "pc-001" {
		t.Errorf("device_id = %q", batch.Reports[0].DeviceID)
	}
	// Everything read is acknowledged, including what could not be decoded;
	// leaving it would make it redeliver forever.
	if len(receiver.deleted) != 2 {
		t.Errorf("acknowledged %d messages, want 2", len(receiver.deleted))
	}
}

// A failed acknowledgement means a duplicate later, never a lost report.
func TestResultsReturnsReportsEvenIfAcknowledgementFails(t *testing.T) {
	svc, _, receiver := newService(t, fleet(1))
	receiver.messages = []queue.Message{{ID: "1", Body: `{"device_id":"pc-001","success":true}`}}
	receiver.deleteErr = errors.New("receipt handle expired")

	batch, err := svc.Results(context.Background())
	if err != nil {
		t.Fatalf("Results must not fail when only the delete failed: %v", err)
	}
	if batch.Count != 1 {
		t.Fatalf("count = %d, want the report to survive", batch.Count)
	}
}

func TestResultsRequiresAResultQueue(t *testing.T) {
	cfg := fleet(1)
	cfg.ResultQueueURL = ""
	svc, _, _ := newService(t, cfg)

	if _, err := svc.Results(context.Background()); !errors.Is(err, apierr.ErrNoResultQueue) {
		t.Fatalf("want a no-result-queue error, got %v", err)
	}
}

func TestResultsPropagatesReceiveFailure(t *testing.T) {
	svc, _, receiver := newService(t, fleet(1))
	receiver.receiveErr = errors.New("access denied")

	if _, err := svc.Results(context.Background()); !errors.Is(err, apierr.ErrInternal) {
		t.Fatalf("want an internal error, got %v", err)
	}
}

// /commands 로 같은 명령을 보내면 /deployments 의 검증을 우회할 수 있었다.
func TestCommandValidatesDownloadPayloadLikeDeploy(t *testing.T) {
	bad := map[string]string{
		"empty artifacts":  `{"release_id":"rel_1","artifacts":[]}`,
		"parent reference": `{"release_id":"rel_1","artifacts":[{"install_path":"../../etc/passwd","sha256":"` + validHash + `"}]}`,
		"release_id dots":  `{"release_id":"..","artifacts":[{"install_path":"bin/app","sha256":"` + validHash + `"}]}`,
		"non-hex hash":     `{"release_id":"rel_1","artifacts":[{"install_path":"bin/app","sha256":"zz"}]}`,
	}

	for name, payload := range bad {
		t.Run(name, func(t *testing.T) {
			svc, publisher, _ := newService(t, fleet(1))

			_, err := svc.Command(context.Background(), CommandRequest{
				Command: models.CommandDownloadVersion,
				Payload: json.RawMessage(payload),
			})
			if !errors.Is(err, apierr.ErrBadRequest) {
				t.Fatalf("want a bad-request error, got %v", err)
			}
			if len(publisher.sent) != 0 {
				t.Error("a rejected payload must not reach any queue")
			}
		})
	}
}

// "." 과 ".." 는 구분자를 포함하지 않아 종전 검사를 통과했다.
func TestDeployRejectsPathReferenceReleaseID(t *testing.T) {
	for _, releaseID := range []string{".", ".."} {
		t.Run(releaseID, func(t *testing.T) {
			svc, publisher, _ := newService(t, fleet(1))

			request := validDeploy()
			request.ReleaseID = releaseID

			if _, err := svc.Deploy(context.Background(), request); !errors.Is(err, apierr.ErrBadRequest) {
				t.Fatalf("want a bad-request error, got %v", err)
			}
			if len(publisher.sent) != 0 {
				t.Error("a rejected release_id must not reach any queue")
			}
		})
	}
}

// 부분 실패는 202 지만 0건 수락은 성공이 아니다.
func TestPublishFailsWhenNoTargetAccepts(t *testing.T) {
	cfg := fleet(2)
	svc, publisher, _ := newService(t, cfg)
	for _, queue := range cfg.DeviceQueues {
		publisher.failFor[queue.URL] = errors.New("expired credentials")
	}

	_, err := svc.Deploy(context.Background(), validDeploy())
	if err == nil {
		t.Fatal("expected an error when every target failed")
	}
	if got := apierr.From(err).Code; got != apierr.CodePublishFailed {
		t.Fatalf("code = %q, want %q", got, apierr.CodePublishFailed)
	}
}
