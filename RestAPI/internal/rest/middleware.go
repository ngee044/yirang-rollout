package rest

import (
	"context"
	"log/slog"
	"net/http"
	"runtime/debug"
	"time"
)

type middleware func(http.Handler) http.Handler

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

func recoverPanic(logger *slog.Logger) middleware {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			defer func() {
				recovered := recover()
				if recovered == nil {
					return
				}

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

func withTimeout(budget time.Duration) middleware {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			ctx, cancel := context.WithTimeout(r.Context(), budget)
			defer cancel()

			next.ServeHTTP(w, r.WithContext(ctx))
		})
	}
}
