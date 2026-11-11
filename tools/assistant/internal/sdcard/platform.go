package sdcard

import "io"

// platform is everything the card layer needs from the running OS. newPlatform (one file per OS) builds it;
// a build for an OS whose disk layer is not written yet gets stubs that say so.
type platform struct {
	Inventory Inventory
	Opener    Opener
	Transport Transport
}

// Transport is how a request reaches code that has the rights to write the disk.
type Transport interface {
	// NeedsElevation reports whether writing c needs administrator rights.
	NeedsElevation(c *Candidate) bool
	// RunElevated runs `self sd-helper` with the rights, sends the request and returns the answer. elevate is
	// the user's choice of tool ("" = automatic); the child's stderr goes to stderr.
	RunElevated(self, elevate string, req Request, stderr io.Writer) (Response, error)
}
