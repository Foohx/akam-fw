package appfs

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
)

// The update of a card-less 2K, as one flow: PrepareUpdate (host only: nothing is sent to the camera), then RunUpdate
// (rehearsal that writes nothing, explicit confirmation, write, wait, verify, keep the previous image).
//
// Every safety gate of the write lives in Flash (the port of tools/flash_appfs.sh) and is reused unchanged: a fresh
// dump of mtd3 must equal the image believed to be on the camera byte for byte, the hot runner proves card-less boot,
// the process census, the memory budget, the detached worker rolls back by itself, the read-back is compared before any
// reboot. This file adds the order of the screens and the bookkeeping, never a way around a gate: it cannot build a
// live Flash run without a Confirmation, and it asks for one before each live run.

// ErrUpToDate: the image to install is the one on the camera.
var ErrUpToDate = errors.New("the camera already runs this image: nothing to update")

// Camera is what the control API says about the camera (empty when it could not be asked).
type Camera struct {
	Model   string
	Version string
}

// ImageMeta describes a stored image.
type ImageMeta struct {
	SHA256  string `json:"sha256"`
	Version string `json:"version"`
	Commit  string `json:"commit"`
	Sign    string `json:"appfs_sign"`
	Time    string `json:"time"`
}

// CleanWorkDir removes what a run left in its work directory that nothing needs any more. mtd4 (the jffs2 config
// partition: it holds the Wi-Fi settings) is never needed to go back and is always deleted. When the update is
// verified (verified true) the other full dumps and the read-back go too: the image that was on the camera and the one
// that is now are in the image store. Otherwise they stay, they are what an operator needs to look at a failure.
func CleanWorkDir(dir string, verified bool) {
	if dir == "" {
		return
	}
	names := []string{"mtd4.bin"}
	if verified {
		names = append(names, "mtd0.bin", "mtd1.bin", "mtd2.bin", "mtd3.bin", "mtd5.bin", "mtd3.readback.bin")
	}
	for _, n := range names {
		_ = os.Remove(filepath.Join(dir, n))
	}
}

// Reconciled says what Reconcile did with a pending record.
type Reconciled int

const (
	ReconNone      Reconciled = iota // nothing was pending
	ReconPromoted                    // the camera runs the pending image: it is now the current one
	ReconDiscarded                   // the camera still runs the current image: the pending one was dropped
)

// rerun is what every refusal that follows a write tells the operator to do next.
const rerun = "run `./akam update` again: it reads mtd3 on the camera first and records what it finds"

