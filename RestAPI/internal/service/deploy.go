package service

import (
	"context"
	"encoding/json"
	"log/slog"
	"strings"
	"sync"

	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/apierr"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/config"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/models"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/queue"
)

const maxConcurrentSends = 8

type Publisher interface {
	Send(ctx context.Context, queueURL, body string) (string, error)
}

type Receiver interface {
	Receive(ctx context.Context, queueURL string, max, waitSeconds int32) ([]queue.Message, error)
	Delete(ctx context.Context, queueURL string, messages []queue.Message) error
}

type DeployRequest struct {
	ReleaseID string            `json:"release_id"`
	Group     string            `json:"group"`
	Artifacts []models.Artifact `json:"artifacts"`
}

type CommandRequest struct {
	Command string          `json:"command"`
	Group   string          `json:"group"`
	Payload json.RawMessage `json:"payload"`
}

type Delivery struct {
	Queue     string `json:"queue"`
	QueueURL  string `json:"queue_url"`
	MessageID string `json:"message_id,omitempty"`
	Error     string `json:"error,omitempty"`
}

type PublishResult struct {
	Command    string     `json:"command"`
	Targeted   int        `json:"targeted"`
	Published  int        `json:"published"`
	Deliveries []Delivery `json:"deliveries"`
}

type ResultBatch struct {
	Count   int             `json:"count"`
	Reports []models.Report `json:"reports"`

	Skipped int `json:"skipped,omitempty"`
}

type Service struct {
	cfg       *config.Config
	publisher Publisher
	receiver  Receiver
	logger    *slog.Logger
}

func New(cfg *config.Config, publisher Publisher, receiver Receiver, logger *slog.Logger) *Service {
	return &Service{cfg: cfg, publisher: publisher, receiver: receiver, logger: logger}
}

func (s *Service) Deploy(ctx context.Context, request DeployRequest) (*PublishResult, error) {
	payload, err := downloadPayload(request.ReleaseID, request.Artifacts)
	if err != nil {
		return nil, err
	}

	return s.publish(ctx, models.CommandDownloadVersion, request.Group, payload)
}

func downloadPayload(releaseID string, artifacts []models.Artifact) (json.RawMessage, error) {
	if len(artifacts) == 0 {
		return nil, apierr.BadRequest("artifacts must not be empty")
	}

	normalized := make([]models.Artifact, len(artifacts))
	for index, artifact := range artifacts {
		if err := validateObjectKey(releaseID, artifact.InstallPath, index); err != nil {
			return nil, err
		}

		if !isSHA256(artifact.SHA256) {
			return nil, apierr.BadRequest("artifacts[%d].sha256 must be 64 hexadecimal characters", index)
		}

		normalized[index] = artifact
		normalized[index].SHA256 = strings.ToLower(artifact.SHA256)
	}

	encoded, err := json.Marshal(models.DownloadPayload{ReleaseID: releaseID, Artifacts: normalized})
	if err != nil {
		return nil, apierr.Internal(err, "cannot encode download payload")
	}

	return encoded, nil
}

func (s *Service) Command(ctx context.Context, request CommandRequest) (*PublishResult, error) {
	if !models.IsSupportedCommand(request.Command) {
		return nil, apierr.Unsupported(request.Command)
	}

	payload := request.Payload
	if len(payload) == 0 {
		payload = json.RawMessage(`{}`)
	}
	if !json.Valid(payload) {
		return nil, apierr.BadRequest("payload must be valid JSON")
	}

	switch request.Command {
	case models.CommandDownloadVersion:
		var download models.DownloadPayload
		if err := json.Unmarshal(payload, &download); err != nil {
			return nil, apierr.BadRequest("download_version payload must be an object with release_id and artifacts")
		}

		validated, err := downloadPayload(download.ReleaseID, download.Artifacts)
		if err != nil {
			return nil, err
		}
		payload = validated

	case models.CommandApplyVersion, models.CommandRollbackVersion:
		var version models.VersionPayload
		if err := json.Unmarshal(payload, &version); err != nil || version.ReleaseID == "" {
			return nil, apierr.BadRequest("%s requires payload.release_id", request.Command)
		}
	}

	return s.publish(ctx, request.Command, request.Group, payload)
}

