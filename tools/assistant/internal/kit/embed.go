package kit

import (
	_ "embed"
)

// DefaultAllowedSigners is a copy of tools/release/allowed_signers (the public key of the Akam
// release signing key). A test fails when the two files differ.
//
//go:embed allowed_signers
var DefaultAllowedSigners []byte
