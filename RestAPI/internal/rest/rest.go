package rest

import (
	"context"
	"net/http"

	"log/slog"

	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/config"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/service"
)

type deployer interface {
	Deploy(ctx context.Context, request service.DeployRequest) (*service.PublishResult, error)
	Command(ctx context.Context, request service.CommandRequest) (*service.PublishResult, error)
	Results(ctx context.Context) (*service.ResultBatch, error)
}

type Server struct {
	cfg    *config.Config
	svc    deployer
	logger *slog.Logger
}

func NewHandler(cfg *config.Config, svc deployer, logger *slog.Logger) http.Handler {
	server := &Server{cfg: cfg, svc: svc, logger: logger}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", server.liveness)
	mux.HandleFunc("GET /readyz", server.readiness)
	mux.HandleFunc("POST /api/v1/deployments", server.deploy)
	mux.HandleFunc("POST /api/v1/commands", server.command)
	mux.HandleFunc("GET /api/v1/commands", server.commands)
	mux.HandleFunc("GET /api/v1/results", server.results)

	return recoverPanic(server.logger)(
		logRequests(server.logger)(
			withTimeout(cfg.RequestTimeout)(
				server.jsonFallback(mux))))
}

type statusProbe struct {
	header http.Header
	status int
}

func (p *statusProbe) Header() http.Header { return p.header }

func (p *statusProbe) Write(payload []byte) (int, error) { return len(payload), nil }

func (p *statusProbe) WriteHeader(status int) {
	if p.status == 0 {
		p.status = status
	}
}

func (s *Server) jsonFallback(mux *http.ServeMux) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		handler, pattern := mux.Handler(r)
		if pattern != "" {
			handler.ServeHTTP(w, r)
			return
		}

		probe := &statusProbe{header: http.Header{}}
		handler.ServeHTTP(probe, r)

		if probe.status != http.StatusNotFound && probe.status != http.StatusMethodNotAllowed {
			handler.ServeHTTP(w, r)
			return
		}

		code, message := "NOT_FOUND", "no such endpoint: "+r.Method+" "+r.URL.Path
		if probe.status == http.StatusMethodNotAllowed {
			allowed := probe.header.Get("Allow")
			w.Header().Set("Allow", allowed)
			code, message = "METHOD_NOT_ALLOWED", r.URL.Path+" accepts "+allowed
		}

		writeJSON(s.logger, w, probe.status, envelope{Success: false, Error: &errorBody{Code: code, Message: message}})
	})
}