// Reconcile settles a pending record left by an update whose result the PC did not finish recording (Ctrl-C at the
// power-cycle prompt, a closed terminal, another IP after the reboot, a sleeping PC). It reads mtd3 READ-ONLY and
// compares: equal to pending -> promoted; equal to current -> discarded; neither -> refused with the hashes (nothing
// is changed). alsoSHA is the sha256 of an image the operator names as being on the camera (--current-image): mtd3
// equal to it also discards the pending record (the byte for byte gate then proves it again before any write).
func (s ImageStore) Reconcile(ctx context.Context, dial func(port int) (Conn, error), alsoSHA string, say func(string)) (Reconciled, error) {
	if say == nil {
		say = func(string) {}
	}
	pend, pm, err := s.Load("pending")
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return ReconNone, nil
		}
		return ReconNone, fmt.Errorf("an earlier update left a record that cannot be used (%v). Nothing was changed on the camera. Delete the folder %s, then %s", err, s.Dir, rerun)
	}
	pendSHA := sum(pend)
	curSHA := ""
	if cur, _, e := s.Load("current"); e == nil {
		curSHA = sum(cur)
	}
	say("An earlier update was written but never recorded here: reading mtd3 on the camera (read only) to settle it.")
	conn, err := dial(0)
	if err != nil {
		return ReconNone, fmt.Errorf("an earlier update was written (image %s, version %s) but not recorded, and the camera cannot be reached (%v). Nothing was changed. Power the camera on, check its IP, then %s", short(pendSHA), pm.Version, err, rerun)
	}
	defer conn.Close()
	r, err := conn.Run(ctx, "dd if=/dev/mtd3 bs=65536 2>/dev/null", nil)
	if err != nil || r.Status != 0 || len(r.Stdout) != PartitionSize {
		return ReconNone, fmt.Errorf("an earlier update was not recorded and mtd3 could not be read to settle it. Nothing was changed. Wait for the camera to be fully started, then %s", rerun)
	}
	got := sum(r.Stdout)
	switch {
	case got == pendSHA:
		if err := s.Promote(); err != nil {
			return ReconNone, fmt.Errorf("the camera runs the pending image (sha256 %s) but it could not be recorded: %w. Nothing was changed on the camera. Free some disk space or fix the permissions of %s, then %s", got, err, s.Dir, rerun)
		}
		say(fmt.Sprintf("The camera runs the image of the earlier update (sha256 %s): recorded as the current image.", short(got)))
		return ReconPromoted, nil
	case got == curSHA || (alsoSHA != "" && got == alsoSHA):
		s.Discard()
		say(fmt.Sprintf("The camera still runs the recorded image (sha256 %s): the unfinished update record was dropped.", short(got)))
		return ReconDiscarded, nil
	}
	return ReconNone, fmt.Errorf("mtd3 on the camera (sha256 %s) is neither the pending image of an earlier update (%s) nor the recorded current image (%s). Nothing was changed. If you know which image file is on the camera, run `./akam update --current-image FILE` with it; otherwise restore the camera from your backup (docs/porting/lsc-rotatable-2k/cardless-appfs.md, recovery) before updating", got, pendSHA, orNone(curSHA))
}

func short(h string) string {
	if len(h) > 16 {
		return h[:16]
	}
	return h
}

func orNone(h string) string {
	if h == "" {
		return "none"
	}
	return h
}

// Change is one file that differs between two images.
type Change struct {
	Path string
	Kind string // added | removed | changed
}

