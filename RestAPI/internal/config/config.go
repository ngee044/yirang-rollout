// Package config loads and validates the server's environment. Every value the
// server needs is resolved here so no other package reads os.Getenv.
package config

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/netip"
	"os"
	"strconv"
	"strings"
	"time"
)

// DeviceQueue routes commands to one device. The API owns this registry, which
// is the reason the CLI never has to know a queue URL.
type DeviceQueue struct {
	Name  string `json:"name"`
	URL   string `json:"url"`
	Group string `json:"group"`
}

type Config struct {
	// BindAddress defaults to loopback. The API is unauthenticated, so binding
	// it to every interface would let anyone on the same network command the
	// whole fleet; that has to be an explicit choice, never a default.
	BindAddress string

	Port            int
	ReadTimeout     time.Duration
	WriteTimeout    time.Duration
	RequestTimeout  time.Duration
	ShutdownTimeout time.Duration

	AWSRegion   string
	AWSEndpoint string

	DeviceQueues   []DeviceQueue
	ResultQueueURL string

	// ResultBatchSize caps one drain of the result queue.
	ResultBatchSize int

	// ResultWaitSeconds is the SQS long-poll window. It is short because a
	// client is blocked on the HTTP request while it elapses.
	ResultWaitSeconds int32
}

// Load reads the environment and refuses a configuration the server cannot run
// with. It reports every problem as a cause, never as a silent default.
func Load() (*Config, error) {
	port, err := envInt("PORT", 8080)
	if err != nil {
		return nil, err
	}
	readTimeout, err := envSeconds("READ_TIMEOUT_SECONDS", 15)
	if err != nil {
		return nil, err
	}
	writeTimeout, err := envSeconds("WRITE_TIMEOUT_SECONDS", 30)
	if err != nil {
		return nil, err
	}
	requestTimeout, err := envSeconds("REQUEST_TIMEOUT_SECONDS", 20)
	if err != nil {
		return nil, err
	}
	shutdownTimeout, err := envSeconds("SHUTDOWN_TIMEOUT_SECONDS", 10)
	if err != nil {
		return nil, err
	}
	batchSize, err := envInt("RESULT_BATCH_SIZE", 10)
	if err != nil {
		return nil, err
	}
	waitSeconds, err := envInt("RESULT_WAIT_SECONDS", 1)
	if err != nil {
		return nil, err
	}
	queues, err := parseDeviceQueues(os.Getenv("DEVICE_QUEUES"))
	if err != nil {
		return nil, err
	}

	cfg := &Config{
		BindAddress:       envString("BIND_ADDRESS", "127.0.0.1"),
		Port:              port,
		ReadTimeout:       readTimeout,
		WriteTimeout:      writeTimeout,
		RequestTimeout:    requestTimeout,
		ShutdownTimeout:   shutdownTimeout,
		AWSRegion:         envString("AWS_REGION", "us-east-1"),
		AWSEndpoint:       os.Getenv("AWS_ENDPOINT_URL"),
		DeviceQueues:      queues,
		ResultQueueURL:    os.Getenv("RESULT_QUEUE_URL"),
		ResultBatchSize:   batchSize,
		ResultWaitSeconds: int32(waitSeconds),
	}

	if err := cfg.Validate(); err != nil {
		return nil, err
	}
	return cfg, nil
}

