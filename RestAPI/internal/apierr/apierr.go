package apierr

import (
	"context"
	"errors"
	"fmt"
	"net/http"
)

const (
	CodeBadRequest       = "BAD_REQUEST"
	CodeTooLarge         = "REQUEST_TOO_LARGE"
	CodeUnsupportedMedia = "UNSUPPORTED_MEDIA_TYPE"
	CodeUnsupported      = "UNSUPPORTED_COMMAND"
	CodeNoTargets        = "NO_TARGETS"
	CodeNoResultQueue    = "NO_RESULT_QUEUE"
	CodePublishFailed    = "PUBLISH_FAILED"
	CodeTimeout          = "TIMEOUT"
	CodeClientClosed     = "CLIENT_CLOSED"
	CodeInternal         = "INTERNAL"
)

const statusClientClosed = 499

type Error struct {
	Code    string
	Message string
	Status  int

	cause error
}

func (e *Error) Error() string {
	if e.cause == nil {
		return e.Message
	}
	return e.Message + ": " + e.cause.Error()
}

func (e *Error) Unwrap() error { return e.cause }

func (e *Error) ClientMessage() string {
	if e.Code == CodeInternal {
		return "internal error"
	}
	return e.Message
}

func (e *Error) Is(target error) bool {
	var other *Error
	return errors.As(target, &other) && other.Code == e.Code
}

var (
	ErrBadRequest    = &Error{Code: CodeBadRequest, Status: http.StatusBadRequest}
	ErrUnsupported   = &Error{Code: CodeUnsupported, Status: http.StatusBadRequest}
	ErrNoTargets     = &Error{Code: CodeNoTargets, Status: http.StatusNotFound}
	ErrNoResultQueue = &Error{Code: CodeNoResultQueue, Status: http.StatusServiceUnavailable}
	ErrInternal      = &Error{Code: CodeInternal, Status: http.StatusInternalServerError}
)

func BadRequest(format string, args ...any) *Error {
	return &Error{Code: CodeBadRequest, Message: fmt.Sprintf(format, args...), Status: http.StatusBadRequest}
}

func Unsupported(command string) *Error {
	return &Error{
		Code:    CodeUnsupported,
		Message: fmt.Sprintf("the agent has no handler for command %q", command),
		Status:  http.StatusBadRequest,
	}
}

func NoTargets(group string) *Error {
	return &Error{
		Code:    CodeNoTargets,
		Message: fmt.Sprintf("no device queue is registered for group %q", group),
		Status:  http.StatusNotFound,
	}
}

func NoResultQueue() *Error {
	return &Error{
		Code:    CodeNoResultQueue,
		Message: "RESULT_QUEUE_URL is not configured, so no reports can be collected",
		Status:  http.StatusServiceUnavailable,
	}
}

func TooLarge(limit int64) *Error {
	return &Error{
		Code:    CodeTooLarge,
		Message: fmt.Sprintf("request body must not exceed %d bytes", limit),
		Status:  http.StatusRequestEntityTooLarge,
	}
}

func UnsupportedMedia(got string) *Error {
	if got == "" {
		got = "(none)"
	}
	return &Error{
		Code:    CodeUnsupportedMedia,
		Message: fmt.Sprintf("Content-Type must be application/json, got %s", got),
		Status:  http.StatusUnsupportedMediaType,
	}
}

func AllTargetsFailed(targeted int) *Error {
	return &Error{
		Code:    CodePublishFailed,
		Message: fmt.Sprintf("no queue accepted the command (%d of %d targets failed); see server logs for the per-queue reason", targeted, targeted),
		Status:  http.StatusBadGateway,
	}
}

func Internal(cause error, format string, args ...any) *Error {
	return &Error{
		Code:    CodeInternal,
		Message: fmt.Sprintf(format, args...),
		Status:  http.StatusInternalServerError,
		cause:   cause,
	}
}

func From(err error) *Error {
	switch {
	case errors.Is(err, context.DeadlineExceeded):
		return &Error{Code: CodeTimeout, Message: "the request exceeded its time budget", Status: http.StatusGatewayTimeout, cause: err}
	case errors.Is(err, context.Canceled):
		return &Error{Code: CodeClientClosed, Message: "the client closed the request", Status: statusClientClosed, cause: err}
	}

	var apiErr *Error
	if errors.As(err, &apiErr) {
		return apiErr
	}
	return &Error{Code: CodeInternal, Message: "internal error", Status: http.StatusInternalServerError, cause: err}
}