// DiffImages lists the regular files and links that differ between two appfs images (what an update changes).
func DiffImages(oldImg, newImg []byte) ([]Change, error) {
	a, err := fileHashes(oldImg)
	if err != nil {
		return nil, fmt.Errorf("current image: %w", err)
	}
	b, err := fileHashes(newImg)
	if err != nil {
		return nil, fmt.Errorf("new image: %w", err)
	}
	var out []Change
	for p, h := range b {
		switch o, ok := a[p]; {
		case !ok:
			out = append(out, Change{p, "added"})
		case o != h:
			out = append(out, Change{p, "changed"})
		}
	}
	for p := range a {
		if _, ok := b[p]; !ok {
			out = append(out, Change{p, "removed"})
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out, nil
}

func fileHashes(img []byte) (map[string]string, error) {
	fsr, err := squashfs.Open(bytes.NewReader(img), int64(len(img)))
	if err != nil {
		return nil, err
	}
	m := map[string]string{}
	err = fsr.Walk("/", func(fi squashfs.FileInfo) error {
		switch fi.Type {
		case squashfs.TypeFile:
			b, err := fsr.ReadFile(fi.Path)
			if err != nil {
				return err
			}
			h := sha256.Sum256(b)
			m[fi.Path] = hex.EncodeToString(h[:])
		case squashfs.TypeSymlink:
			m[fi.Path] = "link:" + fi.Target
		}
		return nil
	})
	return m, err
}

// UpdateInput of PrepareUpdate.
type UpdateInput struct {
	ModelID   string // the registry id of the model being updated
	Camera    Camera // what the API said; Camera.Model == "" means it could not be asked
	Kit       *Kit   // the verified kit of the release to install (nil only for Rollback)
	BackupDir string // the verified full backup (mtd3.bin is the base of the image)
	Store     ImageStore
	// CurrentOverride is the image on the camera, given by the operator (a camera flashed by the scripts): it becomes
	// the stored current image. Flash proves it by a byte for byte comparison with a fresh dump before any write.
	CurrentOverride []byte
	Rollback        bool // install the previous image instead of a new one
	Hot             bool // card-less update (default of the flow); false = from an SD card boot
	Force           bool // allow a downgrade
	Target          string
	WorkDir         string
	Say             func(string)
}

// Prepared is a ready, verified, not yet flashed update.
type Prepared struct {
	In                      UpdateInput
	FromVersion, ToVersion  string
	Image                   []byte
	SHA, Sign, Commit       string
	Current                 []byte
	CurrentSHA, CurrentFrom string
	Changes                 []Change
	Warnings                []string
	Rollback, Hot           bool
	meta                    ImageMeta
	curMeta                 ImageMeta
}

// PrepareUpdate checks everything that can be checked on the PC and builds the image. It never talks to the camera.
func PrepareUpdate(in UpdateInput) (*Prepared, error) {
	say := in.Say
	if say == nil {
		say = func(string) {}
	}
	if in.Camera.Model != "" && in.Camera.Model != in.ModelID {
		return nil, fmt.Errorf("the camera says it is a %q, this update is for %q: refused", in.Camera.Model, in.ModelID)
	}
	p := &Prepared{In: in, Rollback: in.Rollback, Hot: in.Hot, FromVersion: in.Camera.Version}
	// the image on the camera: the rollback source and the reference of the byte for byte gate
	switch {
	case in.CurrentOverride != nil:
		if len(in.CurrentOverride) != PartitionSize {
			return nil, fmt.Errorf("--current-image is not %d bytes", PartitionSize)
		}
		p.Current, p.CurrentFrom = in.CurrentOverride, "given with --current-image"
		p.curMeta = ImageMeta{SHA256: sum(p.Current), Version: in.Camera.Version}
	default:
		b, m, err := in.Store.Load("current")
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				return nil, errors.New("this assistant has no record of the image on the camera. If the camera runs an image you flashed with an older tool or by hand, give that image file with --current-image FILE (it is checked byte for byte against the camera before anything is written). Otherwise do the first install with appfs-flash --execute: it records the image it writes")
			}
			return nil, err
		}
		p.Current, p.curMeta, p.CurrentFrom = b, m, "recorded by the assistant at the last update"
	}
	p.CurrentSHA = sum(p.Current)
	if p.FromVersion == "" {
		p.FromVersion = p.curMeta.Version
	}
	if in.Rollback {
		b, m, err := in.Store.Load("previous")
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				return nil, errors.New("no previous image is kept: nothing to roll back to (the SD card recovery and the stock backup are the other ways back)")
			}
			return nil, err
		}
		p.Image, p.meta = b, m
		if in.CurrentOverride != nil || !in.Store.PreviousIsPredecessor() {
			p.Warnings = append(p.Warnings, "WARNING: the previous image kept here is not known to be the one that ran just before the image on the camera (the current image was named with --current-image). Check the version above before you continue.")
		}
	} else {
		if in.Kit == nil {
			return nil, errors.New("no kit: choose the release to install")
		}
		if got := in.Kit.Manifest["model"]; got != "" && got != in.ModelID {
			return nil, fmt.Errorf("this kit is for %q, not %q: refused", got, in.ModelID)
		}
		stock, err := os.ReadFile(filepath.Join(in.BackupDir, "mtd3.bin"))
		if err != nil {
			return nil, fmt.Errorf("cannot read mtd3.bin in the backup: %w (the image is built from YOUR backup)", err)
		}
		res, err := Build(Inputs{Backup: stock, Kit: in.Kit, Say: say})
		if err != nil {
			return nil, fmt.Errorf("the image was not built: %w", err)
		}
		p.Image = res.Image
		p.meta = ImageMeta{SHA256: res.SHA256, Version: in.Kit.Version(), Commit: in.Kit.Commit(), Sign: res.Sign}
	}
	p.SHA, p.ToVersion, p.Commit = sum(p.Image), p.meta.Version, p.meta.Commit
	sign, err := Sign(p.Image)
	if err != nil {
		return nil, fmt.Errorf("the new image is not a valid appfs image: %w", err)
	}
	p.Sign = sign
	p.meta.SHA256, p.meta.Sign = p.SHA, sign
	p.meta.Time = time.Now().UTC().Format(time.RFC3339)
	if p.SHA == p.CurrentSHA {
		return p, ErrUpToDate
	}
	if !in.Force && !in.Rollback {
		if c, ok := kit.CompareVersions(p.ToVersion, p.FromVersion); ok && c < 0 {
			return nil, fmt.Errorf("the kit %s is older than what runs now (%s): refused. Use the rollback to go back to the previous image, or --force", p.ToVersion, p.FromVersion)
		}
	}
	ch, err := DiffImages(p.Current, p.Image)
	if err != nil {
		return nil, err
	}
	p.Changes = ch
	return p, nil
}

