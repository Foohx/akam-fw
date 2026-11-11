// Command mkarchive writes a deterministic .tar.gz or .zip (standard library only).
//
//	mkarchive -format tar.gz|zip -epoch SECONDS -o OUT -dir PREFIX NAME=FILE[:MODE] ...
//
// Every entry is stored under PREFIX/NAME in the order given, with a fixed
// modification time (-epoch, UTC), owner 0/0 with no user or group name, and a
// mode of 0755 or 0644 (MODE overrides, octal). Nothing of the build machine
// (user, host, path, clock) reaches the archive, so the same inputs always
// give the same bytes with the same Go release (compress/flate output may
// change between Go releases: the release script pins the Go version).
package main

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"flag"
	"fmt"
	"io"
	"os"
	"path"
	"strconv"
	"strings"
	"time"
)

type entry struct {
	name string // path inside the archive
	src  string
	mode os.FileMode
}

func main() {
	format := flag.String("format", "", "tar.gz or zip")
	epoch := flag.Int64("epoch", -1, "modification time of every entry (seconds since 1970, UTC)")
	out := flag.String("o", "", "output file")
	dir := flag.String("dir", "", "top-level directory inside the archive")
	flag.Parse()
	if err := run(*format, *epoch, *out, *dir, flag.Args()); err != nil {
		fmt.Fprintln(os.Stderr, "mkarchive:", err)
		os.Exit(1)
	}
}

func parseEntries(dir string, args []string) ([]entry, error) {
	var es []entry
	for _, a := range args {
		name, src, ok := strings.Cut(a, "=")
		if !ok || name == "" || src == "" || strings.ContainsAny(name, "\\") || path.IsAbs(name) || strings.Contains(name, "..") {
			return nil, fmt.Errorf("bad entry %q (want NAME=FILE[:MODE])", a)
		}
		mode := os.FileMode(0)
		if i := strings.LastIndex(src, ":"); i > 0 {
			if m, err := strconv.ParseUint(src[i+1:], 8, 32); err == nil {
				mode, src = os.FileMode(m), src[:i]
			}
		}
		if mode == 0 {
			fi, err := os.Stat(src)
			if err != nil {
				return nil, err
			}
			mode = 0o644
			if fi.Mode()&0o111 != 0 {
				mode = 0o755
			}
		}
		if dir != "" {
			name = path.Join(dir, name)
		}
		es = append(es, entry{name: name, src: src, mode: mode})
	}
	if len(es) == 0 {
		return nil, fmt.Errorf("no entry")
	}
	return es, nil
}

func run(format string, epoch int64, out, dir string, args []string) (err error) {
	if epoch < 0 || out == "" || (format != "tar.gz" && format != "zip") {
		return fmt.Errorf("usage: mkarchive -format tar.gz|zip -epoch SECONDS -o OUT [-dir PREFIX] NAME=FILE[:MODE] ...")
	}
	es, err := parseEntries(dir, args)
	if err != nil {
		return err
	}
	mtime := time.Unix(epoch, 0).UTC()
	f, err := os.Create(out)
	if err != nil {
		return err
	}
	defer func() {
		if cerr := f.Close(); err == nil {
			err = cerr
		}
		if err != nil {
			os.Remove(out)
		}
	}()
	if format == "zip" {
		return writeZip(f, es, mtime)
	}
	return writeTarGz(f, es, mtime)
}

func writeTarGz(w io.Writer, es []entry, mtime time.Time) error {
	gz, err := gzip.NewWriterLevel(w, gzip.BestCompression)
	if err != nil {
		return err
	}
	gz.Header = gzip.Header{OS: 3} // no name, no mtime; "Unix"
	tw := tar.NewWriter(gz)
	if dir := path.Dir(es[0].name); dir != "." {
		hdr := &tar.Header{Typeflag: tar.TypeDir, Name: dir + "/", Mode: 0o755, ModTime: mtime, Format: tar.FormatUSTAR}
		if err := tw.WriteHeader(hdr); err != nil {
			return err
		}
	}
	for _, e := range es {
		data, err := os.ReadFile(e.src)
		if err != nil {
			return err
		}
		hdr := &tar.Header{Typeflag: tar.TypeReg, Name: e.name, Mode: int64(e.mode.Perm()), Size: int64(len(data)),
			ModTime: mtime, Format: tar.FormatUSTAR}
		if err := tw.WriteHeader(hdr); err != nil {
			return err
		}
		if _, err := tw.Write(data); err != nil {
			return err
		}
	}
	if err := tw.Close(); err != nil {
		return err
	}
	return gz.Close()
}

func writeZip(w io.Writer, es []entry, mtime time.Time) error {
	zw := zip.NewWriter(w)
	for _, e := range es {
		data, err := os.ReadFile(e.src)
		if err != nil {
			return err
		}
		hdr := &zip.FileHeader{Name: e.name, Method: zip.Deflate, Modified: mtime}
		hdr.SetMode(e.mode.Perm())
		fw, err := zw.CreateHeader(hdr)
		if err != nil {
			return err
		}
		if _, err := fw.Write(data); err != nil {
			return err
		}
	}
	return zw.Close()
}
