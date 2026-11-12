package guard

import (
	"strings"
	"testing"
)

func TestWaitRaisesAWorkerPanicInTheCaller(t *testing.T) {
	var g Group
	g.Go(func() {})
	g.Go(func() { panic("boom") })
	defer func() {
		p, ok := recover().(*Panic)
		if !ok || p.Value != "boom" {
			t.Fatalf("recovered %v", p)
		}
		if !strings.Contains(string(p.Stack), "guard.") || !strings.Contains(p.Error(), "boom") {
			t.Errorf("the stack does not show the worker:\n%s", p.Stack)
		}
	}()
	g.Wait()
	t.Fatal("Wait did not panic")
}

func TestWaitWithoutPanic(t *testing.T) {
	var g Group
	n := 0
	g.Go(func() { n++ })
	g.Wait()
	if n != 1 {
		t.Error("the worker did not run")
	}
}
