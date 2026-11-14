// Package guard carries a panic of a helper goroutine back to the goroutine that waits for it.
//
// A panic in a goroutine nobody recovers kills the process at once, with the full-screen interface
// still on the terminal (raw mode, alternate screen). The flow goroutine of a session is protected:
// the session restores the terminal and raises the panic again. A Group makes the workers of a flow
// (probe and scan workers) end up there: the worker recovers, remembers the value and the stack, and
// Wait panics in the caller's goroutine.
package guard

import (
	"fmt"
	"runtime/debug"
	"sync"
)

// Panic is the value Wait panics with: what the worker panicked with and the stack of the worker.
type Panic struct {
	Value any
	Stack []byte
}

func (p *Panic) Error() string {
	return fmt.Sprintf("%v\n\ngoroutine stack of the worker:\n%s", p.Value, p.Stack)
}

// Group is a sync.WaitGroup whose goroutines may panic.
type Group struct {
	wg    sync.WaitGroup
	mu    sync.Mutex
	first *Panic
}

// Go runs f in a goroutine. A panic of f is kept (the first one) and raised again by Wait.
func (g *Group) Go(f func()) {
	g.wg.Add(1)
	go func() {
		defer g.wg.Done()
		defer func() {
			if r := recover(); r != nil {
				p := &Panic{Value: r, Stack: debug.Stack()}
				g.mu.Lock()
				if g.first == nil {
					g.first = p
				}
				g.mu.Unlock()
			}
		}()
		f()
	}()
}

// Wait waits for every goroutine of the group, then panics with the first *Panic, if any.
func (g *Group) Wait() {
	g.wg.Wait()
	g.mu.Lock()
	p := g.first
	g.mu.Unlock()
	if p != nil {
		panic(p)
	}
}
