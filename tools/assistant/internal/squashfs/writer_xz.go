package squashfs

import (
	"io"

	"github.com/ulikunitz/xz"
)

// newXZWriter returns an encoder of one xz stream with a single LZMA2 block and a CRC32 check. Both are set
// explicitly because the defaults are not what the camera kernel accepts: the default check is CRC64 and the
// default dictionary 8 MiB, while the kernel's xz decoder preallocates a dictionary of the squashfs block
// size (at least 8 KiB) and refuses a stream that declares a bigger one. The declared dictionary is
// lzma.EncodeDictCap(dict), the smallest encodable value not below dict: exactly dict for the powers of two
// the writer uses. The default HashTable4 match finder is used: BinaryTree is 10 to 100 times slower and, on
// repetitive data, produced much larger output with this library version (measured).
//
// Size against liblzma (xz -9e, same 128 KiB dictionary, CRC32), measured on 64 blocks of host ELF files:
// HashTable4 +10.4%, BinaryTree +62.7%; lc/lp/pb variants change less than 0.5% (lc=4 is the best, -0.2%),
// so the parameters stay at the defaults. The gap is the encoder of the library (greedy parsing, no optimal
// parsing like liblzma's): on binaries the writer's image is about 10% bigger than mksquashfs's, on text
// and the metadata blocks 5% to 15%. Nothing in the writer (stored blocks, fragments and metadata are all
// compressed) adds to it.
func newXZWriter(w io.Writer, dict int) (*xz.Writer, error) {
	return xz.WriterConfig{CheckSum: xz.CRC32, DictCap: dict}.NewWriter(w)
}
