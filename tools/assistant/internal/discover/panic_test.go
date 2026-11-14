package discover

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/guard"
)

// A panic in a probe goroutine is raised in the goroutine that called the scan (the flow of the
// session, which restores the terminal), not in a goroutine nobody can recover.
func TestProbePanicReachesTheCaller(t *testing.T) {
	defer func() {
		if _, ok := recover().(*guard.Panic); !ok {
			t.Error("the panic of a probe did not reach the caller as a guard.Panic")
		}
	}()
	dial := func(context.Context, string, string) (net.Conn, error) { panic("probe bug") }
	OpenPorts(context.Background(), "192.0.2.1", []int{22, 554}, time.Second, dial)
	t.Error("OpenPorts returned")
}
