package cli

import (
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/backuploc"
	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
	"github.com/Foohx/akam-fw/tools/assistant/internal/installer"
	"github.com/Foohx/akam-fw/tools/assistant/internal/installer/appfs"
	"github.com/Foohx/akam-fw/tools/assistant/internal/models"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// backupNotes is what README.txt and camera_info.txt of a backup folder say.
type backupNotes struct {
	Layout2K     bool   // mtd0..5.bin (the 2K) instead of mtdblock0..7.bin (the 1080p)
	ModelID      string // the model of the camera (registry id)
	Camera       string // the camera's address, "" when unknown
	Source       string // "ssh" or "import"
	ImportedFrom string // the folder an import was copied from
}

// writeBackupNotes writes README.txt and camera_info.txt in dir, each only when it is not there yet.
func (a *App) writeBackupNotes(dir string, n backupNotes) error {
	now := time.Now().UTC().Format("2006-01-02T15:04:05Z")
	files, kind, how := "mtdblock0..7.bin are the 8 flash partitions", "unknown", "made"
	if n.Layout2K {
		files, kind = "mtd0..5.bin are the 6 flash partitions", "cardless-appfs"
	}
	who := ""
	if n.Camera != "" {
		who = " of the camera at " + n.Camera
	}
	if n.Source == "import" {
		how = "imported"
	}
	readme := fmt.Sprintf("Akam backup%s, %s %s by %s.\n"+
		"%s as they were when saved (the camera's own firmware, or Akam when the camera already ran it).\n"+
		"Keep a second copy on a different medium. NEVER publish these files: they hold proprietary firmware and your passwords.\n"+
		"Check them with: sha256sum -c SHA256SUMS\n", who, how, now, Prog, files)
	if n.Source == "import" {
		readme += "This folder is a copy of a backup made earlier (" + lastElem(n.ImportedFrom) + "); the original was not changed.\n"
	}
	info := "model=" + n.ModelID + "\nkind=" + kind + "\nsource=" + n.Source + "\n"
	if n.Camera != "" {
		info = "camera: " + n.Camera + "\n" + info
	}
	if n.Layout2K {
		info += "--- layout (bytes)\n"
		for i, sz := range appfs.Layout() {
			info += fmt.Sprintf("mtd%d.bin %d\n", i, sz)
		}
	}
	if n.Source == "import" {
		info += importMarker(n.ImportedFrom, now)
	}
	for _, f := range []struct{ name, body string }{{"README.txt", readme}, {"camera_info.txt", info}} {
		p := filepath.Join(dir, f.name)
		if _, err := os.Lstat(p); err == nil {
			continue
		}
		if err := fsutil.WritePrivateFile(p, []byte(f.body)); err != nil {
			return err
		}
	}
	return nil
}

func importMarker(from, now string) string {
	return "imported=1\nimported_from=" + from + "\nimported_at=" + now + "\n"
}

// ---- sums ----

type sumLine struct{ hash, name string }

// parseSums reads a sha256sum / md5sum file: "<hex>  <name>" or "<hex> *<name>", blank and # lines ignored.
func parseSums(b []byte, hexLen int) ([]sumLine, error) {
	var out []sumLine
	for i, l := range strings.Split(string(b), "\n") {
		l = strings.TrimRight(l, "\r")
		if strings.TrimSpace(l) == "" || strings.HasPrefix(l, "#") {
			continue
		}
		if len(l) < hexLen+2 || l[hexLen] != ' ' {
			return nil, fmt.Errorf("line %d is not a checksum line", i+1)
		}
		h := l[:hexLen]
		if _, err := hex.DecodeString(h); err != nil {
			return nil, fmt.Errorf("line %d is not a checksum line", i+1)
		}
		name := strings.TrimPrefix(l[hexLen+1:], " ")
		name = strings.TrimPrefix(name, "*")
		if name == "" || !filepath.IsLocal(name) || strings.Contains(name, `\`) {
			return nil, fmt.Errorf("line %d names %q, which is not a file of this folder", i+1, name)
		}
		out = append(out, sumLine{strings.ToLower(h), filepath.ToSlash(name)})
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("it lists no file")
	}
	return out, nil
}

// ---- layouts ----

type bkLayout struct {
	is2K  bool
	label string
	names []string
	sizes []int64
}

func layout1080() bkLayout {
	l := bkLayout{label: "1080p (8 partitions, mtdblock0..7.bin)", sizes: camMtdSizes}
	for i := range camMtdSizes {
		l.names = append(l.names, fmt.Sprintf("mtdblock%d.bin", i))
	}
	return l
}

func layout2K() bkLayout {
	l := bkLayout{is2K: true, label: "2K (6 partitions, mtd0..mtd5.bin)"}
	for i, sz := range appfs.Layout() {
		l.names = append(l.names, fmt.Sprintf("mtd%d.bin", i))
		l.sizes = append(l.sizes, int64(sz))
	}
	return l
}

var (
	reMtdblock = regexp.MustCompile(`^mtdblock([0-9]{1,6})\.bin$`)
	reMtd      = regexp.MustCompile(`^mtd([0-9]{1,6})\.bin$`)
	reBkDate   = regexp.MustCompile(`(?:^|[^0-9])((?:19|20)[0-9]{2})-?(0[1-9]|1[0-2])-?(0[1-9]|[12][0-9]|3[01])(?:[^0-9]|$)`)
)

// lastElem is the last element of a path written for any OS (a Windows path is read on Linux too).
func lastElem(p string) string {
	p = strings.TrimRight(strings.ReplaceAll(p, `\`, "/"), "/")
	if i := strings.LastIndexByte(p, '/'); i >= 0 {
		p = p[i+1:]
	}
	return p
}

// dateFromName finds a YYYYMMDD (or YYYY-MM-DD) date in the last element of a path.
func dateFromName(p string) (time.Time, bool) {
	m := reBkDate.FindStringSubmatch(lastElem(p))
	if m == nil {
		return time.Time{}, false
	}
	t, err := time.Parse("20060102", m[1]+m[2]+m[3])
	return t, err == nil
}

func atoiSafe(s string) int {
	n := 0
	for _, c := range s {
		n = n*10 + int(c-'0')
	}
	return n
}

// detectLayout says which model's partitions a folder holds, and refuses a mixed or oversized set.
func detectLayout(names []string, want string) (bkLayout, error) {
	var has1080, has2K int
	max1, max2 := -1, -1
	for _, n := range names {
		if m := reMtdblock.FindStringSubmatch(n); m != nil {
			has1080++
			max1 = max(max1, atoiSafe(m[1]))
		} else if m := reMtd.FindStringSubmatch(n); m != nil {
			has2K++
			max2 = max(max2, atoiSafe(m[1]))
		}
	}
	var l bkLayout
	switch {
	case has1080 > 0 && has2K > 0:
		return l, fmt.Errorf("it holds both mtdblock*.bin (1080p) and mtd*.bin (2K) files: one backup is one camera")
	case has1080 == 0 && has2K == 0:
		return l, fmt.Errorf("it holds no mtdblock0..7.bin (1080p) and no mtd0..5.bin (2K)")
	case has2K > 0:
		l = layout2K()
		if max2 > 5 {
			return l, fmt.Errorf("it holds mtd%d.bin: the 2K has 6 partitions (mtd0..5), this is a different layout", max2)
		}
	default:
		l = layout1080()
		if max1 > 7 {
			return l, fmt.Errorf("it holds mtdblock%d.bin: the 1080p has 8 partitions (mtdblock0..7), this is a different layout", max1)
		}
	}
	switch {
	case want == "2k" && !l.is2K:
		return l, fmt.Errorf("--model 2k, but the folder holds the 1080p files (mtdblock0..7.bin)")
	case want == "1080p" && l.is2K:
		return l, fmt.Errorf("--model 1080p, but the folder holds the 2K files (mtd0..5.bin)")
	}
	return l, nil
}

// modelFlag turns --model into "", "1080p" or "2k" (a registry id is accepted too).
func modelFlag(v string) (string, error) {
	v = strings.ToLower(strings.TrimSpace(v))
	switch v {
	case "":
		return "", nil
	case "1080p", "2k":
		return v, nil
	}
	if m, ok := models.ByID(v); ok {
		if f, err := installer.For(m); err == nil {
			if f == installer.Appfs {
				return "2k", nil
			}
			return "1080p", nil
		}
	}
	return "", fmt.Errorf("--model must be 1080p or 2k")
}

// ---- hashing ----

type fileHashes struct {
	size        int64
	sha256, md5 string
}

func hashBoth(p string) (fileHashes, error) {
	f, err := os.Open(p)
	if err != nil {
		return fileHashes{}, err
	}
	defer f.Close()
	s, m := sha256.New(), md5.New()
	n, err := io.Copy(io.MultiWriter(s, m), f)
	if err != nil {
		return fileHashes{}, err
	}
	return fileHashes{n, hex.EncodeToString(s.Sum(nil)), hex.EncodeToString(m.Sum(nil))}, nil
}

// ---- the import ----

type planFile struct {
	rel  string // slash path inside the folder
	size int64
	mod  time.Time
}

type importPlan struct {
	src, dest, base string
	lay             bkLayout
	date            time.Time
	dateWhy         string
	parts           map[string]fileHashes
	files           []planFile // every regular file of the source, partitions included
	dirs            []string   // sub folders (slash paths), parents first
	skipped         []string   // links and special files, not copied
	haveSHA         bool
	haveMD5         bool
	haveInfo        bool
	haveReadme      bool
	verified        []string // what was checked in the source, for the report
	akamKind        bool     // the source's camera_info.txt says kind=akam
	parents         []string
	cloud           string
}

// cmdImportBackup is `import-backup --from DIR [--camera NAME] [--model 1080p|2k] [--date YYYYMMDD] [--record]`.
func (a *App) cmdImportBackup(args []string) error {
	o := a.Opt
	if len(args) > 0 {
		return usageErr("akam: import-backup takes options only: --from DIR [--camera NAME] [--model 1080p|2k] [--date YYYYMMDD] [--record] [--dry-run]")
	}
	if o.CfgFrom == "" {
		return usageErr("akam: import-backup needs --from DIR (the folder of a backup you already have); optional --camera NAME, --model 1080p|2k, --date YYYYMMDD, --record, --dry-run")
	}
	want, err := modelFlag(o.Model)
	if err != nil {
		return usageErr("akam: import-backup: " + err.Error())
	}
	var date time.Time
	if o.Date != "" {
		if date, err = time.Parse("20060102", o.Date); err != nil || len(o.Date) != 8 {
			return usageErr("akam: import-backup: --date must be YYYYMMDD (for example 20260314)")
		}
	}
	if hasControl(o.CfgFrom) {
		return a.UI.Fail("that is not a usable path (no control characters, no line breaks).", "")
	}
	src, err := filepath.Abs(o.CfgFrom)
	if err != nil {
		return a.UI.Fail("that is not a usable path.", "")
	}
	a.UI.Banner("Import an existing backup")
	st, err := os.Stat(src)
	if err != nil || !st.IsDir() {
		return a.UI.Fail("not a folder: "+ui.Sanitize(src), "give the folder that holds the partition files (mtdblock0..7.bin or mtd0..5.bin).")
	}
	ents, err := os.ReadDir(src)
	if err != nil {
		return a.UI.Fail("cannot read "+ui.Sanitize(src)+": "+ui.Sanitize(err.Error()), "")
	}
	var top []string
	for _, e := range ents {
		top = append(top, e.Name())
	}
	p := &importPlan{src: src, parts: map[string]fileHashes{}}
	if p.lay, err = detectLayout(top, want); err != nil {
		return a.UI.Fail(ui.Sanitize(src)+": "+err.Error()+".", "nothing was copied.")
	}
	if err := a.importScan(p); err != nil {
		return err
	}
	if err := a.importVerify(p); err != nil {
		return err
	}
	if !date.IsZero() {
		p.date, p.dateWhy = date, "from --date"
	} else if t, ok := dateFromName(src); ok {
		p.date, p.dateWhy = t, "from the folder name"
	} else {
		for _, n := range p.lay.names {
			for _, f := range p.files {
				if f.rel == n && f.mod.After(p.date) {
					p.date = f.mod
				}
			}
		}
		p.dateWhy = "from the date of the files"
	}
	p.base, p.cloud, _, p.parents = a.backupBase(p.date)
	camDir := filepath.Dir(p.base)
	if dup := a.importDuplicate(p, camDir); dup != "" {
		a.UI.Good("  Already there: " + ui.Sanitize(dup) + " holds the same partitions (same checksums). Nothing was copied.")
		if o.Record {
			if a.UI.Dry() {
				a.UI.Say("  WOULD record " + ui.Sanitize(dup) + " as the backup of '" + a.Cams.Name + "' (" + p.recordKey() + ").")
			} else {
				a.importRecord(p, dup)
			}
		}
		return nil
	}
	exists := func(q string) bool { _, e := os.Lstat(q); return e == nil }
	if osFacts().GOOS == "windows" || osFacts().GOOS == "darwin" {
		exists = backuploc.FoldedSet(backupNames(camDir))
	}
	p.dest = backuploc.NextFree(p.base, exists)
	if inside(src, p.dest) {
		return a.UI.Fail("the destination "+ui.Sanitize(p.dest)+" is inside the folder you import from.", "choose another backup folder with "+Prog+" backup-location DIR.")
	}
	var need int64
	for _, f := range p.files {
		need += f.size
	}
	if probs := a.backupValidateNeed(p.dest, need); len(probs) > 0 {
		return a.UI.Fail("cannot use "+ui.Sanitize(p.dest)+": "+strings.Join(probs, "; ")+".", "choose another folder with "+Prog+" backup-location DIR.")
	}
	a.importReport(p)
	if a.UI.Dry() {
		return nil
	}
	return a.importRun(p)
}

// inside reports whether path is dir itself or below it (also through links).
func inside(dir, path string) bool {
	in := func(d, p string) bool {
		rel, err := filepath.Rel(d, p)
		return err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
	}
	if in(dir, path) {
		return true
	}
	if r, err := filepath.EvalSymlinks(dir); err == nil && in(r, path) {
		return true
	}
	return false
}

// importScan lists what the source holds, never following a link, checks the partitions and hashes them.
func (a *App) importScan(p *importPlan) error {
	isPart := map[string]bool{}
	for _, n := range p.lay.names {
		isPart[n] = true
	}
	var walk func(rel string, depth int) error
	walk = func(rel string, depth int) error {
		dir := filepath.Join(p.src, filepath.FromSlash(rel))
		es, err := os.ReadDir(dir)
		if err != nil {
			return a.UI.Fail("cannot read "+ui.Sanitize(dir)+": "+ui.Sanitize(err.Error()), "")
		}
		for _, e := range es {
			r := e.Name()
			if rel != "" {
				r = rel + "/" + e.Name()
			}
			fi, err := os.Lstat(filepath.Join(p.src, filepath.FromSlash(r)))
			if err != nil {
				return a.UI.Fail("cannot read "+ui.Sanitize(r)+": "+ui.Sanitize(err.Error()), "")
			}
			switch {
			case fi.Mode().IsRegular():
				p.files = append(p.files, planFile{r, fi.Size(), fi.ModTime()})
			case fi.IsDir() && depth < 6:
				p.dirs = append(p.dirs, r)
				if err := walk(r, depth+1); err != nil {
					return err
				}
			default:
				if isPart[r] {
					return a.UI.Fail(ui.Sanitize(r)+" is not a regular file (a link or a special file): refused.", "nothing was copied.")
				}
				p.skipped = append(p.skipped, r)
			}
		}
		return nil
	}
	if err := walk("", 0); err != nil {
		return err
	}
	sort.Slice(p.files, func(i, j int) bool { return p.files[i].rel < p.files[j].rel })
	sizes := map[string]int64{}
	for _, f := range p.files {
		sizes[f.rel] = f.size
		switch f.rel {
		case "SHA256SUMS":
			p.haveSHA = true
		case "MD5SUMS":
			p.haveMD5 = true
		case "camera_info.txt":
			p.haveInfo = true
		case "README.txt":
			p.haveReadme = true
		}
	}
	var missing []string
	for i, n := range p.lay.names {
		sz, ok := sizes[n]
		switch {
		case !ok:
			missing = append(missing, n)
		case sz != p.lay.sizes[i]:
			return a.UI.Fail(fmt.Sprintf("%s is %d bytes, the %s layout needs %d: refused.", n, sz, strings.SplitN(p.lay.label, " ", 2)[0], p.lay.sizes[i]),
				"this is not a complete backup of that model (or it is another layout); nothing was copied.")
		}
	}
	if len(missing) > 0 {
		return a.UI.Fail("the backup is partial: "+strings.Join(missing, ", ")+" missing.", "a backup is all of its partitions or nothing; nothing was copied.")
	}
	for _, n := range p.lay.names {
		h, err := hashBoth(filepath.Join(p.src, n))
		if err != nil {
			return a.UI.Fail("cannot read "+n+": "+ui.Sanitize(err.Error()), "")
		}
		p.parts[n] = h
	}
	if p.haveInfo {
		p.akamKind = backupKindAkam(p.src)
	}
	return nil
}

// importVerify checks the sums the source already has against the partitions. A mismatch refuses the import.
func (a *App) importVerify(p *importPlan) error {
	check := func(file string, hexLen int, pick func(fileHashes) string) error {
		b, err := os.ReadFile(filepath.Join(p.src, file))
		if err != nil {
			return a.UI.Fail("cannot read "+file+": "+ui.Sanitize(err.Error()), "")
		}
		lines, err := parseSums(b, hexLen)
		if err != nil {
			return a.UI.Fail(file+" in "+ui.Sanitize(p.src)+" is not usable: "+err.Error()+".", "nothing was copied.")
		}
		covered := map[string]bool{}
		bad := func(n string) error {
			return a.UI.Fail(file+" does not match "+n+": the file is damaged or was changed.", "nothing was copied; use another copy of the backup.")
		}
		for _, l := range lines {
			if h, ok := p.parts[l.name]; ok {
				if pick(h) != l.hash {
					return bad(l.name)
				}
				covered[l.name] = true
				continue
			}
			other, err := hashBoth(filepath.Join(p.src, filepath.FromSlash(l.name)))
			switch {
			case err != nil && file == "MD5SUMS":
				continue // an MD5SUMS may list files that were not kept
			case err != nil:
				return a.UI.Fail(file+" lists "+l.name+", which cannot be read here.", "nothing was copied.")
			case pick(other) != l.hash:
				return bad(l.name)
			}
		}
		if len(covered) == len(p.lay.names) {
			p.verified = append(p.verified, fmt.Sprintf("%s matches all %d partitions", file, len(p.lay.names)))
			return nil
		}
		if file == "SHA256SUMS" {
			var miss []string
			for _, n := range p.lay.names {
				if !covered[n] {
					miss = append(miss, n)
				}
			}
			return a.UI.Fail("SHA256SUMS does not list "+strings.Join(miss, ", ")+": it cannot vouch for the whole backup.", "complete it or remove it (the import then writes a new one); nothing was copied.")
		}
		p.verified = append(p.verified, fmt.Sprintf("MD5SUMS matches %d of the %d partitions (the others are checked by size only)", len(covered), len(p.lay.names)))
		return nil
	}
	if p.haveSHA {
		if err := check("SHA256SUMS", 64, func(h fileHashes) string { return h.sha256 }); err != nil {
			return err
		}
	}
	if p.haveMD5 {
		if err := check("MD5SUMS", 32, func(h fileHashes) string { return h.md5 }); err != nil {
			return err
		}
	}
	if len(p.verified) == 0 {
		p.verified = append(p.verified, "no SHA256SUMS or MD5SUMS in the source: only the sizes are checked; the sums of the copy are computed from these files")
	}
	return nil
}

// importDuplicate returns the folder under camDir that holds the same partitions (same SHA256SUMS content), or "".
func (a *App) importDuplicate(p *importPlan, camDir string) string {
	es, err := os.ReadDir(camDir)
	if err != nil {
		return ""
	}
	for _, e := range es {
		d := filepath.Join(camDir, e.Name())
		if !e.IsDir() {
			continue
		}
		if inside(d, p.src) { // the folder to import already is one of the camera's backups
			return d
		}
		b, err := os.ReadFile(filepath.Join(d, "SHA256SUMS"))
		if err != nil || len(b) > 1<<20 {
			continue
		}
		lines, err := parseSums(b, 64)
		if err != nil {
			continue
		}
		got := map[string]string{}
		for _, l := range lines {
			got[l.name] = l.hash
		}
		same := true
		for _, n := range p.lay.names {
			if got[n] != p.parts[n].sha256 {
				same = false
				break
			}
		}
		if same {
			return d
		}
	}
	return ""
}

// importReport prints what the import does (the same lines are the rehearsal of --dry-run).
func (a *App) importReport(p *importPlan) {
	pre := "  "
	if a.UI.Dry() {
		pre = "  WOULD "
	}
	say := func(s string) { a.UI.Say(s) }
	say("  Source:       " + ui.Sanitize(p.src) + " (read only: it is never changed or removed)")
	say("  Camera model: " + p.lay.label + ": all partitions present, sizes match")
	for _, v := range p.verified {
		say("  Checked:      " + v)
	}
	say(fmt.Sprintf("  Camera name:  %s   Date: %s (%s)", a.Cams.Name, p.date.Format("20060102"), p.dateWhy))
	say("  Destination:  " + ui.Sanitize(p.dest) + " (private to your user)")
	if p.cloud != "" {
		a.UI.Warn(fmt.Sprintf("your Documents folder is inside %s, which copies files to the cloud and the backup holds your Wi-Fi password: the destination is in your home folder.", p.cloud))
	}
	if why := backuploc.CloudReason(p.dest, osFacts()); why != "" {
		a.UI.Warn(fmt.Sprintf("the destination looks like part of %s: its content may be copied to the cloud, with your Wi-Fi password in it.", why))
	}
	say(pre + "create " + ui.Sanitize(p.dest) + " (private) and the folders above it")
	for _, d := range p.dirs {
		say(pre + "create the sub folder " + ui.Sanitize(d))
	}
	for _, f := range p.files {
		say(fmt.Sprintf("%scopy %s (%d bytes)", pre, ui.Sanitize(f.rel), f.size))
	}
	for _, s := range p.skipped {
		say("  not copied (a link or a special file): " + ui.Sanitize(s))
	}
	if !p.haveSHA {
		say(pre + "write SHA256SUMS (computed from the partitions)")
	}
	if !p.haveReadme {
		say(pre + "write README.txt")
	}
	if p.haveInfo {
		say(pre + "mark camera_info.txt as imported (imported=1, imported_from, imported_at appended)")
	} else {
		say(pre + "write camera_info.txt (marked as imported)")
	}
	say(pre + "re-read the copy and check every partition and the sums again; remove the copy if anything differs")
	if a.Opt.Record {
		say(pre + "record the copy as the backup of '" + a.Cams.Name + "' (" + p.recordKey() + ")")
	}
}

func (p *importPlan) recordKey() string {
	switch {
	case p.lay.is2K:
		return "rot2k_backup"
	case p.akamKind:
		return "ssh_backup_dir"
	}
	return "backup_dir"
}

func (a *App) importRecord(p *importPlan, dir string) {
	_ = a.St.Set(p.recordKey(), dir)
	if p.lay.is2K {
		// a 2K backup makes the camera a 2K for every later command (options, update), without --model each time
		a.recordModel(a.model2KID())
	}
	if p.recordKey() == "backup_dir" {
		_ = a.St.Set("backup_sha", p.parts["mtdblock7.bin"].sha256)
		a.UI.Say("  Recorded as the backup of '" + a.Cams.Name + "'. The setup step is not marked done: " + Prog + " backup --backup-dir " + shellPath(ui.Sanitize(dir)) + " accepts it as the original firmware after the comparison.")
		return
	}
	a.UI.Say("  Recorded as the backup of '" + a.Cams.Name + "' (" + p.recordKey() + ").")
}

// copyRegular copies one file into a private new file (O_EXCL), keeps its date, and returns the sha256 of what was written.
func copyRegular(src, dst string, mod time.Time) (string, error) {
	in, err := os.Open(src)
	if err != nil {
		return "", err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return "", err
	}
	h := sha256.New()
	if _, err := io.Copy(io.MultiWriter(out, h), in); err != nil {
		out.Close()
		return "", err
	}
	if err := out.Sync(); err != nil {
		out.Close()
		return "", err
	}
	if err := out.Close(); err != nil {
		return "", err
	}
	fsutil.PrivateFile(dst)
	_ = os.Chtimes(dst, mod, mod)
	return hex.EncodeToString(h.Sum(nil)), nil
}

func (a *App) importRun(p *importPlan) (err error) {
	if len(p.parents) > 0 {
		err = fsutil.PrivateMkdirChain(p.parents...)
	} else {
		err = os.MkdirAll(filepath.Dir(p.dest), 0o700)
	}
	if err != nil {
		return a.UI.Fail("cannot create "+ui.Sanitize(filepath.Dir(p.dest))+": "+ui.Sanitize(err.Error()), "")
	}
	if err = fsutil.PrivateMkdirNew(p.dest); err != nil {
		return a.UI.Fail("cannot create "+ui.Sanitize(p.dest)+": "+ui.Sanitize(err.Error()), "nothing was copied.")
	}
	defer func() {
		if err != nil {
			_ = os.RemoveAll(p.dest) // a folder this run made: no partial backup stays
		}
	}()
	fail := func(msg string) error {
		err = a.UI.Fail(msg, "the copy was removed; the source was not changed.")
		return err
	}
	for _, d := range p.dirs {
		if e := os.Mkdir(filepath.Join(p.dest, filepath.FromSlash(d)), 0o700); e != nil {
			return fail("cannot create " + ui.Sanitize(d) + ": " + ui.Sanitize(e.Error()))
		}
	}
	copied := map[string]string{}
	for _, f := range p.files {
		sum, e := copyRegular(filepath.Join(p.src, filepath.FromSlash(f.rel)), filepath.Join(p.dest, filepath.FromSlash(f.rel)), f.mod)
		if e != nil {
			return fail("cannot copy " + ui.Sanitize(f.rel) + ": " + ui.Sanitize(e.Error()))
		}
		copied[f.rel] = sum
	}
	if !p.haveSHA {
		var sb strings.Builder
		for _, n := range p.lay.names {
			fmt.Fprintf(&sb, "%s  %s\n", p.parts[n].sha256, n)
		}
		if e := fsutil.WritePrivateFile(filepath.Join(p.dest, "SHA256SUMS"), []byte(sb.String())); e != nil {
			return fail("cannot write SHA256SUMS: " + ui.Sanitize(e.Error()))
		}
	}
	id := models.DefaultID
	if p.lay.is2K {
		id = a.model2KID()
	}
	if e := a.writeBackupNotes(p.dest, backupNotes{Layout2K: p.lay.is2K, ModelID: id, Source: "import", ImportedFrom: p.src}); e != nil {
		return fail("cannot write README.txt or camera_info.txt: " + ui.Sanitize(e.Error()))
	}
	if p.haveInfo {
		ip := filepath.Join(p.dest, "camera_info.txt")
		b, e := os.ReadFile(ip)
		if e == nil {
			if len(b) > 0 && b[len(b)-1] != '\n' {
				b = append(b, '\n')
			}
			b = append(b, []byte(importMarker(p.src, time.Now().UTC().Format("2006-01-02T15:04:05Z")))...)
			if e = fsutil.WritePrivateFile(ip, b); e == nil {
				h := sha256.Sum256(b)
				copied["camera_info.txt"] = hex.EncodeToString(h[:]) // the one file that is not a plain copy
			}
		}
		if e != nil {
			return fail("cannot mark camera_info.txt as imported: " + ui.Sanitize(e.Error()))
		}
	}
	// the copy is read back from the disk: every copied file, then the partitions, the sums and the model's rules
	for _, f := range p.files {
		if h, e := hashBoth(filepath.Join(p.dest, filepath.FromSlash(f.rel))); e != nil || h.sha256 != copied[f.rel] {
			return fail("the copy of " + ui.Sanitize(f.rel) + " does not read back as it was written")
		}
	}
	for _, n := range p.lay.names {
		if h, e := hashBoth(filepath.Join(p.dest, n)); e != nil || h.sha256 != p.parts[n].sha256 {
			return fail("the copy of " + n + " differs from the source")
		}
	}
	if !sha256sumCheck(p.dest, filepath.Join(p.dest, "SHA256SUMS")) {
		return fail("SHA256SUMS does not match the files of the copy")
	}
	if p.lay.is2K {
		if e := appfs.VerifyBackupDir(p.dest); e != nil {
			return fail("the copy does not verify as a 2K backup: " + ui.Sanitize(e.Error()))
		}
	}
	a.backupSecure(p.dest, false)
	a.UI.Good("  Imported and verified: " + ui.Sanitize(p.dest))
	a.UI.Warn("This backup holds your Wi-Fi password and the camera's secrets: keep it private, and keep a second copy on another medium. The folder you imported from is unchanged: remove it yourself when you no longer need it.")
	if a.Opt.Record {
		a.importRecord(p, p.dest)
	} else {
		cmd := "backup"
		if p.lay.is2K {
			cmd = "update"
		}
		a.UI.Hint("use it for this camera with --record, or: " + Prog + " " + cmd + " --backup-dir " + shellPath(ui.Sanitize(p.dest)))
	}
	return nil
}

// model2KID is the registry id written in camera_info.txt of a 2K backup: the camera's own when it is a 2K,
// else the (only) card-less appfs model.
func (a *App) model2KID() string {
	if a.is2K() {
		return a.modelID2K()
	}
	for _, m := range models.All() {
		if f, err := installer.For(m); err == nil && f == installer.Appfs {
			return m.ID
		}
	}
	return ""
}
