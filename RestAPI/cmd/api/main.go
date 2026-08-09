// Command api is the yirang-rollout deployment REST API: it accepts a release
// from the CLI and fans the matching command out to every device queue.
package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"syscall"

	awsconfig "github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/service/sqs"

	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/config"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/queue"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/rest"
	"github.com/hyunkyu/yirang-rollout/RestAPI/internal/service"
)

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: logLevel()}))

	if err := run(logger); err != nil {
		logger.Error("fatal", "error", err)
		os.Exit(1)
	}

	logger.Info("stopped")
}

func run(logger *slog.Logger) error {
	cfg, err := config.Load()
	if err != nil {
		return fmt.Errorf("load configuration: %w", err)
	}

	// The signal context is cancelled on the first SIGINT/SIGTERM; a second one
	// is left to the default handler so a stuck shutdown can still be killed.
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	sqsClient, err := newSQSClient(ctx, cfg)
	if err != nil {
		return fmt.Errorf("create sqs client: %w", err)
	}

	queues := queue.NewSQS(sqsClient)
	deployService := service.New(cfg, queues, queues, logger)

	server := &http.Server{
		Addr:         net.JoinHostPort(cfg.BindAddress, strconv.Itoa(cfg.Port)),
		Handler:      rest.NewHandler(cfg, deployService, logger),
		ReadTimeout:  cfg.ReadTimeout,
		WriteTimeout: cfg.WriteTimeout,
		ErrorLog:     slog.NewLogLogger(logger.Handler(), slog.LevelWarn),
	}

	// Bind before announcing, so "listening" is never logged for a port that
	// was already taken.
	listener, err := net.Listen("tcp", server.Addr)
	if err != nil {
		return fmt.Errorf("listen on %s: %w", server.Addr, err)
	}

	serveErrors := make(chan error, 1)
	go func() {
		if err := server.Serve(listener); err != nil && !errors.Is(err, http.ErrServerClosed) {
			serveErrors <- err
		}
	}()

	logger.Info("listening",
		"address", listener.Addr().String(),
		"device_queues", len(cfg.DeviceQueues),
		"result_queue_set", cfg.ResultQueueURL != "",
	)

	// The API has no authentication. On loopback that is fine — anyone who can
	// reach it can already read the AWS credentials it would be protecting.
	// Off loopback it is not, and the operator has to be told plainly.
	if !cfg.IsLoopback() {
		logger.Warn("listening beyond loopback WITHOUT authentication",
			"bind_address", cfg.BindAddress,
			"risk", "anyone who can reach this port can command every registered device",
			"fix", "set BIND_ADDRESS=127.0.0.1, or publish the container port as 127.0.0.1:8080:8080",
		)
	}

	select {
	case err := <-serveErrors:
		return err
	case <-ctx.Done():
		stop()
		logger.Info("shutdown requested")
	}

	// In-flight requests get a bounded window; the timeout is separate from the
	// signal context, which is already cancelled by this point.
	shutdownCtx, cancel := context.WithTimeout(context.Background(), cfg.ShutdownTimeout)
	defer cancel()

	if err := server.Shutdown(shutdownCtx); err != nil {
		return fmt.Errorf("graceful shutdown: %w", err)
	}
	return nil
}

func newSQSClient(ctx context.Context, cfg *config.Config) (*sqs.Client, error) {
	awsCfg, err := awsconfig.LoadDefaultConfig(ctx, awsconfig.WithRegion(cfg.AWSRegion))
	if err != nil {
		return nil, err
	}

	if cfg.AWSEndpoint == "" {
		return sqs.NewFromConfig(awsCfg), nil
	}

	// A custom endpoint means LocalStack or another SQS-compatible service.
	return sqs.NewFromConfig(awsCfg, func(o *sqs.Options) { o.BaseEndpoint = &cfg.AWSEndpoint }), nil
}

// logLevel is read straight from the environment because logging is set up
// before the configuration is loaded, so a load failure is still logged.
func logLevel() slog.Level {
	var level slog.Level
	if err := level.UnmarshalText([]byte(os.Getenv("LOG_LEVEL"))); err != nil {
		return slog.LevelInfo
	}
	return level
}
