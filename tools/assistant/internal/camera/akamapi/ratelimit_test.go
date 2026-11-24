package akamapi

import (
	"context"
	"errors"
	"testing"
	"time"
)

// limited is a client whose /hello answers 429 helloN times (with Retry-After retry seconds), then 200; the signed
// request answers per answers (the last one repeats).
func limited(helloN, retry int, answers ...int) (c *Client, hellos, signed *int, slept *[]time.Duration) {
	hellos, signed, slept = new(int), new(int), new([]time.Duration)
	c = settingsClient(func(m, p string, body []byte, h Header) (int, string) {
		i := *signed
		*signed++
		if i >= len(answers) {
			i = len(answers) - 1
		}
		if answers[i] == 429 {
			return 429, `{"error":"rate","detail":"request rate limit","retry_after_s":2}`
		}
		return answers[i], `{"a":"1","results":{"a":{"ok":true,"apply":"applied","value":"1"}}}`
	})
	inner := c.Do
	c.Do = func(ctx context.Context, hp, path string, h Header) (int, []byte, error) {
		if path == "/akam/v1/hello" {
			*hellos++
			if *hellos <= helloN {
				return reply(429, `{"error":"rate","detail":"too many hello requests","retry_after_s":`+string(rune('0'+retry))+`}`)
			}
		}
		return inner(ctx, hp, path, h)
	}
	c.Sleep = func(_ context.Context, d time.Duration) { *slept = append(*slept, d) }
	return c, hellos, signed, slept
}

func TestReadWaitsOutAHelloRateLimitHonouringRetryAfter(t *testing.T) {
	c, hellos, _, slept := limited(2, 3, 200)
	if _, err := c.Settings(context.Background()); err != nil {
		t.Fatal(err)
	}
	if *hellos != 3 || len(*slept) != 2 || (*slept)[0] != 3*time.Second {
		t.Errorf("hellos %d, waits %v", *hellos, *slept)
	}
}

func TestRetryAfterIsBounded(t *testing.T) {
	c, hellos, _, slept := limited(100, 9, 200) // always limited, Retry-After 9 s
	_, err := c.Settings(context.Background())
	var ae *APIError
	if !errors.As(err, &ae) || ae.Status != 429 {
		t.Fatalf("%v", err)
	}
	if *hellos != rateTries {
		t.Errorf("%d hellos: not bounded", *hellos)
	}
	for _, d := range *slept {
		if d > rateMaxWait || d < rateMinWait {
			t.Errorf("wait %v out of bounds", d)
		}
	}
}

func TestReadRetriesA429OfTheSignedRequest(t *testing.T) {
	c, _, signed, slept := limited(0, 1, 429, 429, 200)
	if _, err := c.State(context.Background()); err != nil {
		t.Fatal(err)
	}
	if *signed != 3 || len(*slept) != 2 || (*slept)[0] != 2*time.Second {
		t.Errorf("signed %d, waits %v", *signed, *slept)
	}
}

// A write is never repeated: a 429 of the PATCH itself comes back at once; only its /hello (nothing sent yet) may be retried.
func TestWriteIsNeverRepeatedButItsHelloIs(t *testing.T) {
	c, _, signed, slept := limited(0, 1, 429, 200)
	_, err := c.Patch(context.Background(), map[string]string{"a": "1"})
	var ae *APIError
	if !errors.As(err, &ae) || ae.Status != 429 || *signed != 1 || len(*slept) != 0 {
		t.Fatalf("err %v, signed %d, waits %v", err, *signed, *slept)
	}
	c, hellos, signed, slept := limited(1, 1, 200)
	res, err := c.Patch(context.Background(), map[string]string{"a": "1"})
	if err != nil || !res.Results["a"].OK || *signed != 1 || *hellos != 2 || len(*slept) != 1 {
		t.Fatalf("%+v %v signed %d hellos %d waits %v", res, err, *signed, *hellos, *slept)
	}
}

func TestRetryAfterHeaderBecomesAField(t *testing.T) {
	got := addRetryAfter("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 7\r\nContent-Length: 20", `{"error":"rate"}`)
	if apiError(429, []byte(got)).RetryAfter != 7*time.Second || apiError(429, []byte(got)).Code != "rate" {
		t.Errorf("%s", got)
	}
	if got := addRetryAfter("HTTP/1.1 429 X\r\nRetry-After: soon", `{"error":"rate"}`); got != `{"error":"rate"}` {
		t.Errorf("%s", got)
	}
}
