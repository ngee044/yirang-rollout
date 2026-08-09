package queue

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/service/sqs"
	"github.com/aws/aws-sdk-go-v2/service/sqs/types"
)

type fakeAPI struct {
	sendInput   *sqs.SendMessageInput
	sendErr     error
	received    []types.Message
	receiveIn   *sqs.ReceiveMessageInput
	deleteIn    *sqs.DeleteMessageBatchInput
	deleteFails []types.BatchResultErrorEntry
}

func (f *fakeAPI) SendMessage(_ context.Context, in *sqs.SendMessageInput, _ ...func(*sqs.Options)) (*sqs.SendMessageOutput, error) {
	f.sendInput = in
	if f.sendErr != nil {
		return nil, f.sendErr
	}
	return &sqs.SendMessageOutput{MessageId: aws.String("msg-1")}, nil
}

func (f *fakeAPI) ReceiveMessage(_ context.Context, in *sqs.ReceiveMessageInput, _ ...func(*sqs.Options)) (*sqs.ReceiveMessageOutput, error) {
	f.receiveIn = in
	return &sqs.ReceiveMessageOutput{Messages: f.received}, nil
}

func (f *fakeAPI) DeleteMessageBatch(_ context.Context, in *sqs.DeleteMessageBatchInput, _ ...func(*sqs.Options)) (*sqs.DeleteMessageBatchOutput, error) {
	f.deleteIn = in
	return &sqs.DeleteMessageBatchOutput{Failed: f.deleteFails}, nil
}

func TestSendPassesQueueURLAndBody(t *testing.T) {
	fake := &fakeAPI{}

	id, err := NewSQS(fake).Send(context.Background(), "https://sqs.test/q", `{"command":"current_status"}`)
	if err != nil {
		t.Fatalf("Send: %v", err)
	}

	if id != "msg-1" {
		t.Errorf("message id = %q", id)
	}
	if aws.ToString(fake.sendInput.QueueUrl) != "https://sqs.test/q" {
		t.Errorf("queue url = %q", aws.ToString(fake.sendInput.QueueUrl))
	}
	if aws.ToString(fake.sendInput.MessageBody) != `{"command":"current_status"}` {
		t.Errorf("body = %q", aws.ToString(fake.sendInput.MessageBody))
	}
}

func TestSendPropagatesFailure(t *testing.T) {
	fake := &fakeAPI{sendErr: errors.New("queue does not exist")}

	if _, err := NewSQS(fake).Send(context.Background(), "https://sqs.test/q", "{}"); err == nil {
		t.Fatal("expected the SQS failure to propagate")
	}
}

// Receive must not delete: a delete that failed halfway would drop everything
// already read.
func TestReceiveDoesNotAcknowledge(t *testing.T) {
	fake := &fakeAPI{received: []types.Message{
		{MessageId: aws.String("1"), Body: aws.String("a"), ReceiptHandle: aws.String("h1")},
		{MessageId: aws.String("2"), Body: aws.String("b"), ReceiptHandle: aws.String("h2")},
	}}

	messages, err := NewSQS(fake).Receive(context.Background(), "https://sqs.test/q", 10, 3)
	if err != nil {
		t.Fatalf("Receive: %v", err)
	}

	if len(messages) != 2 {
		t.Fatalf("received %d messages, want 2", len(messages))
	}
	if messages[0].ReceiptHandle != "h1" {
		t.Errorf("receipt handle = %q, it is required to acknowledge later", messages[0].ReceiptHandle)
	}
	if fake.deleteIn != nil {
		t.Error("Receive must not delete")
	}
	if fake.receiveIn.MaxNumberOfMessages != 10 || fake.receiveIn.WaitTimeSeconds != 3 {
		t.Errorf("receive parameters were not passed through: %+v", fake.receiveIn)
	}
}

// One batch call, not one call per message.
func TestDeleteBatchesEveryHandle(t *testing.T) {
	fake := &fakeAPI{}
	messages := []Message{{ReceiptHandle: "h1"}, {ReceiptHandle: "h2"}, {ReceiptHandle: "h3"}}

	if err := NewSQS(fake).Delete(context.Background(), "https://sqs.test/q", messages); err != nil {
		t.Fatalf("Delete: %v", err)
	}

	if len(fake.deleteIn.Entries) != 3 {
		t.Fatalf("batched %d entries, want 3", len(fake.deleteIn.Entries))
	}

	seen := map[string]bool{}
	for _, entry := range fake.deleteIn.Entries {
		if seen[aws.ToString(entry.Id)] {
			t.Fatalf("batch entry id %q is duplicated, SQS would reject the request", aws.ToString(entry.Id))
		}
		seen[aws.ToString(entry.Id)] = true
	}
}

func TestDeleteIsANoOpWhenThereIsNothingToAcknowledge(t *testing.T) {
	fake := &fakeAPI{}

	if err := NewSQS(fake).Delete(context.Background(), "https://sqs.test/q", nil); err != nil {
		t.Fatalf("Delete: %v", err)
	}
	if fake.deleteIn != nil {
		t.Error("an empty batch must not reach SQS")
	}
}

// A partial batch failure is reported, never silently dropped.
func TestDeleteReportsPartialFailure(t *testing.T) {
	fake := &fakeAPI{deleteFails: []types.BatchResultErrorEntry{
		{Id: aws.String("1"), Message: aws.String("ReceiptHandleIsInvalid")},
	}}

	err := NewSQS(fake).Delete(context.Background(), "https://sqs.test/q", []Message{{ReceiptHandle: "h1"}, {ReceiptHandle: "h2"}})
	if err == nil {
		t.Fatal("expected a failed delete entry to be reported")
	}
	if !strings.Contains(err.Error(), "ReceiptHandleIsInvalid") {
		t.Errorf("the reason should survive: %v", err)
	}
}
