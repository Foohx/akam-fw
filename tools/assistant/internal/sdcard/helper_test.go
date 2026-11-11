//go:build akamtest

package sdcard

import (
	"bytes"
	"encoding/json"
	"os"
	"strings"
	"testing"
)

func imageReq(op string, path string, size int64) Request {
	return Request{Op: op, Device: path, Token: ImageToken(size), ImageFile: true}
}

func prepareReq(t *testing.T, path string, size int64) Request {
	tk := synthToolkit()
	files, err := BuildPayload(PayloadInput{Mode: ModeBackup, Toolkit: tk, SSID: "net", Pass: "pw-secret", Flash: nil})
	if err != nil {
		t.Fatal(err)
	}
	r := imageReq(OpPrepare, path, size)
	r.Files = files
	r.Verify = &VerifySpec{Mode: ModeBackup, Toolkit: ToolkitHashes(tk)}
	return r
}

func TestHelperPrepareAndWipeOnAnImageFile(t *testing.T) {
	const size = 192 << 20
	f, p := sparse(t, size)
	_ = f
	resp := Helper{}.Handle(prepareReq(t, p, size))
	if !resp.OK {
		t.Fatalf("%s\n%s", resp.Error, strings.Join(resp.Lines, "\n"))
	}
	if resp.Lines[len(resp.Lines)-1] != "VERIFY: OK" {
		t.Errorf("lines: %v", resp.Lines)
	}
	// verify op on the written card
	vr := imageReq(OpVerify, p, size)
	vr.Verify = &VerifySpec{Mode: ModeBackup}
	if r := (Helper{}).Handle(vr); !r.OK {
		t.Errorf("verify: %s", r.Error)
	}
	// the card holds the password in clear text: after the wipe not a single byte of it is left
	raw, _ := os.ReadFile(p)
	if !bytes.Contains(raw, []byte("pw-secret")) {
		t.Fatal("test premise: the password is on the card")
	}
	if r := (Helper{}).Handle(imageReq(OpWipe, p, size)); !r.OK {
		t.Fatal(r.Error)
	}
	raw, _ = os.ReadFile(p)
	if len(raw) != size || bytes.Count(raw, []byte{0}) != size {
		t.Error("the wipe left non-zero bytes")
	}
}

func TestHelperRefusals(t *testing.T) {
	const size = 192 << 20
	_, p := sparse(t, size)
	h := Helper{}
	cases := map[string]Request{
		"unknown op":    {Op: "format-everything", Device: p, Token: ImageToken(size), ImageFile: true},
		"no device":     {Op: OpWipe, Token: ImageToken(size), ImageFile: true},
		"wrong token":   {Op: OpWipe, Device: p, Token: "image|1", ImageFile: true},
		"no token":      {Op: OpWipe, Device: p, ImageFile: true},
		"not a file":    {Op: OpWipe, Device: t.TempDir(), Token: ImageToken(0), ImageFile: true},
		"real device":   {Op: OpWipe, Device: "/dev/null", Token: ImageToken(0)}, // no image-file flag: the guards apply
		"prepare empty": {Op: OpPrepare, Device: p, Token: ImageToken(size), ImageFile: true},
	}
	for n, r := range cases {
		if resp := h.Handle(r); resp.OK {
			t.Errorf("%s: accepted", n)
		}
	}
	// nothing was written by any refusal
	raw, _ := os.ReadFile(p)
	if bytes.Count(raw, []byte{0}) != size {
		t.Error("a refused request wrote to the image")
	}
}

func TestHelperDeviceGuardsApplyToRealPaths(t *testing.T) {
	f := newFakeSys(t)
	f.disk("sdc", 32*gib, "0", "usb1", "Hard disk", "8:32")
	h := Helper{Sys: f.Sys}
	r := h.Handle(Request{Op: OpWipe, Device: f.dev("sdc"), Token: "x"})
	if r.OK || !strings.Contains(r.Error, "not removable") {
		t.Errorf("got %+v", r)
	}
	f.disk("sdb", 32*gib, "1", "usb2", "Card", "8:16")
	c, _ := f.Sys.Check(f.dev("sdb"))
	// right guards, right token, but the node is not a block device (a regular file in the fake)
	r = h.Handle(Request{Op: OpWipe, Device: f.dev("sdb"), Token: c.Ident})
	if r.OK || !strings.Contains(r.Error, "not a block device") {
		t.Errorf("got %+v", r)
	}
	// a changed identity (another card in the slot)
	r = h.Handle(Request{Op: OpWipe, Device: f.dev("sdb"), Token: "123|Other|||x|8:16"})
	if r.OK || !strings.Contains(r.Error, "changed") {
		t.Errorf("got %+v", r)
	}
}

func TestHelperMainProtocol(t *testing.T) {
	const size = 192 << 20
	_, p := sparse(t, size)
	body, _ := json.Marshal(imageReq(OpWipe, p, size))
	var out bytes.Buffer
	if rc := HelperMain(bytes.NewReader(body), &out, Helper{}); rc != 0 {
		t.Fatalf("rc %d: %s", rc, out.String())
	}
	var resp Response
	json.Unmarshal(out.Bytes(), &resp)
	if !resp.OK {
		t.Error(resp.Error)
	}
	out.Reset()
	if rc := HelperMain(strings.NewReader("not json"), &out, Helper{}); rc != 2 {
		t.Errorf("bad request rc %d", rc)
	}
	// argv is never used: the request holds the secret only on stdin
}
