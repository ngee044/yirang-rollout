package rest

import (
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"mime"
	"net/http"

	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/apierr"
)

const maxRequestBytes = 1 << 20

type envelope struct {
	Success bool       `json:"success"`
	Data    any        `json:"data,omitempty"`
	Error   *errorBody `json:"error,omitempty"`
}

type errorBody struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

func decodeJSON[T any](w http.ResponseWriter, r *http.Request) (T, error) {
	var value T

	if err := requireJSONContentType(r); err != nil {
		return value, err
	}

	r.Body = http.MaxBytesReader(w, r.Body, maxRequestBytes)

	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()

	if err := decoder.Decode(&value); err != nil {
		return value, decodeError(err)
	}

	if decoder.More() {
		return value, apierr.BadRequest("request body must contain exactly one JSON object")
	}

	return value, nil
}

func requireJSONContentType(r *http.Request) error {
	raw := r.Header.Get("Content-Type")

	mediaType, _, err := mime.ParseMediaType(raw)
	if err != nil || mediaType != "application/json" {
		return apierr.UnsupportedMedia(raw)
	}
	return nil
}

func decodeError(err error) error {
	var tooLarge *http.MaxBytesError
	if errors.As(err, &tooLarge) {
		return apierr.TooLarge(maxRequestBytes)
	}

	if errors.Is(err, io.EOF) {
		return apierr.BadRequest("request body is empty")
	}

	var syntax *json.SyntaxError
	if errors.As(err, &syntax) {
		return apierr.BadRequest("request body is not valid JSON (at byte %d)", syntax.Offset)
	}

	var typeErr *json.UnmarshalTypeError
	if errors.As(err, &typeErr) {
		return apierr.BadRequest("field %q must be of type %s", typeErr.Field, typeErr.Type)
	}

	return apierr.BadRequest("cannot decode request body: %v", err)
}

func writeJSON(logger *slog.Logger, w http.ResponseWriter, status int, body envelope) {
	encoded, err := json.Marshal(body)
	if err != nil {
		logger.Error("cannot encode response", "error", err)
		http.Error(w, `{"success":false,"error":{"code":"INTERNAL","message":"internal error"}}`, http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)

	if _, err := w.Write(encoded); err != nil {
		logger.Warn("cannot write response", "error", err)
	}
}

func writeData(logger *slog.Logger, w http.ResponseWriter, status int, data any) {
	writeJSON(logger, w, status, envelope{Success: true, Data: data})
}

func writeError(logger *slog.Logger, w http.ResponseWriter, r *http.Request, err error) {
	apiError := apierr.From(err)

	attributes := []any{"path", r.URL.Path, "code", apiError.Code, "error", err}
	if apiError.Status >= http.StatusInternalServerError {
		logger.Error("request failed", attributes...)
	} else {
		logger.Warn("request rejected", attributes...)
	}

	writeJSON(logger, w, apiError.Status, envelope{
		Success: false,
		Error:   &errorBody{Code: apiError.Code, Message: apiError.ClientMessage()},
	})
}
