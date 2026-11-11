//go:build akamtest

package testhook

import "os"

// Compiled is true in a test build.
const Compiled = true

// Env returns the value of the test hook variable name.
func Env(name string) string { return os.Getenv(name) }
