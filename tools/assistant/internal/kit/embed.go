package kit

import (
	_ "embed"
)

// DefaultAllowedSigners is a copy of tools/release/allowed_signers (a placeholder with no key
// today, so a signed kit is refused until the maintainer publishes one). A test fails when the two
// files differ.
//
//go:embed allowed_signers
var DefaultAllowedSigners []byte
