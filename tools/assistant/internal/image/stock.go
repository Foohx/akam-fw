package image

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Stock is the user's backup of the stock APP partition (mtdblock7.bin) and what is known of the kernel.
type Stock struct {
	// App is the content of mtdblock7.bin (exactly PartitionSize bytes).
	App []byte
	// Name is the file name shown in the report ("mtdblock7.bin").
	Name string
	// Kernel is the kernel version read from the uImage name of mtdblock4.bin ("" when unknown).
	Kernel string
	// KernelExpected is true when a whole backup folder was given (an unreadable kernel then counts).
	KernelExpected bool
}

// LoadBackup reads a backup folder the way build_firmware.sh --backup-dir does: exactly the eight dumps
// mtdblock0.bin to mtdblock7.bin, none empty, the APP dump of the partition size, and the kernel name from
// the u-boot header of mtdblock4.bin.
func LoadBackup(dir string) (*Stock, error) {
	st, err := os.Stat(dir)
	if err != nil || !st.IsDir() {
		return nil, fmt.Errorf("backup folder %s is not a directory", dir)
	}
	n := 0
	if ents, err := os.ReadDir(dir); err == nil {
		for _, e := range ents {
			if ok, _ := filepath.Match("mtdblock*.bin", e.Name()); ok {
				n++
			}
		}
	}
	if n != 8 {
		return nil, fmt.Errorf("MTD layout differs from the supported one: %d mtdblock*.bin in %s, expected 8. Refusing.", n, dir)
	}
	for i := 0; i < 8; i++ {
		p := filepath.Join(dir, fmt.Sprintf("mtdblock%d.bin", i))
		if fi, err := os.Stat(p); err != nil || fi.Size() == 0 {
			return nil, fmt.Errorf("MTD layout differs from the supported one: %s is missing or empty. Refusing.", p)
		}
	}
	s := &Stock{Name: "mtdblock7.bin", KernelExpected: true}
	app := filepath.Join(dir, "mtdblock7.bin")
	if fi, err := os.Stat(app); err != nil || fi.Size() != PartitionSize {
		return nil, fmt.Errorf("MTD layout differs: %s is not %d bytes, the APP partition (mtd7) is %d. Refusing.", app, PartitionSize, PartitionSize)
	}
	if s.App, err = os.ReadFile(app); err != nil {
		return nil, err
	}
	if k, err := readHead(filepath.Join(dir, "mtdblock4.bin"), 64); err == nil {
		s.Kernel = kernelName(k)
	}
	return s, nil
}

func readHead(p string, n int) ([]byte, error) {
	f, err := os.Open(p)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	b := make([]byte, n)
	m, err := f.Read(b)
	if m == 0 && err != nil {
		return nil, err
	}
	return b[:m], nil
}

// kernelName is the u-boot legacy uImage header of mtdblock4: magic 27051956 at offset 0, the image name at
// offset 32 (32 bytes), e.g. "Linux-4.4.302-cip94". Anything else leaves the kernel unknown.
func kernelName(h []byte) string {
	if len(h) < 4 || h[0] != 0x27 || h[1] != 0x05 || h[2] != 0x19 || h[3] != 0x56 {
		return ""
	}
	var b []byte
	if len(h) > 32 {
		end := min(len(h), 64)
		for _, c := range h[32:end] {
			switch {
			case c >= 'A' && c <= 'Z', c >= 'a' && c <= 'z', c >= '0' && c <= '9', c == '.', c == '_', c == '+', c == ' ', c == '-':
				b = append(b, c)
			}
		}
	}
	s := string(b)
	if rest, ok := strings.CutPrefix(s, "Linux-"); ok && rest != "" {
		return rest
	}
	return ""
}

// verdict is the hardware check of build_firmware.sh against supported_hardware.txt.
func verdict(hw string, sha, format string, s *Stock) (v, detail string) {
	v = "unknown"
	detail = "stock APP sha256 " + sha + " is not listed in supported_hardware.txt"
	for _, line := range strings.Split(hw, "\n") {
		f := strings.SplitN(line, "|", 8)
		st := f[0]
		if st == "" || strings.HasPrefix(st, "#") || strings.HasPrefix(st, "supported_hardware_format=") {
			continue
		}
		get := func(i int) string {
			if i < len(f) {
				return f[i]
			}
			return ""
		}
		dev, kern, sq, lsha := get(1), get(3), get(5), get(6)
		if lsha != sha || sha == "" {
			continue
		}
		if sq != format {
			detail = fmt.Sprintf("stock APP sha256 matches '%s' but its squashfs format differs (%s, listed: %s)", dev, format, sq)
			continue
		}
		if s.KernelExpected && s.Kernel == "" && kern != "-" {
			detail = fmt.Sprintf("stock APP matches '%s' but the kernel of the backup could not be read (mtdblock4.bin is not a uImage)", dev)
			continue
		}
		kfirst, _, _ := strings.Cut(kern, " ")
		if s.Kernel != "" && kern != "-" && kfirst != s.Kernel {
			detail = fmt.Sprintf("stock APP matches '%s' but the kernel differs (dump: %s, listed: %s)", dev, s.Kernel, kfirst)
			continue
		}
		return st, fmt.Sprintf("matches '%s'", dev)
	}
	return v, detail
}
