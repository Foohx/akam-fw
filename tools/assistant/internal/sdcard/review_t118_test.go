package sdcard

import (
	"bytes"
	"errors"
	"io"
	"math/rand"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// ---- plist: random and mutated input never panics ----

func plistSeeds(t testing.TB) [][]byte {
	var seeds [][]byte
	for _, n := range []string{"list.xml", "info_disk4.xml", "info_root.xml", "info_disk3.xml"} {
		b, err := os.ReadFile(filepath.Join("testdata", "diskutil", n))
		if err != nil {
			t.Fatal(err)
		}
		seeds = append(seeds, b)
	}
	return seeds
}

func TestPlistRandomInputNeverPanics(t *testing.T) {
	rng := rand.New(rand.NewSource(11))
	seeds := plistSeeds(t)
	pieces := []string{"<dict>", "</dict>", "<array>", "</array>", "<key>", "</key>", "<string>", "</string>", "<integer>", "<true/>",
		"<!DOCTYPE plist [<!ENTITY a \"&a;\">]>", "&a;", "&#0;", "<![CDATA[", "]]>", "<!--", "-->", "<?x", "?>", "\x00", "\xff\xfe", "<plist>", "</plist>"}
	for i := 0; i < 3000; i++ {
		var in []byte
		switch i % 3 {
		case 0: // pure noise
			in = make([]byte, rng.Intn(300))
			rng.Read(in)
		case 1: // a real document with bytes flipped, cut or spliced
			in = append([]byte(nil), seeds[rng.Intn(len(seeds))]...)
			for k := rng.Intn(6); k >= 0 && len(in) > 0; k-- {
				p := rng.Intn(len(in))
				switch rng.Intn(3) {
				case 0:
					in[p] = byte(rng.Intn(256))
				case 1:
					in = in[:p]
				default:
					in = append(in[:p], append([]byte(pieces[rng.Intn(len(pieces))]), in[p:]...)...)
				}
			}
		default: // token soup
			var b bytes.Buffer
			for k := rng.Intn(40); k >= 0; k-- {
				b.WriteString(pieces[rng.Intn(len(pieces))])
			}
			in = b.Bytes()
		}
		func() {
			defer func() {
				if r := recover(); r != nil {
					t.Fatalf("panic on %q: %v", in, r)
				}
			}()
			d, err := ParsePlist(in)
			if err == nil {
				// whatever parsed must be safely readable through the typed accessors
				_, _ = d.Str("DeviceIdentifier")
				_, _ = d.Arr("AllDisksAndPartitions")
				_ = storeDisks(d)
			}
		}()
	}
}

func FuzzParsePlist(f *testing.F) {
	for _, s := range plistSeeds(f) {
		f.Add(s)
	}
	f.Fuzz(func(t *testing.T, in []byte) { _, _ = ParsePlist(in) })
}

// type confusion in a well-formed document is an error or an absent value, never a panic
func TestMacFactsSurviveTypeConfusion(t *testing.T) {
	for _, mut := range []func([]byte) []byte{
		replaceIn("<key>AllDisksAndPartitions</key>\n\t<array>", "<key>AllDisksAndPartitions</key>\n\t<array><string>x</string><integer>1</integer>"),
		replaceIn("<key>APFSPhysicalStores</key>\n\t\t\t<array>", "<key>APFSPhysicalStores</key>\n\t\t\t<array><string>x</string>"),
		replaceIn("<key>APFSVolumes</key>\n\t\t\t<array>", "<key>APFSVolumes</key>\n\t\t\t<array><true/>"),
		replaceIn("<key>Partitions</key>\n\t\t\t<array>", "<key>Partitions</key>\n\t\t\t<array><integer>5</integer>"),
	} {
		r := newFakeRunner(t)
		r.mut["list -plist"] = mut
		func() {
			defer func() {
				if x := recover(); x != nil {
					t.Fatalf("panic: %v", x)
				}
			}()
			for _, dev := range []string{"/dev/disk4", "/dev/disk3"} {
				if _, _, err := evalMac(t, r, dev); err == nil && dev == "/dev/disk3" {
					t.Error("synthesized disk accepted")
				}
			}
		}()
	}
}

// ---- the disk behind "/" is found even when the mount scan is blind ----

func TestMacSystemContainerCannotHideOnACard(t *testing.T) {
	noMounts := func(b []byte) []byte {
		return []byte(strings.ReplaceAll(string(b), "<key>MountPoint</key>", "<key>Other</key>"))
	}
	storeOnCard := replaceIn("<key>DeviceIdentifier</key>\n\t\t\t\t\t<string>disk0s2</string>\n\t\t\t\t</dict>\n\t\t\t</array>", "<key>DeviceIdentifier</key>\n\t\t\t\t\t<string>disk4s1</string>\n\t\t\t\t</dict>\n\t\t\t</array>")
	hideStores := replaceIn("<key>APFSPhysicalStores</key>\n\t\t\t<array>\n\t\t\t\t<dict>\n\t\t\t\t\t<key>DeviceIdentifier</key>\n\t\t\t\t\t<string>disk0s2</string>", "<key>Other</key>\n\t\t\t<array>\n\t\t\t\t<dict>\n\t\t\t\t\t<key>DeviceIdentifier</key>\n\t\t\t\t\t<string>disk0s2</string>")
	cases := []struct {
		name  string
		setup func(r *fakeRunner)
		want  string
	}{
		{"list says the store is on the card, no mounts visible", func(r *fakeRunner) {
			r.mut["list -plist"] = func(b []byte) []byte { return noMounts(storeOnCard(b)) }
		}, "system role"},
		{"only the volume under / says the store is on the card", func(r *fakeRunner) {
			r.mut["list -plist"] = func(b []byte) []byte { return noMounts(hideStores(b)) }
			r.mut["info -plist /"] = replaceIn("<string>disk0s2</string>", "<string>disk4s1</string>")
		}, "system role"},
		{"no source names the store of the system container", func(r *fakeRunner) {
			r.mut["list -plist"] = func(b []byte) []byte { return noMounts(hideStores(b)) }
			r.mut["info -plist /"] = replaceIn("<key>APFSPhysicalStores</key>", "<key>Other</key>")
		}, "physical disk"},
		{"the disk behind / is not in the list", func(r *fakeRunner) {
			r.mut["info -plist /"] = replaceIn("<string>disk3</string>", "<string>disk9</string>")
		}, "not in the disk list"},
	}
	for _, c := range cases {
		r := newFakeRunner(t)
		c.setup(r)
		_, cand, err := evalMac(t, r, "/dev/disk4")
		if err == nil {
			t.Errorf("%s: accepted %+v", c.name, cand)
		} else if !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v lacks %q", c.name, err, c.want)
		}
	}
}

