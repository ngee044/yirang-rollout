package config

import (
	"strings"
	"testing"
)

const oneQueue = `[{"name":"pc-001","url":"https://sqs.test/pc-001","group":"kiosk"}]`

func TestLoadUsesDefaults(t *testing.T) {
	t.Setenv("DEVICE_QUEUES", oneQueue)

	cfg, err := Load()
	if err != nil {
		t.Fatalf("Load: %v", err)
	}

	if cfg.Port != 8080 {
		t.Errorf("Port = %d, want 8080", cfg.Port)
	}
	if cfg.ResultBatchSize != 10 {
		t.Errorf("ResultBatchSize = %d, want 10", cfg.ResultBatchSize)
	}
	if cfg.AWSRegion != "us-east-1" {
		t.Errorf("AWSRegion = %q, want us-east-1", cfg.AWSRegion)
	}
	if cfg.RequestTimeout <= 0 {
		t.Errorf("RequestTimeout = %v, want a positive budget", cfg.RequestTimeout)
	}
	// The API is unauthenticated, so exposure must be opted into, not defaulted.
	if cfg.BindAddress != "127.0.0.1" || !cfg.IsLoopback() {
		t.Errorf("BindAddress = %q, want loopback by default", cfg.BindAddress)
	}
}

func TestIsLoopback(t *testing.T) {
	tests := map[string]bool{
		"127.0.0.1":        true,
		"127.0.0.53":       true,
		"::1":              true,
		"localhost":        true,
		"LocalHost":        true,
		"0.0.0.0":          false,
		"192.168.0.10":     false,
		"::":               false,
		"example.internal": false,
	}

	for address, wantLoopback := range tests {
		t.Run(address, func(t *testing.T) {
			cfg := &Config{BindAddress: address}
			if got := cfg.IsLoopback(); got != wantLoopback {
				t.Errorf("IsLoopback(%q) = %v, want %v", address, got, wantLoopback)
			}
		})
	}
}

func TestValidateRejectsEmptyBindAddress(t *testing.T) {
	cfg := &Config{Port: 8080, ResultBatchSize: 10, DeviceQueues: []DeviceQueue{{Name: "pc", URL: "u"}}}

	if err := cfg.Validate(); err == nil {
		t.Fatal("expected Validate to reject an empty BIND_ADDRESS")
	}
}

// Without a queue the server would accept requests and publish nothing.
func TestLoadRequiresDeviceQueues(t *testing.T) {
	t.Setenv("DEVICE_QUEUES", "")

	if _, err := Load(); err == nil {
		t.Fatal("expected Load to fail without DEVICE_QUEUES")
	}
}

// A typo in a numeric variable must not be swallowed by the default.
func TestLoadRejectsMalformedInteger(t *testing.T) {
	t.Setenv("DEVICE_QUEUES", oneQueue)
	t.Setenv("PORT", "8O80")

	_, err := Load()
	if err == nil {
		t.Fatal("expected Load to fail on a non-numeric PORT")
	}
	if !strings.Contains(err.Error(), "PORT") {
		t.Errorf("error should name PORT, got %v", err)
	}
}

func TestLoadRejectsUnknownQueueField(t *testing.T) {
	t.Setenv("DEVICE_QUEUES", `[{"name":"pc-001","url":"https://sqs.test/pc-001","grp":"kiosk"}]`)

	if _, err := Load(); err == nil {
		t.Fatal("expected Load to reject a misspelled queue field")
	}
}

func TestValidateRejectsDuplicates(t *testing.T) {
	tests := map[string]*Config{
		"duplicate name": {Port: 8080, BindAddress: "127.0.0.1", ResultBatchSize: 10, DeviceQueues: []DeviceQueue{
			{Name: "pc", URL: "https://sqs.test/a"},
			{Name: "pc", URL: "https://sqs.test/b"},
		}},
		"duplicate url": {Port: 8080, BindAddress: "127.0.0.1", ResultBatchSize: 10, DeviceQueues: []DeviceQueue{
			{Name: "pc-001", URL: "https://sqs.test/a"},
			{Name: "pc-002", URL: "https://sqs.test/a"},
		}},
	}

	for name, cfg := range tests {
		t.Run(name, func(t *testing.T) {
			if err := cfg.Validate(); err == nil {
				t.Fatalf("expected Validate to reject a %s", name)
			}
		})
	}
}

// SQS caps a receive and a delete batch at 10.
func TestValidateBoundsBatchSize(t *testing.T) {
	cfg := &Config{Port: 8080, BindAddress: "127.0.0.1", ResultBatchSize: 11, DeviceQueues: []DeviceQueue{{Name: "pc", URL: "u"}}}

	if err := cfg.Validate(); err == nil {
		t.Fatal("expected Validate to reject a batch size above 10")
	}
}

func TestQueuesForGroup(t *testing.T) {
	cfg := &Config{DeviceQueues: []DeviceQueue{
		{Name: "kiosk-1", URL: "u1", Group: "kiosk"},
		{Name: "kiosk-2", URL: "u2", Group: "kiosk"},
		{Name: "sign-1", URL: "u3", Group: "signage"},
	}}

	if got := len(cfg.QueuesForGroup("kiosk")); got != 2 {
		t.Errorf("kiosk group = %d queues, want 2", got)
	}
	if got := len(cfg.QueuesForGroup("signage")); got != 1 {
		t.Errorf("signage group = %d queues, want 1", got)
	}
	// An empty group is how a broadcast is expressed.
	if got := len(cfg.QueuesForGroup("")); got != 3 {
		t.Errorf("empty group = %d queues, want all 3", got)
	}
	if got := len(cfg.QueuesForGroup("nope")); got != 0 {
		t.Errorf("unknown group = %d queues, want 0", got)
	}
}

// 결과 큐가 디바이스 큐와 같으면 결과 조회가 미수신 명령을 삭제한다.
func TestValidateRejectsResultQueueThatIsAlsoADeviceQueue(t *testing.T) {
	cfg := &Config{
		Port: 8080, BindAddress: "127.0.0.1", ResultBatchSize: 10,
		DeviceQueues:   []DeviceQueue{{Name: "pc-001", URL: "https://sqs.test/pc-001"}},
		ResultQueueURL: "https://sqs.test/pc-001",
	}

	err := cfg.Validate()
	if err == nil {
		t.Fatal("expected Validate to reject a result queue that is also a device queue")
	}
	if !strings.Contains(err.Error(), "RESULT_QUEUE_URL") {
		t.Errorf("error should name RESULT_QUEUE_URL, got %v", err)
	}
}
