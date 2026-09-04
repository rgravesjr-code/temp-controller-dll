/* canpack.c - generic DBC-style signal packer into NI-XNET raw frames. */
#include "xnet_raw.h"

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Round half away from zero without <math.h>. Input already range-limited. */
static long long round_ll(double v)
{
    if (v >= 0.0) return (long long)(v + 0.5);
    return -(long long)(-v + 0.5);
}

static void set_bit(uint8_t* buf, int pos, int val)
{
    if (val) buf[pos >> 3] |= (uint8_t)(1u << (pos & 7));
}

/* Place `len` raw bits into buf using DBC start-bit semantics. */
static void place_bits(uint8_t* buf, int startBit, int len, int motorola, uint64_t raw)
{
    if (!motorola) {
        for (int i = 0; i < len; i++)
            set_bit(buf, startBit + i, (int)((raw >> i) & 1u));
    } else {
        int byte = startBit >> 3, bit = startBit & 7;
        for (int j = 0; j < len; j++) {                 /* j = 0 is the MSB */
            set_bit(buf, byte * 8 + bit, (int)((raw >> (len - 1 - j)) & 1u));
            if (--bit < 0) { bit = 7; byte++; }
        }
    }
}

/* Highest frame bit position touched by a signal (for the DLC check). */
static int last_bit_pos(int startBit, int len, int motorola)
{
    if (!motorola) return startBit + len - 1;
    int byte = startBit >> 3, bit = startBit & 7;
    for (int j = 1; j < len; j++) { if (--bit < 0) { bit = 7; byte++; } }
    return byte * 8 + bit;
}

TC_API int32_t TcCanPack(const double* sigDefs, int32_t nSig,
                         const double* frameDefs, int32_t nFrames,
                         const float* values, int32_t nValues,
                         uint64_t timestamp100ns,
                         uint8_t* out, int32_t outLen, int32_t* bytesWritten)
{
    if (bytesWritten) *bytesWritten = 0;
    if (!sigDefs || !frameDefs || !values || !bytesWritten) return TC_ERR_ARG;
    if (nSig < 0 || nFrames <= 0 || nValues < nSig) return TC_ERR_ARG;

    int32_t need = nFrames * TC_RAW_FRAME_SIZE;
    if (!out || outLen < need) { *bytesWritten = need; return TC_ERR_BUFFER; }

    /* validate frames */
    for (int32_t f = 0; f < nFrames; f++) {
        const double* fd = frameDefs + f * TC_FRAMEDEF_COLS;
        int dlc = (int)fd[2];
        int ext = fd[1] != 0.0;
        if (dlc < 0 || dlc > 8) return TC_ERR_FRAMEDEF;
        if (fd[0] < 0 || fd[0] > (ext ? 0x1FFFFFFF : 0x7FF)) return TC_ERR_FRAMEDEF;
    }

    memset(out, 0, (size_t)need);

    /* pack signals into a scratch payload per frame */
    uint8_t payload[8 * 256];
    if (nFrames > 256) return TC_ERR_FRAMEDEF;
    memset(payload, 0, (size_t)nFrames * 8);

    for (int32_t s = 0; s < nSig; s++) {
        const double* sd = sigDefs + s * TC_SIGDEF_COLS;
        int f = (int)sd[0], start = (int)sd[1], len = (int)sd[2];
        int motorola = sd[3] != 0.0, vtype = (int)sd[4];
        double factor = sd[5], offset = sd[6], mn = sd[7], mx = sd[8];
        if (f < 0 || f >= nFrames) return TC_ERR_SIGDEF;
        if (len < 1 || len > 64 || start < 0 || start > 63) return TC_ERR_SIGDEF;
        if (factor == 0.0) return TC_ERR_SIGDEF;
        int dlc = (int)frameDefs[f * TC_FRAMEDEF_COLS + 2];
        if (last_bit_pos(start, len, motorola) >= dlc * 8 || (!motorola && start + len > 64))
            return TC_ERR_SIGDEF;

        double phys = (double)values[s];
        if (mx > mn) phys = clampd(phys, mn, mx);
        double rawd = (phys - offset) / factor;
        uint64_t raw;

        switch (vtype) {
        case 0: {                                        /* unsigned */
            double maxv = (len >= 64) ? 1.8446744073709552e19 : (double)((1ULL << len) - 1u);
            rawd = clampd(rawd, 0.0, maxv);
            if (len >= 64) {
                raw = (rawd >= 1.8446744073709552e19) ? ~0ULL : (uint64_t)(rawd + 0.5);
            } else {
                raw = (uint64_t)round_ll(rawd);
                if (raw > ((1ULL << len) - 1u)) raw = (1ULL << len) - 1u;
            }
            break;
        }
        case 1: {                                        /* signed two's complement */
            double lim = (len >= 64) ? 9.2233720368547758e18 : (double)(1ULL << (len - 1));
            rawd = clampd(rawd, -lim, lim - 1.0);
            long long v = round_ll(rawd);
            raw = (uint64_t)v;
            break;
        }
        case 2: {                                        /* IEEE float32 */
            if (len != 32) return TC_ERR_SIGDEF;
            float fv = (float)rawd; uint32_t b; memcpy(&b, &fv, 4); raw = b;
            break;
        }
        case 3: {                                        /* IEEE float64 */
            if (len != 64) return TC_ERR_SIGDEF;
            memcpy(&raw, &rawd, 8);
            break;
        }
        default:
            return TC_ERR_SIGDEF;
        }
        if (len < 64) raw &= (1ULL << len) - 1u;
        place_bits(payload + f * 8, start, len, motorola, raw);
    }

    for (int32_t f = 0; f < nFrames; f++) {
        const double* fd = frameDefs + f * TC_FRAMEDEF_COLS;
        xnet_raw_frame(out + f * TC_RAW_FRAME_SIZE, timestamp100ns,
                       (uint32_t)fd[0], fd[1] != 0.0, payload + f * 8, (int)fd[2]);
    }
    *bytesWritten = need;
    return TC_OK;
}