// Summary is the "what changes" text, one line per item.
func (p *Prepared) Summary() []string {
	from, to := p.FromVersion, p.ToVersion
	if from == "" {
		from = "unknown"
	}
	if to == "" {
		to = "unknown"
	}
	l := []string{
		fmt.Sprintf("Version: %s -> %s%s", from, to, map[bool]string{true: " (rollback to the previous image)"}[p.Rollback]),
		fmt.Sprintf("Image now on the camera: sha256 %s (%s)", p.CurrentSHA[:16], p.CurrentFrom),
		fmt.Sprintf("New image: sha256 %s, %d bytes (partition mtd3)", p.SHA[:16], len(p.Image)),
	}
	if p.Commit != "" {
		l = append(l, "Source commit of the new image: "+p.Commit)
	}
	l = append(l, p.Warnings...)
	n := len(p.Changes)
	l = append(l, fmt.Sprintf("Files that change: %d", n))
	for i, c := range p.Changes {
		if i == 12 {
			l = append(l, fmt.Sprintf("  ... and %d more", n-12))
			break
		}
		l = append(l, fmt.Sprintf("  %-8s %s", c.Kind, c.Path))
	}
	mode := "from an SD card boot (the card stays in)"
	if p.Hot {
		mode = "card-less (hot update: the camera runs from RAM for the write, nothing to insert)"
	}
	l = append(l, "Method: "+mode, "The previous image is kept on this PC for a rollback.")
	return l
}

// Asks are the questions of a live run, answered by a person (default No, refused without a terminal).
type Asks struct {
	AckRecovery      func() (bool, error)           // UART or CH341A at hand, backup verified
	ConfirmRehearsal func() (bool, error)           // the reversible rehearsal
	ConfirmWrite     func(sha string) (bool, error) // the typed go naming the image and the partition
}

// RunDeps of RunUpdate.
type RunDeps struct {
	Flash     func(Plan, Confirmation, Deps) (*FlashResult, error)
	FlashDeps Deps // Dial and Out are the camera's; Ctx too
	Asks      Asks
	// Detect asks the control API (nil or failing: the verification relies on the flash read-back alone).
	Detect func(ctx context.Context) (Camera, error)
	// PowerCycle tells the operator to power-cycle the camera and returns when they say it is done.
	PowerCycle func() error
	Sleep      func(time.Duration)
	Say        func(string)
	Tries      int // reconnect attempts after the power cycle, 5 s apart (default 60)
}

// RunResult of RunUpdate.
type RunResult struct {
	Rehearsed, Written, Verified bool
	VersionSeen                  string
	Note                         string
}

func (d *RunDeps) say(f string, a ...any) {
	if d.Say != nil {
		d.Say(fmt.Sprintf(f, a...))
	}
}

