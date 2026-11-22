#include "h264_nalu.h"

int h264_annexb_each(const uint8_t *data, size_t len, h264_nalu_fn fn, void *user)
{
    size_t i = 0;
    while (i + 3 < len) {
        size_t sc = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            sc = 4;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            sc = 3;
        if (sc > 0) {
            size_t start = i + sc, next = len, j = start;
            size_t nlen;
            while (j + 2 < len) {
                if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1) {
                    next = (j > 0 && data[j - 1] == 0) ? j - 1 : j;
                    break;
                }
                j++;
            }
            nlen = next - start;
            if (nlen > 0) {
                unsigned t = data[start] & 0x1f;
                int r = fn(user, data + start, nlen, t >= 1 && t <= 5);
                if (r != 0)
                    return r;
            }
            i = next;
        } else {
            i++;
        }
    }
    return 0;
}
