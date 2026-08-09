package rest

import (
	"net/http"

	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/models"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/service"
)

// liveness answers as long as the process is running.
func (s *Server) liveness(w http.ResponseWriter, _ *http.Request) {
	writeData(s.logger, w, http.StatusOK, map[string]any{"status": "ok"})
}

// readiness reports what the server was configured to do. It deliberately makes
// no SQS call: a probe runs every few seconds and must not spend quota, and a
// queue outage is reported per delivery where it can actually be acted on.
func (s *Server) readiness(w http.ResponseWriter, _ *http.Request) {
	writeData(s.logger, w, http.StatusOK, map[string]any{
		"device_queues":    len(s.cfg.DeviceQueues),
		"result_queue_set": s.cfg.ResultQueueURL != "",
	})
}

// deploy publishes download_version to every queue in the group.
func (s *Server) deploy(w http.ResponseWriter, r *http.Request) {
	request, err := decodeJSON[service.DeployRequest](w, r)
	if err != nil {
		writeError(s.logger, w, r, err)
		return
	}

	result, err := s.svc.Deploy(r.Context(), request)
	if err != nil {
		writeError(s.logger, w, r, err)
		return
	}

	// 202, not 200: the queues accepted the command, the devices have not yet
	// acted on it. The outcome arrives later on the result queue.
	writeData(s.logger, w, http.StatusAccepted, result)
}

// command publishes any supported agent command.
func (s *Server) command(w http.ResponseWriter, r *http.Request) {
	request, err := decodeJSON[service.CommandRequest](w, r)
	if err != nil {
		writeError(s.logger, w, r, err)
		return
	}

	result, err := s.svc.Command(r.Context(), request)
	if err != nil {
		writeError(s.logger, w, r, err)
		return
	}

	writeData(s.logger, w, http.StatusAccepted, result)
}

// commands lists what the agent can handle so the CLI can reject a typo before
// it costs a round trip.
func (s *Server) commands(w http.ResponseWriter, _ *http.Request) {
	writeData(s.logger, w, http.StatusOK, map[string]any{"commands": models.SupportedCommands()})
}

// results drains the result queue and returns the reports it decoded.
func (s *Server) results(w http.ResponseWriter, r *http.Request) {
	batch, err := s.svc.Results(r.Context())
	if err != nil {
		writeError(s.logger, w, r, err)
		return
	}

	writeData(s.logger, w, http.StatusOK, batch)
}
