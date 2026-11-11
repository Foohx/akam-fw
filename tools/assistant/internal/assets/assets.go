// Package assets holds the camera-side scripts the assistant uploads (to the camera's RAM or to the
// flash card), embedded in the binary so that it works from any directory, without a checkout: a
// folder found on disk is never trusted to provide what is sent to a camera. The copies in scripts/
// are made from their source paths (sources.txt) by `go generate`; a test fails if they drift.
// flash_tool is an ARM binary built from src/ak_rtsp: it is only taken from the verified kit.
package assets

import (
	"embed"
	"fmt"
)

//go:generate bash sync.sh

//go:embed scripts/*
var scripts embed.FS

// Script returns an embedded script by file name (for example "preflight_check.sh").
func Script(name string) ([]byte, error) {
	b, err := scripts.ReadFile("scripts/" + name)
	if err != nil {
		return nil, fmt.Errorf("no embedded script %q", name)
	}
	return b, nil
}

// Names lists the embedded files.
func Names() []string {
	es, _ := scripts.ReadDir("scripts")
	var out []string
	for _, e := range es {
		out = append(out, e.Name())
	}
	return out
}
