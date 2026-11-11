// Package squashfs is a pure-Go reader and writer (see writer.go) of squashfs 4.0 images (little-endian), written for the
// image builder of the assistant. The input is untrusted (it comes from the user's own backup of a camera):
// every count, size and offset is checked against a Limits value and against the image itself, loops are
// detected, and nothing is ever extracted to the host file system. Callers get an in-memory view: Lstat,
// ReadDir, Walk, Readlink, ReadFile, an io.Reader per file and a canonical listing.
//
// Supported: xz (CRC32, CRC64 or no check, no BCJ or other filters) and gzip (zlib) compression, any block
// size from 4 KiB to 1 MiB, fragments, the id table, the export (lookup) table, per-block "stored"
// metadata, data and fragment blocks, sparse blocks, and the basic and extended inode types for directory,
// regular file, symlink, block and character device, fifo and socket. Extended attributes are not
// interpreted: FileInfo.HasXattr and Info.HasXattrTable tell the caller they exist so that it can refuse
// them. lzma, lzo, lz4 and zstd images, and other superblock versions, are refused with ErrUnsupported.
//
// The directory index of extended directories is skipped, not used: directories are scanned linearly.
//
// The writer (Write, Build) turns an in-memory Node tree into an image laid out like mksquashfs's (xz with a
// CRC32 check, 128 KiB blocks, fragments, export table, no xattrs); hard links are refused.
package squashfs
