package appfs

import (
	"strings"
	"testing"
)

// SEC5: a development kit is refused unless the operator chose a local folder (AllowTestGrade).
func TestKitGradeMustBeRelease(t *testing.T) {
	for _, grade := range []string{"test", "", "nightly"} {
		f := kitFiles()
		man := strings.Replace(string(f[pathManifest]), "kit_grade=release\n", "", 1)
		if grade != "" {
			man += "kit_grade=" + grade + "\n"
		}
		f[pathManifest] = []byte(man)
		if _, err := NewKit(sealKit(f)); err == nil || !strings.Contains(err.Error(), "kit_grade") {
			t.Errorf("grade %q accepted: %v", grade, err)
		}
	}
	f := kitFiles()
	f[pathManifest] = []byte(strings.Replace(string(f[pathManifest]), "kit_grade=release", "kit_grade=test", 1))
	k, err := NewKit(sealKit(f), AllowTestGrade())
	if err != nil || !k.IsTestGrade() {
		t.Fatalf("an explicitly allowed test kit: %v", err)
	}
	if k, err := NewKit(kitFiles()); err != nil || k.IsTestGrade() {
		t.Fatalf("a release kit: %v", err)
	}
}

// SEC5: the card boots the kit's busybox, so the pin is checked for the card as it is for the image.
func TestBuildCardRefusesAnUnpinnedBusybox(t *testing.T) {
	k := cardKit(t)
	old := busyBoxPin
	busyBoxPin = strings.Repeat("0", 64)
	t.Cleanup(func() { busyBoxPin = old })
	_, err := BuildCard(k, CardInput{PubKey: []byte(testPub), SSID: "home", Pass: "correct horse"})
	if err == nil || !strings.Contains(err.Error(), "not the pinned") {
		t.Fatalf("a card was built with a busybox that is not the pinned one: %v", err)
	}
}
