package squashfs

// Limits are the caps applied to an untrusted image. A zero field takes the default of DefaultLimits.
type Limits struct {
	// MaxImageSize is the largest image accepted (the size given to Open). Default 5 MiB (the APP partition).
	MaxImageSize int64
	// MaxInodes caps the inode count of the superblock, the fragment count and the entries of a listing
	// (hard links make entries outnumber inodes: Walk allows twice this). Default 262144.
	MaxInodes uint32
	// MaxDirEntries caps the entries of one directory. Default 65536.
	MaxDirEntries int
	// MaxDirBytes caps the stored size of one directory. Default 8 MiB.
	MaxDirBytes int64
	// MaxDepth caps the path depth (components) of Lstat and Walk. Default 128.
	MaxDepth int
	// MaxFileSize caps the declared size of a regular file. Default 256 MiB.
	MaxFileSize int64
	// MaxCallBytes caps the bytes decompressed (zero blocks of sparse files included) by one ReadFile,
	// one OpenFile reader, or one Listing/Verify call. Default 512 MiB.
	MaxCallBytes int64
	// MaxMetadataBytes caps the decompressed metadata (inode, directory and table blocks) kept in memory
	// for one FS. Default 64 MiB.
	MaxMetadataBytes int64
	// MaxMetaChain caps the metadata blocks chained by one read (an inode, a directory). Default 4096.
	MaxMetaChain int
}

// DefaultLimits returns the limits used when Open gets none.
func DefaultLimits() Limits {
	return Limits{
		MaxImageSize:     5 << 20,
		MaxInodes:        1 << 18,
		MaxDirEntries:    1 << 16,
		MaxDirBytes:      8 << 20,
		MaxDepth:         128,
		MaxFileSize:      256 << 20,
		MaxCallBytes:     512 << 20,
		MaxMetadataBytes: 64 << 20,
		MaxMetaChain:     4096,
	}
}

func (l Limits) filled() Limits {
	d := DefaultLimits()
	if l.MaxImageSize > 0 {
		d.MaxImageSize = l.MaxImageSize
	}
	if l.MaxInodes > 0 {
		d.MaxInodes = l.MaxInodes
	}
	if l.MaxDirEntries > 0 {
		d.MaxDirEntries = l.MaxDirEntries
	}
	if l.MaxDirBytes > 0 {
		d.MaxDirBytes = l.MaxDirBytes
	}
	if l.MaxDepth > 0 {
		d.MaxDepth = l.MaxDepth
	}
	if l.MaxFileSize > 0 {
		d.MaxFileSize = l.MaxFileSize
	}
	if l.MaxCallBytes > 0 {
		d.MaxCallBytes = l.MaxCallBytes
	}
	if l.MaxMetadataBytes > 0 {
		d.MaxMetadataBytes = l.MaxMetadataBytes
	}
	if l.MaxMetaChain > 0 {
		d.MaxMetaChain = l.MaxMetaChain
	}
	return d
}

// budget counts the bytes decompressed during one call.
type budget struct{ left int64 }

func (b *budget) take(n int64) error {
	if n < 0 || n > b.left {
		return limited("more than the allowed decompressed bytes in one call")
	}
	b.left -= n
	return nil
}