// Validate rejects a configuration that would publish nothing, publish twice to
// one device, or answer ambiguously.
func (c *Config) Validate() error {
	if c.Port < 1 || c.Port > 65535 {
		return fmt.Errorf("PORT must be within 1-65535, got %d", c.Port)
	}
	if c.BindAddress == "" {
		return errors.New(`BIND_ADDRESS must not be empty: use "127.0.0.1" for local-only or "0.0.0.0" to expose deliberately`)
	}
	if len(c.DeviceQueues) == 0 {
		return errors.New("DEVICE_QUEUES is required: the API has no queue to publish to")
	}

	names := make(map[string]struct{}, len(c.DeviceQueues))
	urls := make(map[string]struct{}, len(c.DeviceQueues))
	for index, queue := range c.DeviceQueues {
		if queue.Name == "" {
			return fmt.Errorf("DEVICE_QUEUES[%d].name is required", index)
		}
		if queue.URL == "" {
			return fmt.Errorf("DEVICE_QUEUES[%d].url is required", index)
		}
		if _, duplicated := names[queue.Name]; duplicated {
			return fmt.Errorf("DEVICE_QUEUES[%d].name %q is duplicated: a delivery report would be ambiguous", index, queue.Name)
		}
		// The same URL listed twice publishes one command to that device twice.
		if _, duplicated := urls[queue.URL]; duplicated {
			return fmt.Errorf("DEVICE_QUEUES[%d].url %q is duplicated", index, queue.URL)
		}
		names[queue.Name] = struct{}{}
		urls[queue.URL] = struct{}{}
	}

	// Draining results from a device queue would delete commands the device has
	// not fetched yet, and a command envelope decodes into a Report without error.
	if c.ResultQueueURL != "" {
		if _, clash := urls[c.ResultQueueURL]; clash {
			return fmt.Errorf("RESULT_QUEUE_URL %q is also a device queue: draining results would delete pending commands", c.ResultQueueURL)
		}
	}

	// SQS caps both a receive and a delete batch at 10.
	if c.ResultBatchSize < 1 || c.ResultBatchSize > 10 {
		return fmt.Errorf("RESULT_BATCH_SIZE must be within 1-10, got %d", c.ResultBatchSize)
	}
	if c.ResultWaitSeconds < 0 || c.ResultWaitSeconds > 20 {
		return fmt.Errorf("RESULT_WAIT_SECONDS must be within 0-20, got %d", c.ResultWaitSeconds)
	}
	return nil
}

// QueuesForGroup returns every queue in the group. An empty group targets all
// devices, which is how a broadcast is expressed.
func (c *Config) QueuesForGroup(group string) []DeviceQueue {
	if group == "" {
		return c.DeviceQueues
	}

	matched := make([]DeviceQueue, 0, len(c.DeviceQueues))
	for _, queue := range c.DeviceQueues {
		if queue.Group == group {
			matched = append(matched, queue)
		}
	}
	return matched
}

func parseDeviceQueues(raw string) ([]DeviceQueue, error) {
	if raw == "" {
		return nil, nil
	}

	var queues []DeviceQueue
	decoder := json.NewDecoder(strings.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&queues); err != nil {
		return nil, fmt.Errorf("DEVICE_QUEUES must be a JSON array of {name,url,group}: %w", err)
	}
	return queues, nil
}

func envString(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

// envInt fails loudly on a malformed value. Falling back to the default would
// hide a typo until the wrong port or batch size showed up in production.
func envInt(key string, fallback int) (int, error) {
	raw := os.Getenv(key)
	if raw == "" {
		return fallback, nil
	}

	value, err := strconv.Atoi(raw)
	if err != nil {
		return 0, fmt.Errorf("%s must be an integer, got %q", key, raw)
	}
	return value, nil
}

func envSeconds(key string, fallback int) (time.Duration, error) {
	value, err := envInt(key, fallback)
	if err != nil {
		return 0, err
	}
	if value < 1 {
		return 0, fmt.Errorf("%s must be at least 1 second, got %d", key, value)
	}
	return time.Duration(value) * time.Second, nil
}

// IsLoopback reports whether the listener is reachable only from this machine.
// It is what makes "no authentication is fine here" true rather than assumed.
func (c *Config) IsLoopback() bool {
	address, err := netip.ParseAddr(c.BindAddress)
	if err != nil {
		// A hostname rather than a literal. Only "localhost" is safe to assume.
		return strings.EqualFold(c.BindAddress, "localhost")
	}
	return address.IsLoopback()
}
