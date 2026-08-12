package rest

import (
	"net/http"

	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/models"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/service"
)

func (s *Server) liveness(w http.ResponseWriter, _ *http.Request) {
	writeData(s.logger, w, http.StatusOK, map[string]any{"status": "ok"})
}

func (s *Server) readiness(w http.ResponseWriter, _ *http.Request) {
	writeData(s.logger, w, http.StatusOK, map[string]any{
		"device_queues":    len(s.cfg.DeviceQueues),
		"result_queue_set": s.cfg.ResultQueueURL != "",
	})
}

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

	writeData(s.logger, w, http.StatusAccepted, result)
}

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

func (s *Server) commands(w http.ResponseWriter, _ *http.Request) {
	writeData(s.logger, w, http.StatusOK, map[string]any{"commands": models.SupportedCommands()})
}

func (s *Server) results(w http.ResponseWriter, r *http.Request) {
	batch, err := s.svc.Results(r.Context())
	if err != nil {
		writeError(s.logger, w, r, err)
		return
	}

	writeData(s.logger, w, http.StatusOK, batch)
}
