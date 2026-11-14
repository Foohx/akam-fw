//go:build race

package squashfs

// raceEnabled: the tests run under the race detector (go test -race), 5 to 10 times slower. This package is pure
// computation (decoders, encoder, tree walks) and starts no goroutine, so the detector has nothing to find in the
// heavy generated cases; they are thinned out (see the uses of raceEnabled) so that the -race run of the whole
// module stays within the CI time budget. Every case is still built, written, read back and verified in the normal
// run (tools/ci/local_check.sh --only go), and the structural checks all run in both modes.
const raceEnabled = true
