package sdcard

import (
	"fmt"
	"sort"
)

// Inventory is the OS side of the guards: it reads the system and reports Facts, it never decides.
type Inventory interface {
	// Devices lists the device nodes worth inspecting (whole-disk candidates).
	Devices() []string
	// Inspect reads the facts of one device. An error means the device cannot be described at all.
	Inspect(dev string, ctx Context) (Facts, error)
}

// checkDevice is Inspect then Evaluate.
func checkDevice(inv Inventory, dev string, ctx Context) (*Candidate, error) {
	f, err := inv.Inspect(dev, ctx)
	if err != nil {
		return nil, err
	}
	return Evaluate(f, DefaultPolicy)
}

// listDevices returns every eligible disk, sorted.
func listDevices(inv Inventory, ctx Context) []Candidate {
	var out []Candidate
	for _, d := range inv.Devices() {
		if c, err := checkDevice(inv, d, ctx); err == nil {
			out = append(out, *c)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Dev < out[j].Dev })
	return out
}

// recheckDevice is the whole guard again, right before a destructive step: still eligible, same identity.
func recheckDevice(inv Inventory, dev, ident string, ctx Context) (*Candidate, error) {
	c, err := checkDevice(inv, dev, ctx)
	if err != nil {
		return nil, err
	}
	if c.Ident != ident {
		return nil, fmt.Errorf("%s changed since it was checked (was: %s, now: %s): %w", dev, ident, c.Ident, ErrIdentChanged)
	}
	return c, nil
}