// ---- socket transport ----

func TestSocketTransportStopsAfterTooManyBadConnections(t *testing.T) {
	tr := &osascriptTransport{uid: 501, tmpBase: t.TempDir(), accept: 10 * time.Second,
		peer: func(*net.UnixConn) (uint32, error) { return 501, nil }} // every peer is the user, none is root
	tr.launch = func(argv []string) (<-chan error, error) {
		m := argvRe.FindStringSubmatch(argv[2])
		go func() {
			for i := 0; i < sockMaxBadPeers+4; i++ {
				c, err := net.Dial("unix", m[2])
				if err != nil {
					return
				}
				_, _ = c.Write([]byte(m[4] + "\n")) // even with the right nonce: not root
				c.Close()
			}
		}()
		return make(chan error), nil // the prompt stays open
	}
	start := time.Now()
	_, err := tr.run("/usr/local/bin/akam", Request{Op: OpList}, io.Discard)
	if err == nil || !strings.Contains(err.Error(), "too many bad connections") {
		t.Fatalf("%v", err)
	}
	if time.Since(start) > 5*time.Second {
		t.Error("slow")
	}
}

func TestSocketTransportNeverEchoesTheNonce(t *testing.T) {
	var nonce string
	tr := &osascriptTransport{uid: 501, tmpBase: t.TempDir(), accept: time.Second, peer: func(*net.UnixConn) (uint32, error) { return 0, nil },
		launch: func(argv []string) (<-chan error, error) {
			nonce = argvRe.FindStringSubmatch(argv[2])[4]
			ch := make(chan error, 1)
			ch <- errors.New("syntax error near: " + argv[2]) // osascript can quote the script
			return ch, nil
		}}
	_, err := tr.run("/usr/local/bin/akam", Request{Op: OpList}, io.Discard)
	if err == nil || nonce == "" || strings.Contains(err.Error(), nonce) {
		t.Fatalf("nonce %q in %v", nonce, err)
	}
}

func TestPrivateSockDirSkipsAnUnsafeBase(t *testing.T) {
	dir, err := privateSockDir("/tmp/a'b$(id)")
	if err != nil {
		t.Skip("no /tmp: ", err)
	}
	defer os.RemoveAll(dir)
	if !strings.HasPrefix(dir, "/tmp/akam-") {
		t.Errorf("dir %q", dir)
	}
	long := "/tmp/" + strings.Repeat("d", 120)
	if d, err := privateSockDir(long); err == nil {
		defer os.RemoveAll(d)
		if strings.HasPrefix(d, long) {
			t.Error("accepted a base that makes sun_path too long")
		}
	}
}

// ---- HelperEntry: the Linux sd-helper invocation (one argument, request on stdin) is unchanged ----

func TestHelperEntryPlainSdHelper(t *testing.T) {
	var out bytes.Buffer
	code, handled := HelperEntry([]string{"sd-helper"}, strings.NewReader(`{"op":"bogus"}`), &out, io.Discard)
	if !handled || code != 1 || !strings.Contains(out.String(), "unknown operation") {
		t.Errorf("code %d handled %v out %q", code, handled, out.String())
	}
	out.Reset()
	if code, handled = HelperEntry([]string{"sd-helper"}, strings.NewReader(`not json`), &out, io.Discard); !handled || code != 2 || !strings.Contains(out.String(), "bad request") {
		t.Errorf("code %d handled %v out %q", code, handled, out.String())
	}
	for _, args := range [][]string{nil, {"other"}, {"flash", "sd-helper"}} {
		if _, handled := HelperEntry(args, strings.NewReader(""), io.Discard, io.Discard); handled {
			t.Errorf("%v handled", args)
		}
	}
	// extra arguments are never silently treated as the plain mode
	if code, handled := HelperEntry([]string{"sd-helper", "--wipe"}, strings.NewReader(`{"op":"list"}`), io.Discard, io.Discard); !handled || code == 0 {
		t.Errorf("extra argument: code %d handled %v", code, handled)
	}
}