func (s *Service) publish(ctx context.Context, command, group string, payload json.RawMessage) (*PublishResult, error) {
	targets := s.cfg.QueuesForGroup(group)
	if len(targets) == 0 {
		return nil, apierr.NoTargets(group)
	}

	body, err := json.Marshal(models.AgentMessage{
		Command:       command,
		Payload:       payload,
		ReplyQueueURL: s.cfg.ResultQueueURL,
	})
	if err != nil {
		return nil, apierr.Internal(err, "cannot encode agent message")
	}

	deliveries := make([]Delivery, len(targets))
	slots := make(chan struct{}, min(maxConcurrentSends, len(targets)))

	var waiter sync.WaitGroup
	for index, target := range targets {
		waiter.Add(1)
		go func() {
			defer waiter.Done()

			slots <- struct{}{}
			defer func() { <-slots }()

			delivery := Delivery{Queue: target.Name, QueueURL: target.URL}
			messageID, sendErr := s.publisher.Send(ctx, target.URL, string(body))
			if sendErr != nil {
				delivery.Error = sendErr.Error()
			} else {
				delivery.MessageID = messageID
			}
			deliveries[index] = delivery
		}()
	}
	waiter.Wait()

	result := &PublishResult{Command: command, Targeted: len(targets), Deliveries: deliveries}
	for _, delivery := range deliveries {
		if delivery.Error == "" {
			result.Published++
			continue
		}
		s.logger.Error("publish failed", "command", command, "queue", delivery.Queue, "error", delivery.Error)
	}

	if result.Published == 0 {
		return nil, apierr.AllTargetsFailed(result.Targeted)
	}

	return result, nil
}

func (s *Service) Results(ctx context.Context) (*ResultBatch, error) {
	if s.cfg.ResultQueueURL == "" {
		return nil, apierr.NoResultQueue()
	}

	messages, err := s.receiver.Receive(ctx, s.cfg.ResultQueueURL, int32(s.cfg.ResultBatchSize), s.cfg.ResultWaitSeconds)
	if err != nil {
		return nil, apierr.Internal(err, "cannot receive from the result queue")
	}

	batch := &ResultBatch{Reports: make([]models.Report, 0, len(messages))}
	for _, message := range messages {
		var report models.Report
		if decodeErr := json.Unmarshal([]byte(message.Body), &report); decodeErr != nil {
			batch.Skipped++
			s.logger.Warn("undecodable result message", "message_id", message.ID, "error", decodeErr)
			continue
		}
		batch.Reports = append(batch.Reports, report)
	}
	batch.Count = len(batch.Reports)

	if deleteErr := s.receiver.Delete(ctx, s.cfg.ResultQueueURL, messages); deleteErr != nil {
		s.logger.Error("cannot acknowledge result messages", "count", len(messages), "error", deleteErr)
	}

	return batch, nil
}

func validateObjectKey(releaseID, installPath string, index int) error {
	if releaseID == "" {
		return apierr.BadRequest("release_id is required")
	}
	if strings.ContainsAny(releaseID, `/\`) {
		return apierr.BadRequest("release_id must not contain a path separator: %q", releaseID)
	}
	if releaseID == "." || releaseID == ".." {
		return apierr.BadRequest("release_id must not be a path reference: %q", releaseID)
	}
	if installPath == "" {
		return apierr.BadRequest("artifacts[%d].install_path is required", index)
	}
	if strings.Contains(installPath, `\`) {
		return apierr.BadRequest("artifacts[%d].install_path must use '/' as separator: %q", index, installPath)
	}
	if strings.HasPrefix(installPath, "/") || (len(installPath) >= 2 && installPath[1] == ':') {
		return apierr.BadRequest("artifacts[%d].install_path must be relative: %q", index, installPath)
	}
	for _, segment := range strings.Split(installPath, "/") {
		if segment == ".." {
			return apierr.BadRequest("artifacts[%d].install_path must not contain '..': %q", index, installPath)
		}
	}
	return nil
}

func isSHA256(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, character := range value {
		switch {
		case character >= '0' && character <= '9':
		case character >= 'a' && character <= 'f':
		case character >= 'A' && character <= 'F':
		default:
			return false
		}
	}
	return true
}