// RunUpdate runs the live part. Order: stage the image on the PC, rehearsal (hot: H1..H3 then restore, never
// writes), confirmation of the write, write with read-back, power cycle by the operator, wait, verify mtd3 and the
// version, promote the stored images. A refusal at any question stops with nothing written.
func RunUpdate(p *Prepared, d RunDeps) (*RunResult, error) {
	if d.Flash == nil {
		d.Flash = Flash
	}
	if d.FlashDeps.Dial == nil {
		return nil, errors.New("no way to connect to the camera")
	}
	if d.Asks.AckRecovery == nil || d.Asks.ConfirmRehearsal == nil || d.Asks.ConfirmWrite == nil {
		return nil, errors.New("internal: a live update needs all three questions")
	}
	sleep := d.Sleep
	if sleep == nil {
		sleep = time.Sleep
	}
	res := &RunResult{}
	defer func() { CleanWorkDir(p.In.WorkDir, res.Verified) }()
	base := Plan{Image: p.Image, ImageSHA256: p.SHA, Sign: p.Sign, BackupDir: p.In.BackupDir, WorkDir: p.In.WorkDir,
		Target: p.In.Target, Current: p.Current, Update: true, OwnReboot: true}
	// An image named by the operator (--current-image) is recorded as the current one only once a live step (a)
	// compared it with a fresh dump of mtd3 byte for byte (FlashResult.CurrentProven): a wrong file must never
	// replace a good record.
	proven := func(fr *FlashResult) error {
		if p.In.CurrentOverride == nil || fr == nil || !fr.CurrentProven {
			return nil
		}
		if err := p.In.Store.SetCurrent(p.Current, p.curMeta); err != nil {
			return fmt.Errorf("the image you named is the one on the camera, but it could not be recorded on this PC: %w. Nothing was written to the camera. Fix that, then %s", err, rerun)
		}
		p.In.CurrentOverride = nil // recorded
		return nil
	}
	if err := p.In.Store.Stage(p.Image, p.meta); err != nil {
		return res, fmt.Errorf("cannot keep the new image on this PC: %w (nothing was sent to the camera)", err)
	}
	keep := false
	defer func() {
		if !keep {
			p.In.Store.Discard()
		}
	}()
	// 1. the preflight. Hot: the reversible rehearsal, itself a live run, so it has its own (lighter) confirmation.
	if p.Hot {
		d.say("Step 1 of 4: rehearsal on the camera. It stops the app, moves the helpers to RAM and restores everything. It writes no flash.")
		c, err := NewConfirmation(p.SHA, d.Asks.AckRecovery, d.Asks.ConfirmRehearsal)
		if err != nil {
			return res, err
		}
		pl := base
		pl.Hot, pl.HotPreflight, pl.Execute = false, true, true
		fr, err := d.Flash(pl, c, d.FlashDeps)
		if perr := proven(fr); perr != nil {
			return res, perr
		}
		if err != nil {
			return res, fmt.Errorf("the rehearsal failed, nothing was written: %w", err)
		}
		res.Rehearsed = true
	} else {
		d.say("Step 1 of 4: dry run (local checks and the printed plan; no connection).")
		pl := base
		if _, err := d.Flash(pl, Confirmation{}, d.FlashDeps); err != nil {
			return res, fmt.Errorf("the dry run failed, nothing was written: %w", err)
		}
	}
	// 2. the explicit go for the write
	d.say("Step 2 of 4: the write. This is the only step that changes the camera flash.")
	c, err := NewConfirmation(p.SHA, d.Asks.AckRecovery, func() (bool, error) { return d.Asks.ConfirmWrite(p.SHA) })
	if err != nil {
		return res, err
	}
	pl := base
	pl.Hot, pl.Execute = p.Hot, true
	fr, err := d.Flash(pl, c, d.FlashDeps)
	if perr := proven(fr); perr != nil {
		keep = true
		return res, perr
	}
	if err != nil {
		if fr != nil && fr.Live && fr.WorkerResult != "" {
			keep = true // a write may have started: keep the staged image, the operator must look
		}
		return res, err
	}
	if fr == nil || !fr.Written {
		keep = true
		return res, errors.New("the flash did not report a verified write: do not reboot, read the lines above")
	}
	res.Written = true
	keep = true
	// 3. the operator power-cycles; the program never reboots the camera
	d.say("Step 3 of 4: power-cycle the camera yourself (this program never reboots it).")
	if d.PowerCycle != nil {
		if err := d.PowerCycle(); err != nil {
			return res, fmt.Errorf("%w: the image is written and verified; the stored images were not updated yet. Power-cycle the camera, then %s", err, rerun)
		}
	}
	// 4. wait and verify
	d.say("Step 4 of 4: waiting for the camera and checking it.")
	if err := d.verify(p, res, sleep); err != nil {
		return res, err
	}
	if err := p.In.Store.Promote(); err != nil {
		return res, fmt.Errorf("the camera is verified but the images could not be recorded: %w. Nothing is wrong on the camera: free some disk space or fix the permissions of %s, then %s", err, p.In.Store.Dir, rerun)
	}
	res.Verified = true
	return res, nil
}

