package appfs

import (
	"context"
	"errors"
	"fmt"
	"strings"
)

// Seed copies the Wi-Fi configuration and the SSH public key of the first-install card (and the optional service files the
// install questions made: akam.conf, mqtt, mqtt_pass, rtsp.conf, ntp) into /config/akam of the
// camera (jffs2, kept across the appfs flash), once, over SSH, while the camera runs from that card. Without them a
// card-less camera has no Wi-Fi and no SSH (docs/porting/lsc-rotatable-2k/cardless-appfs.md section 4).
//
// The copy happens on the camera (card file to /config file): the secrets never travel to this PC and nothing of them
// is printed here, the camera's answer carries file names and verdicts only. Idempotent: a file already identical is
// left alone, a file that differs is never overwritten (the operator decides), a missing one is created 0600 through a
// temporary name and checked by md5 before it takes its final name.

// SeedResult is what the camera reported for each file.
type SeedResult struct {
	Lines []string // "wpa.conf: seeded", "authorized_keys: already seeded", ...
}

// seedScript is run with sh on the camera. root is empty on the camera; tests point it at a directory.
func seedScript(root string) string {
	return `R='` + root + `'
SD=$R/mnt/sdcard/akam
CF=$R/config/akam
if ! grep -q '^/dev/mmcblk' "$R/proc/mounts" 2>/dev/null; then echo "no-card"; exit 3; fi
for f in wpa.conf authorized_keys; do
  [ -s "$SD/$f" ] || { echo "card-file-missing $f"; exit 4; }
done
grep -q 'network' "$SD/wpa.conf" || { echo "card-file-bad wpa.conf"; exit 4; }
grep -q 'ssh-ed25519' "$SD/authorized_keys" || { echo "card-file-bad authorized_keys"; exit 4; }
umask 077
mkdir -p "$CF" || { echo "mkdir-failed"; exit 5; }
chmod 700 "$CF"
rc=0
OPT=""
for f in ` + strings.Join(SetupFileNames, " ") + `; do
  [ -f "$SD/$f" ] && OPT="$OPT $f"
done
for f in wpa.conf authorized_keys $OPT; do
  src=$(md5sum < "$SD/$f" | cut -d' ' -f1)
  if [ -s "$CF/$f" ]; then
    dst=$(md5sum < "$CF/$f" | cut -d' ' -f1)
    if [ "$src" = "$dst" ]; then echo "$f: already seeded"; else echo "$f: kept, differs from the card (not overwritten)"; fi
    continue
  fi
  if cp "$SD/$f" "$CF/.$f.new" && [ "$(md5sum < "$CF/.$f.new" | cut -d' ' -f1)" = "$src" ] && mv "$CF/.$f.new" "$CF/$f"; then
    chmod 600 "$CF/$f"
    echo "$f: seeded"
  else
    rm -f "$CF/.$f.new"
    echo "$f: FAILED"
    rc=6
  fi
done
sync
exit $rc
`
}

// Seed runs the seeding on the camera behind conn.
func Seed(ctx context.Context, conn Conn, say func(string)) (*SeedResult, error) {
	return seedWith(ctx, conn, "", say)
}

func seedWith(ctx context.Context, conn Conn, root string, say func(string)) (*SeedResult, error) {
	if say == nil {
		say = func(string) {}
	}
	r, err := conn.Run(ctx, seedScript(root), nil)
	if err != nil {
		return nil, fmt.Errorf("the camera did not answer: %w", err)
	}
	var res SeedResult
	for _, l := range strings.Split(strings.TrimSpace(string(r.Stdout)), "\n") {
		if l = strings.TrimSpace(l); l != "" {
			res.Lines = append(res.Lines, l)
		}
	}
	switch r.Status {
	case 0:
		for _, l := range res.Lines {
			say(l)
		}
		return &res, nil
	case 3:
		return &res, errors.New("the camera is not running from the first-install card (no SD card is mounted): boot it with the card in, then run this again")
	case 4:
		return &res, errors.New("the card has no usable wpa.conf and authorized_keys (" + strings.Join(res.Lines, "; ") + "): prepare the card again with prepare-2k-sd")
	default:
		return &res, fmt.Errorf("seeding /config/akam failed (status %d: %s)", r.Status, strings.Join(res.Lines, "; "))
	}
}
