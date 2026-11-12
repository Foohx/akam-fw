package childenv

import (
	"os"
	"os/exec"
	"strings"
	"testing"
)

func TestFilterAndTake(t *testing.T) {
	t.Setenv("AKAM_RTSP_PASSWORD", "s3cret-one")
	t.Setenv("AKAM_WIFI_PASS", "s3cret-two")
	t.Setenv("AKAM_SD_PASS", "s3cret-three")
	t.Setenv("AKAM_PLAIN", "1")
	found := false
	for _, kv := range Env() {
		if strings.Contains(kv, "s3cret") {
			t.Fatalf("Env leaks %q", kv)
		}
		if kv == "AKAM_PLAIN=1" {
			found = true
		}
	}
	if !found {
		t.Error("Env dropped a non-secret variable")
	}
	got := Take()
	if got["AKAM_RTSP_PASSWORD"] != "s3cret-one" || got["AKAM_WIFI_PASS"] != "s3cret-two" || got["AKAM_SD_PASS"] != "s3cret-three" {
		t.Errorf("Take = %v", got)
	}
	if os.Getenv("AKAM_RTSP_PASSWORD") != "" || os.Getenv("AKAM_SD_PASS") != "" {
		t.Error("Take left a secret in the environment")
	}
	if os.Getenv("AKAM_PLAIN") != "1" {
		t.Error("Take removed a non-secret variable")
	}
}

// A child started with Env() does not see the secrets (a fake `env`-like child, never a real tool).
func TestChildDoesNotSeeSecrets(t *testing.T) {
	sh, err := exec.LookPath("sh")
	if err != nil {
		t.Skip("no sh")
	}
	t.Setenv("AKAM_RTSP_PASSWORD", "s3cret-one")
	cmd := exec.Command(sh, "-c", `printf '%s|%s' "${AKAM_RTSP_PASSWORD-unset}" "${AKAM_PLAIN-unset}"`)
	t.Setenv("AKAM_PLAIN", "1")
	cmd.Env = Env()
	out, err := cmd.Output()
	if err != nil {
		t.Fatal(err)
	}
	if string(out) != "unset|1" {
		t.Errorf("child saw %q", out)
	}
}
