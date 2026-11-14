//go:build !akamtest

// Package testhook gates the AKAM_* variables that only tests may use. A release binary (built
// without the akamtest tag) ignores them all: nothing a user is told to "export" can swap the
// trust list (AKAM_ALLOWED_SIGNERS), the kit download address (AKAM_KIT_URL_BASE), the release list address (AKAM_API_URL_BASE), the scripts or
// flash_tool sent to the camera, the ports, the poll timings or the "this is a terminal" check.
package testhook

// Compiled is false in a release build.
const Compiled = false

// Env always returns "" in a release build.
func Env(name string) string { return "" }
