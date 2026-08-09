// Package rest is the transport layer: it decodes HTTP, calls the service, and
// encodes the answer. It holds no rule about what a command means.
package rest

import (
	"context"
	"net/http"

	"log/slog"

	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/config"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/service"
)

// deployer is the slice of the service this layer uses. Declaring it here, and
// not in the service package, is what lets a test drive the routes with a fake.
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

// NewHandler wires the routes and the middleware chain.
//
// Routing is stdlib: since Go 1.22 ServeMux matches "METHOD /path" and answers
// 405 on its own, which is all six of these endpoints need from a router.
func NewHandler(cfg *config.Config, svc deployer, logger *slog.Logger) http.Handler {
	server := &Server{cfg: cfg, svc: svc, logger: logger}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", server.liveness)
	mux.HandleFunc("GET /readyz", server.readiness)
	mux.HandleFunc("POST /api/v1/deployments", server.deploy)
	mux.HandleFunc("POST /api/v1/commands", server.command)
	mux.HandleFunc("GET /api/v1/commands", server.commands)
	mux.HandleFunc("GET /api/v1/results", server.results)

	// Outermost first: recover has to survive a panic raised inside logging,
	// and the deadline has to be on the context every handler below sees.
	return recoverPanic(server.logger)(
		logRequests(server.logger)(
			withTimeout(cfg.RequestTimeout)(
				server.jsonFallback(mux))))
}

// statusProbe captures what a handler would have answered without writing it.
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

// jsonFallback replaces ServeMux's plain-text 404 and 405 with the envelope
// every other endpoint answers with, so a client never has to parse two shapes.
//
// Registering a "/" catch-all instead would be simpler but would swallow 405:
// the catch-all matches a method mismatch too, and the mux would stop reporting
// which methods the path does accept.
func (s *Server) jsonFallback(mux *http.ServeMux) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		handler, pattern := mux.Handler(r)
		if pattern != "" {
			handler.ServeHTTP(w, r)
			return
		}

		// Unmatched. The mux still knows whether this is 404 or 405, and fills
		// in Allow for the latter, so ask it and keep only the verdict.
		probe := &statusProbe{header: http.Header{}}
		handler.ServeHTTP(probe, r)

		// mux 는 비정규 경로에 대해 pattern 이 빈 리다이렉트 핸들러도 돌려준다. 그 상태를 삼키면
		// Location 없는 307 이 나가므로, 404·405 가 아니면 원래 핸들러에 그대로 위임한다.
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
