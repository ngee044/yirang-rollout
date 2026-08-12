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

type Message struct {
	ID            string
	Body          string
	ReceiptHandle string
}

type API interface {
	SendMessage(ctx context.Context, in *sqs.SendMessageInput, opts ...func(*sqs.Options)) (*sqs.SendMessageOutput, error)
	ReceiveMessage(ctx context.Context, in *sqs.ReceiveMessageInput, opts ...func(*sqs.Options)) (*sqs.ReceiveMessageOutput, error)
	DeleteMessageBatch(ctx context.Context, in *sqs.DeleteMessageBatchInput, opts ...func(*sqs.Options)) (*sqs.DeleteMessageBatchOutput, error)
}

type SQS struct {
	client API
}

func NewSQS(client API) *SQS { return &SQS{client: client} }

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

func (q *SQS) Delete(ctx context.Context, queueURL string, messages []Message) error {
	if len(messages) == 0 {
		return nil
	}

	entries := make([]types.DeleteMessageBatchRequestEntry, 0, len(messages))
	for index, message := range messages {
		entries = append(entries, types.DeleteMessageBatchRequestEntry{
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
