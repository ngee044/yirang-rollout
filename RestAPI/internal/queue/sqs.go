// Package queue is the repository layer: it turns SQS calls into plain values
// and holds no policy about what a message means.
package queue

import (
	"context"
	"fmt"
	"strconv"
	"strings"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/service/sqs"
	"github.com/aws/aws-sdk-go-v2/service/sqs/types"
)

// Message is one received message. ReceiptHandle is required to delete it and
// is valid only until the visibility timeout expires.
type Message struct {
	ID            string
	Body          string
	ReceiptHandle string
}

// API is the slice of the SQS client used here. Narrowing it keeps the fake in
// tests small and makes the dependency obvious.
type API interface {
	SendMessage(ctx context.Context, in *sqs.SendMessageInput, opts ...func(*sqs.Options)) (*sqs.SendMessageOutput, error)
	ReceiveMessage(ctx context.Context, in *sqs.ReceiveMessageInput, opts ...func(*sqs.Options)) (*sqs.ReceiveMessageOutput, error)
	DeleteMessageBatch(ctx context.Context, in *sqs.DeleteMessageBatchInput, opts ...func(*sqs.Options)) (*sqs.DeleteMessageBatchOutput, error)
}

// SQS implements sending and receiving against any SQS-compatible endpoint.
type SQS struct {
	client API
}

func NewSQS(client API) *SQS { return &SQS{client: client} }

// Send publishes one message. The queue URL is a parameter rather than state
// because a single request fans out to many device queues.
func (q *SQS) Send(ctx context.Context, queueURL, body string) (string, error) {
	out, err := q.client.SendMessage(ctx, &sqs.SendMessageInput{
		QueueUrl:    aws.String(queueURL),
		MessageBody: aws.String(body),
	})
	if err != nil {
		return "", err
	}
	return aws.ToString(out.MessageId), nil
}

// Receive returns up to max messages. It does not delete them: deleting here
// would lose every message already read if a later delete failed.
func (q *SQS) Receive(ctx context.Context, queueURL string, max int32, waitSeconds int32) ([]Message, error) {
	out, err := q.client.ReceiveMessage(ctx, &sqs.ReceiveMessageInput{
		QueueUrl:            aws.String(queueURL),
		MaxNumberOfMessages: max,
		WaitTimeSeconds:     waitSeconds,
	})
	if err != nil {
		return nil, err
	}

	messages := make([]Message, 0, len(out.Messages))
	for _, message := range out.Messages {
		messages = append(messages, Message{
			ID:            aws.ToString(message.MessageId),
			Body:          aws.ToString(message.Body),
			ReceiptHandle: aws.ToString(message.ReceiptHandle),
		})
	}
	return messages, nil
}

// Delete acknowledges messages in one batch call. SQS caps a batch at 10, which
// is also the cap on a single receive, so one batch always suffices here.
func (q *SQS) Delete(ctx context.Context, queueURL string, messages []Message) error {
	if len(messages) == 0 {
		return nil
	}

	entries := make([]types.DeleteMessageBatchRequestEntry, 0, len(messages))
	for index, message := range messages {
		entries = append(entries, types.DeleteMessageBatchRequestEntry{
			// Batch-local identifier; only has to be unique within this request.
			Id:            aws.String(strconv.Itoa(index)),
			ReceiptHandle: aws.String(message.ReceiptHandle),
		})
	}

	out, err := q.client.DeleteMessageBatch(ctx, &sqs.DeleteMessageBatchInput{
		QueueUrl: aws.String(queueURL),
		Entries:  entries,
	})
	if err != nil {
		return err
	}

	if len(out.Failed) == 0 {
		return nil
	}

	reasons := make([]string, 0, len(out.Failed))
	for _, failure := range out.Failed {
		reasons = append(reasons, fmt.Sprintf("%s: %s", aws.ToString(failure.Id), aws.ToString(failure.Message)))
	}
	return fmt.Errorf("%d of %d deletes failed (%s)", len(out.Failed), len(entries), strings.Join(reasons, "; "))
}
