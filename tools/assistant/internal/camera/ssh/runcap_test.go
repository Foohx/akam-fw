package ssh_test

import (
	"context"
	"testing"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
)

// Run keeps at most 1 MiB of stdout: a camera that sends a 5 MiB stream cannot fill the memory
// of the assistant (the command still ends normally).
func TestRunKeepsOnlyTheFirstMegabyte(t *testing.T) {
	f := newFixture(t)
	c, err := camssh.Dial(f.cfg(yes))
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	res, err := c.Run(context.Background(), readCmd, nil) // partition 7 of the fake camera is 5 MiB
	if err != nil || res.Status != 0 {
		t.Fatalf("%+v %v", res.Status, err)
	}
	if len(res.Stdout) != 1<<20 {
		t.Errorf("kept %d bytes, want %d", len(res.Stdout), 1<<20)
	}
}
