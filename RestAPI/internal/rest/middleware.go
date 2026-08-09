package rest

import (
	"context"
	"log/slog"
	"net/http"
	"runtime/debug"
	"time"
)

type middleware func(http.Handler) http.Handler

// statusWriter remembers what was written so the access log can report it.
type statusWriter struct {
	http.ResponseWriter
	status int
	bytes  int
}

func (w *statusWriter) WriteHeader(status int) {
	w.status = status
	w.ResponseWriter.WriteHeader(status)
}

func (w *statusWriter) Write(payload []byte) (int, error) {
	if w.status == 0 {
		w.status = http.StatusOK
	}
	written, err := w.ResponseWriter.Write(payload)
	w.bytes += written
	return written, err
}

func logRequests(logger *slog.Logger) middleware {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			started := time.Now()
			recorder := &statusWriter{ResponseWriter: w}

			next.ServeHTTP(recorder, r)

			if recorder.status == 0 {
				recorder.status = http.StatusOK
			}

			logger.Info("http.request",
				"method", r.Method,
				"path", r.URL.Path,
				"status", recorder.status,
				"bytes", recorder.bytes,
				"duration_ms", time.Since(started).Milliseconds(),
				"remote", r.RemoteAddr,
			)
		})
	}
}

// recoverPanic keeps one bad request from taking the process down with it.
func recoverPanic(logger *slog.Logger) middleware {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			defer func() {
				recovered := recover()
				if recovered == nil {
					return
				}

				// A panic after the handler already committed a status cannot
				// be turned into a 500, so only the log is left.
				logger.Error("panic recovered", "path", r.URL.Path, "panic", recovered, "stack", string(debug.Stack()))

				writeJSON(logger, w, http.StatusInternalServerError, envelope{
					Success: false,
					Error:   &errorBody{Code: "INTERNAL", Message: "internal error"},
				})
			}()

			next.ServeHTTP(w, r)
		})
	}
}

// withTimeout bounds how long a handler may spend downstream. The deadline is
// carried on the context so the SQS calls are cancelled too, rather than
// running on after the client has been answered.
func withTimeout(budget time.Duration) middleware {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			ctx, cancel := context.WithTimeout(r.Context(), budget)
			defer cancel()

			next.ServeHTTP(w, r.WithContext(ctx))
		})
	}
}
