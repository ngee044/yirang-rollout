package models

import (
	"encoding/json"
	"slices"
)

type AgentMessage struct {
	Command string `json:"command"`

	Payload json.RawMessage `json:"payload"`

	ReplyQueueURL string `json:"reply_queue_url,omitempty"`
}

const (
	CommandDownloadVersion = "download_version"
	CommandApplyVersion    = "apply_version"
	CommandCurrentStatus   = "current_status"
	CommandCleanOldVersion = "clean_old_version"
	CommandRollbackVersion = "rollback_version"
)

var supportedCommands = []string{
	CommandDownloadVersion,
	CommandApplyVersion,
	CommandCurrentStatus,
	CommandCleanOldVersion,
	CommandRollbackVersion,
}

func SupportedCommands() []string { return slices.Clone(supportedCommands) }

func IsSupportedCommand(command string) bool { return slices.Contains(supportedCommands, command) }

type Artifact struct {
	InstallPath string `json:"install_path"`
	SHA256      string `json:"sha256"`
	SizeBytes   uint64 `json:"size_bytes,omitempty"`
}

type DownloadPayload struct {
	ReleaseID string     `json:"release_id"`
	Artifacts []Artifact `json:"artifacts"`
}

type VersionPayload struct {
	ReleaseID string `json:"release_id"`
}

type Report struct {
	DeviceID string `json:"device_id"`
	Group    string `json:"group"`
	Command  string `json:"command"`
	Success  bool   `json:"success"`
	Detail   string `json:"detail"`
}
