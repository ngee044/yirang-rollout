// Package models holds the wire contract shared with YirangAgent (C++).
package models

import (
	"encoding/json"
	"slices"
)

// AgentMessage is the envelope every device queue carries.
// It is the Go half of YirangAgent/AgentMessage.{h,cpp}; changing one side
// without the other breaks every device in the fleet.
type AgentMessage struct {
	Command string `json:"command"`

	// Payload is carried verbatim. Decoding it into map[string]any and
	// re-encoding would round integers through float64 and reorder keys, so
	// the caller's bytes are forwarded untouched.
	Payload json.RawMessage `json:"payload"`

	ReplyQueueURL string `json:"reply_queue_url,omitempty"`
}

// Commands the agent registers in its handler map.
const (
	CommandDownloadVersion = "download_version"
	CommandApplyVersion    = "apply_version"
	CommandCurrentStatus   = "current_status"
	CommandCleanOldVersion = "clean_old_version"
	CommandRollbackVersion = "rollback_version"
)

// supportedCommands is the single source of truth. It is never handed out
// directly so a caller cannot reorder or overwrite it.
var supportedCommands = []string{
	CommandDownloadVersion,
	CommandApplyVersion,
	CommandCurrentStatus,
	CommandCleanOldVersion,
	CommandRollbackVersion,
}

// SupportedCommands lists every command the agent has a handler for.
func SupportedCommands() []string { return slices.Clone(supportedCommands) }

// IsSupportedCommand reports whether publishing the command is worthwhile.
// An unregistered command reaches every device only to be logged and rejected.
func IsSupportedCommand(command string) bool { return slices.Contains(supportedCommands, command) }

// Artifact is one file of a release. The agent derives the S3 object key from
// release_id plus install_path, so the key never travels on the wire.
type Artifact struct {
	InstallPath string `json:"install_path"`
	SHA256      string `json:"sha256"`
	SizeBytes   uint64 `json:"size_bytes,omitempty"`
}

// DownloadPayload is the payload of download_version.
type DownloadPayload struct {
	ReleaseID string     `json:"release_id"`
	Artifacts []Artifact `json:"artifacts"`
}

// VersionPayload is the payload of apply_version and rollback_version.
type VersionPayload struct {
	ReleaseID string `json:"release_id"`
}

// Report is what an agent publishes to the result queue after handling a command.
// Mirrors AgentService::report in YirangAgent/AgentService.cpp.
type Report struct {
	DeviceID string `json:"device_id"`
	Group    string `json:"group"`
	Command  string `json:"command"`
	Success  bool   `json:"success"`
	Detail   string `json:"detail"`
}
