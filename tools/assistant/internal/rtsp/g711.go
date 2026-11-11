// Package rtsp is the talk client: RTSP with Digest login, the ONVIF audio backchannel, G.711 and
// WAV input. It is a port of tools/akam-talk/akam_talk.c (same requests, same RTP framing, same
// exit codes) and reuses nothing from the camera code.
package rtsp

// G.711 mu-law and A-law, the segment/mantissa algorithm of src/ak_rtsp/g711.c (bit exact with
// the Sun reference; the vectors of tests/native/g711_test.c are in the Go tests).

const (
	ulawBias = 0x84
	ulawClip = 32635
)

// UlawDecode decodes one mu-law byte.
func UlawDecode(code byte) int16 {
	u := int(^code) & 0xFF
	t := (((u & 0x0F) << 3) + ulawBias) << ((u & 0x70) >> 4)
	if u&0x80 != 0 {
		return int16(ulawBias - t)
	}
	return int16(t - ulawBias)
}

// AlawDecode decodes one A-law byte.
func AlawDecode(code byte) int16 {
	a := int(code^0x55) & 0xFF
	t := (a & 0x0F) << 4
	seg := (a & 0x70) >> 4
	if seg == 0 {
		t += 8
	} else {
		t = (t + 0x108) << (seg - 1)
	}
	if a&0x80 != 0 {
		return int16(t)
	}
	return int16(-t)
}

// UlawEncode encodes one 16-bit sample as mu-law.
func UlawEncode(pcm int16) byte {
	v := int(pcm)
	sign := 0
	if v < 0 {
		v = -v
		sign = 0x80
	}
	if v > ulawClip {
		v = ulawClip
	}
	v += ulawBias
	exp := 7
	for exp > 0 && v&(0x80<<exp) == 0 {
		exp--
	}
	mant := (v >> (exp + 3)) & 0x0F
	return ^byte(sign | exp<<4 | mant)
}

// AlawEncode encodes one 16-bit sample as A-law.
func AlawEncode(pcm int16) byte {
	v := int(pcm) >> 3
	var mask int
	if v >= 0 {
		mask = 0xD5
	} else {
		mask = 0x55
		v = -v - 1
	}
	if v > 0xFFF {
		v = 0xFFF
	}
	seg := 0
	for seg < 7 && v > (0x20<<seg)-1 {
		seg++
	}
	out := seg << 4
	if seg < 2 {
		out |= (v >> 1) & 0x0F
	} else {
		out |= (v >> seg) & 0x0F
	}
	return byte(out ^ mask)
}
