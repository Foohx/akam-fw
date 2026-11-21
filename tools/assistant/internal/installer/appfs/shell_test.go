//go:build unix

package appfs

import (
	"bytes"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// repoFile finds a file of the repository by name under one of its top directories (the model's firmware directory
// moves as the repository is reorganised: the test follows it instead of hard-coding the path).
func repoFile(t testing.TB, top, name string) string {
	t.Helper()
	root, err := filepath.Abs(filepath.Join("..", "..", "..", "..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	var found string
	_ = filepath.WalkDir(filepath.Join(root, top), func(p string, d fs.DirEntry, err error) error {
		if err == nil && !d.IsDir() && d.Name() == name && strings.Contains(p, "rotatable-2k") {
			found = p
		}
		return nil
	})
	if found == "" {
		t.Fatalf("%s not found under %s", name, top)
	}
	return found
}

func haveTools(tools ...string) bool {
	for _, c := range tools {
		if _, err := exec.LookPath(c); err != nil {
			return false
		}
	}
	return true
}

// The camera-side scripts embedded in this package are those of flash_appfs.sh, byte for byte.
func TestEmbeddedScriptsAreThoseOfTheShellTool(t *testing.T) {
	b, err := os.ReadFile(repoFile(t, "firmware", "flash_appfs.sh"))
	if err != nil {
		t.Fatal(err)
	}
	s := string(b)
	hot := regexp.MustCompile(`(?s)AKAM_HOT_EOF' \|\| true\n(.*?)\nAKAM_HOT_EOF\n`).FindStringSubmatch(s)
	if hot == nil || hot[1]+"\n" != hotHelper {
		t.Error("scripts/akam_hot.sh differs from the HOT_REMOTE heredoc of flash_appfs.sh")
	}
	for name, got := range map[string]string{"WORKER": flashWorker, "ROLLBACK_WORKER": rollbackWorker} {
		m := regexp.MustCompile(`(?s)\n` + name + `='(.*?)'\n`).FindStringSubmatch(s)
		if m == nil || m[1] != got {
			t.Errorf("the embedded %s differs from flash_appfs.sh", name)
		}
	}
}

// The byte-for-byte test of the port: the Go builder and appfs/build_appfs.sh (mksquashfs) are run on the same
// fixture backup and kit. See TestGoImageEqualsShellImage for what is and is not required to be identical.
func shellImage(t *testing.T, f map[string][]byte, stock []byte) (img []byte, out string) {
	t.Helper()
	if !haveTools("bash", "mksquashfs", "unsquashfs", "python3") {
		t.Skip("bash, squashfs-tools and python3 are needed to run appfs/build_appfs.sh")
	}
	script := repoFile(t, "firmware", "build_appfs.sh")
	signPy := repoFile(t, "firmware", "calc_partition_sign.py")
	dir := t.TempDir()
	put := func(p string, b []byte, mode os.FileMode) string {
		p = filepath.Join(dir, p)
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, b, mode); err != nil {
			t.Fatal(err)
		}
		return p
	}
	// a stand-in repository around a copy of the script, with the kit's files where the script looks for them
	sb, _ := os.ReadFile(script)
	pinned := regexp.MustCompile(`BB_SHA256=[0-9a-f]+`)
	put("repo/firmware/m/image/build_appfs.sh", pinned.ReplaceAll(sb, []byte("BB_SHA256="+busyBoxPin)), 0o755)
	put("repo/firmware/m/boot/flash/akam_flash_boot.sh", f[pathOverlay+"akam_flash_boot.sh"], 0o755)
	for _, s := range []string{"own_app_stub.sh", "own_app_run.sh", "common.sh", "decloud.sh", "start_ssh.sh", "akam_net.sh", "akam_api_key.sh"} {
		put("repo/firmware/m/boot/card/akam/"+s, f[pathOverlay+s], 0o755)
	}
	put("repo/src/scripts/akam_ntp.sh", f[pathOverlay+"akam_ntp.sh"], 0o755)
	put("repo/src/scripts/akam_hostname.sh", f[pathOverlay+"akam_hostname.sh"], 0o755)
	put("repo/src/core/time32_shim.c", []byte(time32Shim), 0o644)
	put("repo/firmware/m/image/source_hash.sh", []byte("#!/bin/sh\ncase \"$1\" in sntp) echo 2222;; *) echo 1111;; esac\n"), 0o755)
	py, _ := os.ReadFile(signPy)
	put("repo/firmware/m/tools/calc_partition_sign.py", py, 0o755)
	backup := put("backup/mtd3.bin", stock, 0o644)
	put("app/akam2k_rtsp", f[pathBin+"akam2k_rtsp"], 0o755)
	put("app/akam2k_sntp", f[pathBin+"akam2k_sntp"], 0o755)
	put("app/BUILDINFO.txt", f[pathBin+"akam2k_rtsp.BUILDINFO"], 0o644)
	put("dbear/dropbearmulti", f[pathBin+"dropbearmulti"], 0o755)
	put("dbear/BUILDINFO.txt", f[pathBin+"dropbearmulti.BUILDINFO"], 0o644)
	bb := put("busybox", f[pathBin+"busybox"], 0o755)
	outDir := filepath.Join(dir, "out")
	cmd := exec.Command("bash", filepath.Join(dir, "repo/firmware/m/image/build_appfs.sh"), "--backup", backup,
		"--app-dist", filepath.Join(dir, "app"), "--dropbear-dist", filepath.Join(dir, "dbear"), "--busybox", bb, "--out", outDir)
	cmd.Env = append(os.Environ(), "TMPDIR="+dir)
	if b, err := cmd.CombinedOutput(); err != nil {
		st, _ := exec.Command("unsquashfs", "-s", backup).CombinedOutput()
		bs, _ := os.ReadFile(filepath.Join(outDir, "unsquashfs-s.txt"))
		t.Fatalf("build_appfs.sh failed: %v\n%s\nstock fixture:\n%s\nbuilt:\n%s", err, b, st, bs)
	}
	img, err := os.ReadFile(filepath.Join(outDir, "appfs-akam.squashfs"))
	if err != nil {
		t.Fatal(err)
	}
	mf, _ := os.ReadFile(filepath.Join(outDir, "MANIFEST.txt"))
	return img, string(mf)
}

// What must be identical between the Go builder and build_appfs.sh is everything the camera sees: the same tree
// (names, types, modes, owners, link targets, file contents), the same squashfs parameters, the same superblock in
// kind, the same size, padding, mkfs time and appfs_sign recipe. What cannot be required is the compressed byte
// stream: the shell uses mksquashfs (liblzma, its own block ordering heuristics) and the Go writer its own xz encoder,
// so two correct images are different byte strings. The test therefore compares the decoded content and records
// whether the bytes happen to be equal. The appfs_sign of each image is computed by its own tool (python3
// calc_partition_sign.py for the shell one) and checked against the Go Sign() of the SAME bytes, which proves the
// port of the sign on a real image.
func TestGoImageEqualsShellImage(t *testing.T) {
	f := kitFiles()
	useBusyBox(t, f)
	stock := stockAppfs(t, stockInit)
	shell, manifest := shellImage(t, f, stock)
	res, err := build(t, stock, f)
	if err != nil {
		t.Fatal(err)
	}
	if len(shell) != PartitionSize || len(res.Image) != PartitionSize {
		t.Fatalf("sizes %d %d", len(shell), len(res.Image))
	}
	if d := diffTree("", treeOf(t, shell), res.Tree); d != "" {
		t.Errorf("the tree of the shell image differs from the Go tree at %s", d)
	}
	if d := diffTree("", treeOf(t, shell), treeOf(t, res.Image)); d != "" {
		t.Errorf("the tree read back from the Go image differs from the shell image at %s", d)
	}
	for _, r := range [][2]int{{0, 4}, {8, 4}, {12, 4}, {20, 12}, {56, 8}} {
		if !bytes.Equal(shell[r[0]:r[0]+r[1]], res.Image[r[0]:r[0]+r[1]]) {
			t.Errorf("superblock bytes %v differ between the shell and the Go image", r)
		}
	}
	if !allZero(shell[res.Used:]) && len(shell) > 0 {
		t.Log("note: the shell image has non-zero bytes after the Go bytes_used (its stream is longer)")
	}
	shellSign, err := Sign(shell)
	if err != nil || !strings.Contains(manifest, "appfs_sign="+shellSign) {
		t.Errorf("Sign() of the shell image is %q, its manifest says\n%s", shellSign, manifest)
	}
	if d := os.Getenv("AKAM_APPFS_KEEP"); d != "" { // for a manual look with cmp/unsquashfs
		_ = os.WriteFile(filepath.Join(d, "shell.squashfs"), shell, 0o600)
		_ = os.WriteFile(filepath.Join(d, "go.squashfs"), res.Image, 0o600)
	}
	if bytes.Equal(shell, res.Image) {
		t.Log("the Go image is byte-identical to the mksquashfs one")
	} else {
		t.Logf("the compressed streams differ (expected, see the test comment): shell %s, go %s", shaHex(shell), res.SHA256)
	}
}
