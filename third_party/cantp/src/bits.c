/* bits.c - DBC bit placement/extraction and raw<->physical conversion. No <math.h>. */
#include "cantp_internal.h"

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int isnan_d(double v) { return v != v; }

/* Round half away from zero. Input already range-limited to long long. */
static long long round_ll(double v)
{
    if (v >= 0.0) return (long long)(v + 0.5);
    return -(long long)(-v + 0.5);
}

static void set_bit(uint8_t* buf, int pos, int val)          /* writes the bit (set or clear) */
{
    if (val) buf[pos >> 3] |= (uint8_t)(1u << (pos & 7));
    else     buf[pos >> 3] &= (uint8_t)~(1u << (pos & 7));
}
static int get_bit(const uint8_t* buf, int pos)
{
    return (buf[pos >> 3] >> (pos & 7)) & 1;
}

/* Highest bit position a signal touches (Motorola walks down through bytes). */
int bits_last_pos(int start, int len, int motorola)
{
    if (!motorola) return start + len - 1;
    int byte = start >> 3, bit = start & 7;
    for (int j = 1; j < len; j++) { if (--bit < 0) { bit = 7; byte++; } }
    return byte * 8 + bit;
}

void bits_place(uint8_t* buf, int start, int len, int motorola, uint64_t raw)
{
    if (!motorola) {
        for (int i = 0; i < len; i++) set_bit(buf, start + i, (int)((raw >> i) & 1u));
    } else {
        int byte = start >> 3, bit = start & 7;
        for (int j = 0; j < len; j++) {                 /* j = 0 is the MSB */
            set_bit(buf, byte * 8 + bit, (int)((raw >> (len - 1 - j)) & 1u));
            if (--bit < 0) { bit = 7; byte++; }
        }
    }
}

uint64_t bits_extract(const uint8_t* buf, int start, int len, int motorola)
{
    uint64_t raw = 0;
    if (!motorola) {
        for (int i = 0; i < len; i++) if (get_bit(buf, start + i)) raw |= (1ULL << i);
    } else {
        int byte = start >> 3, bit = start & 7;
        for (int j = 0; j < len; j++) {
            raw = (raw << 1) | (uint64_t)get_bit(buf, byte * 8 + bit);
            if (--bit < 0) { bit = 7; byte++; }
        }
    }
    return raw;
}

int canfd_pad_len(int len)
{
    static const int valid[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
    for (unsigned i = 0; i < sizeof valid / sizeof valid[0]; i++) if (len <= valid[i]) return valid[i];
    return -1;
}

/* physical -> raw bits for one signal. NaN -> "not available" (all ones / NaN). */
int bits_encode(const CanTpSig* s, double phys, uint64_t* raw)
{
    int len = s->len;
    uint64_t mask = (len >= 64) ? ~0ULL : ((1ULL << len) - 1u);

    if (isnan_d(phys)) {
        if (s->type == 2)      { float f = (float)phys; uint32_t b; memcpy(&b, &f, 4); *raw = b; }
        else if (s->type == 3) { uint64_t b; memcpy(&b, &phys, 8); *raw = b; }
        else                   *raw = mask;
        return CANTP_OK;
    }
    if (s->max > s->min) phys = clampd(phys, s->min, s->max);
    double rawd = (phys - s->offset) / s->factor;

    switch (s->type) {
    case 0: {                                             /* unsigned */
        if (len >= 64) {
            rawd = clampd(rawd, 0.0, 1.8446744073709552e19);
            *raw = (rawd >= 1.8446744073709552e19) ? ~0ULL : (uint64_t)(rawd + 0.5);
        } else {
            rawd = clampd(rawd, 0.0, (double)mask);
            uint64_t r = (uint64_t)round_ll(rawd);
            *raw = r > mask ? mask : r;
        }
        break;
    }
    case 1: {                                             /* signed two's complement */
        double lim = (len >= 64) ? 9.2233720368547758e18 : (double)(1ULL << (len - 1));
        rawd = clampd(rawd, -lim, lim - 1.0);
        *raw = (uint64_t)round_ll(rawd) & mask;
        break;
    }
    case 2: { float f = (float)rawd; uint32_t b; memcpy(&b, &f, 4); *raw = b; break; }
    case 3: { memcpy(raw, &rawd, 8); break; }
    default: return CANTP_ERR_SIGDEF;
    }
    return CANTP_OK;
}

double bits_decode(const CanTpSig* s, uint64_t raw)
{
    double rawd;
    switch (s->type) {
    case 1: {
        if (s->len < 64 && (raw & (1ULL << (s->len - 1)))) raw |= ~((1ULL << s->len) - 1u);   /* sign-extend */
        rawd = (double)(long long)raw;
        break;
    }
    case 2: { uint32_t b = (uint32_t)raw; float f; memcpy(&f, &b, 4); rawd = f; break; }
    case 3: { memcpy(&rawd, &raw, 8); break; }
    default: rawd = (double)raw; break;
    }
    return rawd * s->factor + s->offset;
}
