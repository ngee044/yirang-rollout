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

// maxRequestBytes caps a request body. A release manifest of a few hundred
// artifacts stays far below this; anything larger is a mistake or an attack.
const maxRequestBytes = 1 << 20

// envelope is the single response shape for every endpoint, success or not.
type envelope struct {
	Success bool       `json:"success"`
	Data    any        `json:"data,omitempty"`
	Error   *errorBody `json:"error,omitempty"`
}

type errorBody struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

// decodeJSON reads exactly one JSON object of type T from the request.
//
// Unknown fields are rejected rather than ignored: a misspelled "install_path"
// would otherwise be dropped in silence and deploy an empty release.
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

	// A second document in the same body means the client built the request
	// incorrectly; only the first would ever be acted on.
	if decoder.More() {
		return value, apierr.BadRequest("request body must contain exactly one JSON object")
	}

	return value, nil
}

// requireJSONContentType rejects anything a browser could have sent without a
// CORS preflight. See apierr.UnsupportedMedia for why that matters here.
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
	// Encode before touching the header so a marshal failure does not land
	// after a status has already been committed.
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

// writeError maps any error onto its response. The wrapped cause is logged and
// never sent, so an internal failure does not leak its detail to the client.
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
