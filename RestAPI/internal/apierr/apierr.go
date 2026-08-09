// Package apierr defines the failure classes this API can answer with.
//
// It is deliberately not named "errors": a package that shadows a stdlib name
// forces an import alias on every call site that needs both.
package apierr

import (
	"context"
	"errors"
	"fmt"
	"net/http"
)

// Stable machine-readable codes. Clients branch on these, so they are part of
// the API contract and must not change with the wording of a message.
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

// statusClientClosed is nginx's convention for a client that hung up mid-request.
// Nothing reads the response, so the code exists only to keep the access log honest.
const statusClientClosed = 499

// Error pairs a code and an HTTP status with an optional wrapped cause. The
// cause stays out of the response body and is only logged.
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

// ClientMessage is the only text safe to put in a response body.
//
// An internal failure answers with a fixed string: its message is written for
// an operator reading logs and may name a host, a queue URL, or a credential
// path. Every other class is caller-facing by construction.
func (e *Error) ClientMessage() string {
	if e.Code == CodeInternal {
		return "internal error"
	}
	return e.Message
}

// Is matches on the code alone so errors.Is finds the class through a wrap.
func (e *Error) Is(target error) bool {
	var other *Error
	return errors.As(target, &other) && other.Code == e.Code
}

// Sentinels for errors.Is comparison. Constructors below carry the detail.
var (
	ErrBadRequest    = &Error{Code: CodeBadRequest, Status: http.StatusBadRequest}
	ErrUnsupported   = &Error{Code: CodeUnsupported, Status: http.StatusBadRequest}
	ErrNoTargets     = &Error{Code: CodeNoTargets, Status: http.StatusNotFound}
	ErrNoResultQueue = &Error{Code: CodeNoResultQueue, Status: http.StatusServiceUnavailable}
	ErrInternal      = &Error{Code: CodeInternal, Status: http.StatusInternalServerError}
)

// BadRequest reports input the caller can fix. The message reaches the client,
// so it names the offending field.
func BadRequest(format string, args ...any) *Error {
	return &Error{Code: CodeBadRequest, Message: fmt.Sprintf(format, args...), Status: http.StatusBadRequest}
}

// Unsupported reports a command the agent has no handler for. Publishing it
// would only produce a failure report from every device.
func Unsupported(command string) *Error {
	return &Error{
		Code:    CodeUnsupported,
		Message: fmt.Sprintf("the agent has no handler for command %q", command),
		Status:  http.StatusBadRequest,
	}
}

// NoTargets reports that the requested group matches no registered queue. This
// is 404 rather than 400: the request is well-formed, the group just is not here.
func NoTargets(group string) *Error {
	return &Error{
		Code:    CodeNoTargets,
		Message: fmt.Sprintf("no device queue is registered for group %q", group),
		Status:  http.StatusNotFound,
	}
}

// NoResultQueue reports a capability this deployment was not configured for.
func NoResultQueue() *Error {
	return &Error{
		Code:    CodeNoResultQueue,
		Message: "RESULT_QUEUE_URL is not configured, so no reports can be collected",
		Status:  http.StatusServiceUnavailable,
	}
}

// TooLarge reports a body that exceeded the read limit.
func TooLarge(limit int64) *Error {
	return &Error{
		Code:    CodeTooLarge,
		Message: fmt.Sprintf("request body must not exceed %d bytes", limit),
		Status:  http.StatusRequestEntityTooLarge,
	}
}

// UnsupportedMedia reports a body this API will not read.
//
// It exists for more than tidiness: the API is unauthenticated on loopback, and
// a browser can send a cross-origin form POST with text/plain or
// application/x-www-form-urlencoded without a preflight. Requiring JSON makes
// every write a preflighted request, which the browser blocks on its own.
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

// AllTargetsFailed reports that not one queue accepted the command.
//
// A partial fan-out is deliberately answered with 202 — some devices got it. Zero
// accepted is a different outcome: nothing was queued, so answering 202 would tell
// the caller a deployment is under way when none is.
func AllTargetsFailed(targeted int) *Error {
	return &Error{
		Code:    CodePublishFailed,
		Message: fmt.Sprintf("no queue accepted the command (%d of %d targets failed); see server logs for the per-queue reason", targeted, targeted),
		Status:  http.StatusBadGateway,
	}
}

// Internal hides the cause from the client but keeps it on the chain for logs.
func Internal(cause error, format string, args ...any) *Error {
	return &Error{
		Code:    CodeInternal,
		Message: fmt.Sprintf(format, args...),
		Status:  http.StatusInternalServerError,
		cause:   cause,
	}
}

// From maps any error onto the response it should produce. An error that is not
// ours is a bug rather than a client mistake, so it becomes a 500 with a
// generic message.
func From(err error) *Error {
	// Checked before the type match: a context failure is usually already
	// wrapped as Internal, and answering 500 would send an operator hunting
	// for a bug that is really a deadline or a hung-up client.
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
