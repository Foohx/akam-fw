package appfs

import (
	"crypto/aes"
	"crypto/md5" //nolint:gosec // the vendor's partition sign is defined with md5, it is not a security hash
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
)

// PartitionSize is the size of the appfs partition (mtd3): the image is padded with 0x00 to it.
const PartitionSize = 0x490000

const (
	appfsGranularity = 0x4000
	squashMagic      = "hsqs"
)

// signKey is the vendor's fixed key ("yiyiot0518@_YYOS"), the same on every device (the recipe of
// calc_partition_sign.py, which this ports).
var signKey = []byte("yiyiot0518@_YYOS")

// Sign is the stock "appfs_sign" nvram value of an appfs image: hex(AES-128-ECB(key, md5(data))), data being the first
// bytes_used of the squashfs rounded up to 16 KiB. The camera's boot path does not check it (CRC-only bootm); it is
// computed so the optional nvram step and the manifest carry the value the stock updater would write.
func Sign(img []byte) (string, error) {
	if len(img) < 48 || string(img[:4]) != squashMagic {
		return "", errors.New("not a squashfs image (bad magic)")
	}
	used := binary.LittleEndian.Uint64(img[40:48])
	n := (used + appfsGranularity - 1) / appfsGranularity * appfsGranularity
	if n > uint64(len(img)) {
		return "", fmt.Errorf("sign length 0x%x exceeds the image size 0x%x", n, len(img))
	}
	sum := md5.Sum(img[:n]) //nolint:gosec
	c, err := aes.NewCipher(signKey)
	if err != nil {
		return "", err
	}
	var out [16]byte
	c.Encrypt(out[:], sum[:])
	return hex.EncodeToString(out[:]), nil
}
