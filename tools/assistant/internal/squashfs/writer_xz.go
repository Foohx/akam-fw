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
func newXZWriter(w io.Writer, dict int) (*xz.Writer, error) {
	return xz.WriterConfig{CheckSum: xz.CRC32, DictCap: dict}.NewWriter(w)
}