func (d *RunDeps) verify(p *Prepared, res *RunResult, sleep func(time.Duration)) error {
	tries := d.Tries
	if tries == 0 {
		tries = 60
	}
	ctx := d.FlashDeps.Ctx
	if ctx == nil {
		ctx = context.Background()
	}
	var conn Conn
	for i := 0; i < tries && conn == nil; i++ {
		if c, err := d.FlashDeps.Dial(0); err == nil {
			if r, e := c.Run(ctx, "true", nil); e == nil && r.Status == 0 {
				conn = c
				break
			}
			c.Close()
		}
		sleep(5 * time.Second)
	}
	if conn == nil {
		return fmt.Errorf("the camera did not come back in %d seconds. The image was written and verified before the reboot. Check that it is powered and on the network, then %s. If it never boots: insert the Akam SD card and power-cycle (the card overrides the flashed image), see docs/porting/lsc-rotatable-2k/cardless-appfs.md", tries*5, rerun)
	}
	defer conn.Close()
	r, err := conn.Run(ctx, "dd if=/dev/mtd3 bs=65536 2>/dev/null", nil)
	if err != nil || r.Status != 0 || len(r.Stdout) != PartitionSize {
		return fmt.Errorf("the camera is back but mtd3 could not be read for the final check: not marked as updated. Wait a minute for it to be fully started, then %s", rerun)
	}
	if got := sum(r.Stdout); got != p.SHA {
		return fmt.Errorf("the camera is back but mtd3 holds sha256 %s, expected %s: NOT updated. Nothing was recorded. To check again, %s", got[:16], p.SHA[:16], rerun)
	}
	d.say("mtd3 after the reboot is the new image (sha256 %s).", p.SHA[:16])
	if d.Detect != nil {
		var cam Camera
		var derr error
		for i := 0; i < 12; i++ { // the application starts a little after sshd
			if cam, derr = d.Detect(ctx); derr == nil && cam.Version != "" {
				break
			}
			sleep(5 * time.Second)
		}
		if derr == nil && cam.Version != "" {
			res.VersionSeen = cam.Version
			if !versionMatches(cam.Version, p) {
				res.Note = fmt.Sprintf("the camera reports version %q, the kit says %q", cam.Version, p.ToVersion)
				d.say("WARNING: %s (the image itself is verified).", res.Note)
			} else {
				d.say("The camera reports version %s.", cam.Version)
			}
		} else {
			res.Note = "the control API did not answer: the version was not checked (the image itself is verified)"
			d.say("Note: %s.", res.Note)
		}
	}
	return nil
}

// versionMatches: the camera's version string is the kit version, or starts with it, or names the kit's commit.
func versionMatches(seen string, p *Prepared) bool {
	if p.ToVersion != "" && (seen == p.ToVersion || strings.HasPrefix(seen, p.ToVersion)) {
		return true
	}
	if len(p.Commit) >= 7 && strings.Contains(seen, p.Commit[:7]) {
		return true
	}
	return false
}
