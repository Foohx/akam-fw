package audio

import (
	"strings"
	"testing"
)

// Ogg Opus (and any non-Vorbis Ogg) is not built in: the error says so, so the caller can offer ffmpeg.
func TestDecodeOggOpusIsRefusedClearly(t *testing.T) {
	_, err := Decode(fixture(t, "sine_opus.opus"))
	if err == nil || !strings.Contains(err.Error(), "only Vorbis audio is built in") {
		t.Errorf("%v", err)
	}
}
