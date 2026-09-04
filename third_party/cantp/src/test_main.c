/* test_main.c - CanTp unit tests, compiled together with the sources in ../src. */
#include "../src/cantp.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKF(a, b) CHECK(fabs((double)(a) - (double)(b)) < 1e-6)

static uint32_t rd_u32(const uint8_t* p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd_u64(const uint8_t* p) { return rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32); }

/* msgDef helper: id ext len transport sa da pad cycle */
static void msgdef(double* d, double id, double ext, double len, double tp, double sa, double da, double pad)
{
    d[0] = id; d[1] = ext; d[2] = len; d[3] = tp; d[4] = sa; d[5] = da; d[6] = pad; d[7] = 0;
}
/* sigDef helper: start len order type factor offset min max */
static void sigdef(double* r, double start, double len, double order, double type, double factor, double offset, double mn, double mx)
{
    r[0] = start; r[1] = len; r[2] = order; r[3] = type; r[4] = factor; r[5] = offset; r[6] = mn; r[7] = mx;
}

/* ----------------------------------------------------------------------- */
static void test_records(void)
{
    uint8_t rec[80], hdr[12];
    CHECK(CanTp_RecordSizeFor(0) == 24 && CanTp_RecordSizeFor(8) == 24 && CanTp_RecordSizeFor(9) == 32);
    CHECK(CanTp_RecordSizeFor(12) == 32 && CanTp_RecordSizeFor(16) == 32 && CanTp_RecordSizeFor(17) == 40);
    CHECK(CanTp_RecordSizeFor(64) == 80 && CanTp_RecordSizeFor(65) == CANTP_ERR_ARG);
    uint8_t d[64]; for (int i = 0; i < 64; i++) d[i] = (uint8_t)i;
    CHECK(CanTp_MakeRecord(0x123, 0, 0x00, d, 8, 42, rec, sizeof rec) == 24);
    CHECK(rd_u64(rec) == 42 && rd_u32(rec + 8) == 0x123 && rec[12] == 0 && rec[15] == 8 && rec[16] == 0 && rec[23] == 7);
    CHECK(CanTp_MakeRecord(0x18FF0080, 1, 0x00, d, 3, 0, rec, sizeof rec) == 24);
    CHECK(rd_u32(rec + 8) == (0x18FF0080u | CANTP_XNET_EXTENDED_ID_FLAG) && rec[15] == 3 && rec[19] == 0);
    CHECK(CanTp_MakeRecord(0x123, 0, 0x00, d, 9, 0, rec, sizeof rec) == CANTP_ERR_ARG);        /* classic > 8 */
    CHECK(CanTp_MakeRecord(0x123, 0, 0x10, d, 64, 0, rec, sizeof rec) == 80 && rec[12] == 0x10 && rec[15] == 64 && rec[79] == 63);
    CHECK(CanTp_MakeRecord(0x123, 0, 0x18, d, 12, 0, rec, sizeof rec) == 32 && rec[12] == 0x18 && rec[27] == 11 && rec[31] == 0);
    CHECK(CanTp_MakeRecord(0x123, 0, 0x10, d, 64, 0, rec, 79) == CANTP_ERR_BUFFER);
    CHECK(CanTp_MakeRecord(0x800, 0, 0x00, d, 1, 0, rec, sizeof rec) == CANTP_ERR_ARG);
    CHECK(CanTp_RecordSize(rec, 80) == 32 && CanTp_RecordSize(rec, 31) == CANTP_ERR_RECORD && CanTp_RecordSize(rec, 10) == CANTP_ERR_RECORD);
    CHECK(CanTp_NclHeader(hdr, 12) == CANTP_OK && hdr[0] == 0x4E && hdr[1] == 0x49 && hdr[8] == 1);
    CHECK(CanTp_NclHeader(hdr, 11) == CANTP_ERR_ARG);
    CHECK(CanTp_Version() == 0x010000);
}

/* ----------------------------------------------------------------------- */
static void test_define_errors(void)
{
    double m[8], s[2 * 8];
    msgdef(m, 0x123, 0, 8, 0, -1, 255, 0);
    CHECK(CanTp_Define(-1, m, 8, NULL, 0) == CANTP_ERR_SLOT);
    CHECK(CanTp_Define(CANTP_MAX_SLOTS, m, 8, NULL, 0) == CANTP_ERR_SLOT);
    CHECK(CanTp_Define(0, m, 7, NULL, 0) == CANTP_ERR_ARG);
    CHECK(CanTp_Define(0, NULL, 8, NULL, 0) == CANTP_ERR_ARG);
    CHECK(CanTp_Pack(5, NULL, 0, 0, 0, NULL, 0, NULL) == CANTP_ERR_ARG);
    int32_t n;
    CHECK(CanTp_Pack(5, NULL, 0, 0, 0, NULL, 0, &n) == CANTP_ERR_NOT_DEFINED);
    CHECK(CanTp_SignalCount(5) == CANTP_ERR_NOT_DEFINED && CanTp_SignalCount(99) == CANTP_ERR_SLOT);

    msgdef(m, 0x123, 0, 9, 0, -1, 255, 0);  CHECK(CanTp_Define(0, m, 8, NULL, 0) == CANTP_ERR_MSGDEF);   /* classic > 8 */
    msgdef(m, 0x123, 0, 65, 2, -1, 255, 0); CHECK(CanTp_Define(0, m, 8, NULL, 0) == CANTP_ERR_MSGDEF);   /* FD > 64 */
    msgdef(m, 0x123, 0, 20, 1, -1, 255, 0); CHECK(CanTp_Define(0, m, 8, NULL, 0) == CANTP_ERR_MSGDEF);   /* BAM needs 29-bit */
    msgdef(m, 0x18FF0080, 1, 20, 1, -1, 0x10, 255); CHECK(CanTp_Define(0, m, 8, NULL, 0) == CANTP_ERR_TRANSPORT); /* DA != global */
    msgdef(m, 0x18FF0080, 1, 20, 4, -1, 255, 255); CHECK(CanTp_Define(0, m, 8, NULL, 0) == CANTP_ERR_TRANSPORT);
    msgdef(m, 0x18FF0080, 1, 20, 5, -1, 255, 255); CHECK(CanTp_Define(0, m, 8, NULL, 0) == CANTP_ERR_TRANSPORT);
    msgdef(m, 0x18FF0080, 1, 20, 9, -1, 255, 255); CHECK(CanTp_Define(0, m, 8, NULL, 0) == CANTP_ERR_MSGDEF);
    msgdef(m, 0x123, 0, 8, 0, 5, 255, 0);   CHECK(CanTp_Define(0, m, 8, NULL, 0) == CANTP_ERR_MSGDEF);   /* SA on 11-bit */
    msgdef(m, 0x800, 0, 8, 0, -1, 255, 0);  CHECK(CanTp_Define(0, m, 8, NULL, 0) == CANTP_ERR_MSGDEF);
    msgdef(m, 0x123, 0, 8, 0, -1, 256, 0);  CHECK(CanTp_Define(0, m, 8, NULL, 0) == CANTP_ERR_MSGDEF);

    msgdef(m, 0x123, 0, 8, 0, -1, 255, 0);
    sigdef(s, 60, 8, 0, 0, 1, 0, 0, 0);     CHECK(CanTp_Define(0, m, 8, s, 1) == CANTP_ERR_SIGDEF);       /* Intel past end */
    sigdef(s, 63, 16, 1, 0, 1, 0, 0, 0);    CHECK(CanTp_Define(0, m, 8, s, 1) == CANTP_ERR_SIGDEF);       /* Motorola spills */
    sigdef(s, 0, 8, 0, 0, 0, 0, 0, 0);      CHECK(CanTp_Define(0, m, 8, s, 1) == CANTP_ERR_SIGDEF);       /* factor 0 */
    sigdef(s, 0, 16, 0, 2, 1, 0, 0, 0);     CHECK(CanTp_Define(0, m, 8, s, 1) == CANTP_ERR_SIGDEF);       /* float32 not 32 */
    sigdef(s, 0, 0, 0, 0, 1, 0, 0, 0);      CHECK(CanTp_Define(0, m, 8, s, 1) == CANTP_ERR_SIGDEF);
    sigdef(s, 0, 8, 0, 7, 1, 0, 0, 0);      CHECK(CanTp_Define(0, m, 8, s, 1) == CANTP_ERR_SIGDEF);
    CHECK(CanTp_SignalCount(0) == CANTP_ERR_NOT_DEFINED);                                              /* failed define leaves slot empty */
    sigdef(s, 0, 8, 0, 0, 1, 0, 0, 0);      CHECK(CanTp_Define(0, m, 8, s, 1) == CANTP_OK);
    CHECK(CanTp_SignalCount(0) == 1 && CanTp_PayloadLength(0) == 8 && CanTp_FrameCount(0) == 1 && CanTp_OutputSize(0) == 24);
    CHECK(CanTp_Clear(0) == CANTP_OK && CanTp_SignalCount(0) == CANTP_ERR_NOT_DEFINED);
    CHECK(CanTp_Clear(-1) == CANTP_ERR_SLOT);
}

/* ----------------------------------------------------------------------- */
static void test_classic_pack_unpack(void)
{
    /* EEC1-like: 8 bytes, Intel + a Motorola signal, signed, clamped, float */
    double m[8], s[6 * 8];
    msgdef(m, 0x0CF00400 | 0xFE, 1, 8, 0, 0x00, 255, 255);        /* SA override 0x00 replaces the 0xFE placeholder */
    sigdef(s + 0,  0, 4, 0, 0, 1, 0, 0, 15);                        /* nibble */
    sigdef(s + 8, 16, 8, 0, 0, 1, -125, -125, 125);                 /* percent torque, offset */
    sigdef(s + 16, 24, 16, 0, 0, 0.125, 0, 0, 8031.875);            /* engine speed */
    sigdef(s + 24, 40, 8, 0, 1, 1, 0, 0, 0);                        /* signed byte, no clamp */
    sigdef(s + 32, 55, 12, 1, 0, 0.1, -200, -200, 200);             /* Motorola 12-bit, start 55 -> bytes 6,7 */
    sigdef(s + 40, 4, 4, 0, 0, 1, 0, 0, 0);                         /* high nibble of byte 0 */
    CHECK(CanTp_Define(1, m, 8, s, 6) == CANTP_OK);
    CHECK(CanTp_OutputSize(1) == 24);

    double v[6] = { 3, 42, 1500.125, -5, -17.5, 0xA };
    uint8_t out[24]; int32_t n = 0;
    CHECK(CanTp_Pack(1, v, 6, 7, 0, out, 10, &n) == CANTP_ERR_BUFFER && n == 24);
    CHECK(CanTp_Pack(1, v, 5, 7, 0, out, 24, &n) == CANTP_ERR_ARG);                /* too few values */
    CHECK(CanTp_Pack(1, v, 6, 7, 0, out, 24, &n) == CANTP_OK && n == 24);
    CHECK(rd_u64(out) == 7);
    CHECK(rd_u32(out + 8) == (0x0CF00400u | CANTP_XNET_EXTENDED_ID_FLAG));         /* SA replaced by override */
    CHECK(out[12] == 0 && out[15] == 8);
    const uint8_t* d = out + 16;
    CHECK(d[0] == 0xA3);                         /* nibble 3 low, 0xA high */
    CHECK(d[1] == 0xFF);                         /* untouched byte = pad */
    CHECK(d[2] == 167);                          /* 42 + 125 */
    CHECK(d[3] == 0xE1 && d[4] == 0x2E);         /* 1500.125 / 0.125 = 12001 = 0x2EE1 */
    CHECK(d[5] == 0xFB);                         /* -5 */
    /* -17.5 -> raw -175 -> 12-bit unsigned clamp: (phys clamped to [-200,200]) raw = (-17.5+200)/0.1 = 1825 = 0x721
       Motorola start 55 (byte6 bit7) len 12: byte6 = 0x72, byte7 high nibble = 0x1 */
    CHECK(d[6] == 0x72 && (d[7] >> 4) == 0x1 && (d[7] & 0x0F) == 0x0F);   /* low nibble of byte 7 = pad */

    /* unpack round trip */
    double u[6] = { 0 }; int32_t consumed = 0;
    CHECK(CanTp_Unpack(1, out, 24, u, 6, &consumed) == CANTP_FOUND && consumed == 24);
    CHECKF(u[0], 3); CHECKF(u[1], 42); CHECKF(u[2], 1500.125); CHECKF(u[3], -5); CHECKF(u[4], -17.5); CHECKF(u[5], 10);
    float uf[6]; CHECK(CanTp_UnpackSgl(1, out, 24, uf, 6, &consumed) == CANTP_FOUND && uf[2] == 1500.125f);

    /* clamp + saturation + NaN */
    double v2[6] = { 99, 500, -10, 300, 999, NAN };
    CHECK(CanTp_Pack(1, v2, 6, 0, 0, out, 24, &n) == CANTP_OK);
    CHECK(d[0] == 0xFF);                         /* 99 -> 15 (max), NaN -> all ones */
    CHECK(d[2] == 250 && d[3] == 0 && d[4] == 0); /* 500 -> 125 max; -10 -> 0 min */
    CHECK(d[5] == 0x7F);                         /* 300 saturates signed 8 to 127 */
    CHECK(d[6] == 0xFA && (d[7] >> 4) == 0x0);   /* 999 -> 200 -> raw 4000 = 0xFA0 */

    /* SGL variant identical */
    float vf[6] = { 3, 42, 1500.125f, -5, -17.5f, 10 };
    uint8_t out2[24];
    CHECK(CanTp_PackSgl(1, vf, 6, 7, 0, out2, 24, &n) == CANTP_OK && memcmp(out, out2, 24) != 0);
    CHECK(CanTp_Pack(1, v, 6, 7, 0, out, 24, &n) == CANTP_OK && memcmp(out, out2, 24) == 0);

    /* unpack ignores other ids, finds ours later in the buffer, resumes after consumed */
    uint8_t buf[24 * 4]; uint8_t junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CanTp_MakeRecord(0x0CF00401, 1, 0, junk, 8, 0, buf, 24);            /* same PGN, other SA -> not ours (override set) */
    CanTp_MakeRecord(0x123, 0, 0, junk, 8, 0, buf + 24, 24);
    memcpy(buf + 48, out, 24);
    memcpy(buf + 72, out, 24);
    CHECK(CanTp_Unpack(1, buf, 96, u, 6, &consumed) == CANTP_FOUND && consumed == 72);
    CHECK(CanTp_Unpack(1, buf + consumed, 96 - consumed, u, 6, &consumed) == CANTP_FOUND && consumed == 24);
    CHECK(CanTp_Unpack(1, buf, 48, u, 6, &consumed) == CANTP_OK && consumed == 48);
    buf[15] = 70;                                                        /* corrupt payload length */
    CHECK(CanTp_Unpack(1, buf, 96, u, 6, &consumed) == CANTP_ERR_RECORD);

    /* 11-bit message, exact id match, short record rejected */
    double m2[8], s2[8];
    msgdef(m2, 0x321, 0, 4, 0, -1, 255, 0);
    sigdef(s2, 8, 16, 0, 1, 0.01, 0, 0, 0);
    CHECK(CanTp_Define(2, m2, 8, s2, 1) == CANTP_OK && CanTp_OutputSize(2) == 24);
    double v3[1] = { -12.34 };
    CHECK(CanTp_Pack(2, v3, 1, 0, 0, out, 24, &n) == CANTP_OK && out[15] == 4 && rd_u32(out + 8) == 0x321);
    CHECK(out[16] == 0 && out[19] == 0);                                 /* pad 0 */
    CHECK(CanTp_Unpack(2, out, 24, u, 1, &consumed) == CANTP_FOUND); CHECKF(u[0], -12.34);
    CanTp_MakeRecord(0x321, 0, 0, junk, 2, 0, buf, 24);
    CHECK(CanTp_Unpack(2, buf, 24, u, 1, &consumed) == CANTP_OK);       /* too short */
    CanTp_MakeRecord(0x321, 1, 0, junk, 8, 0, buf, 24);
    CHECK(CanTp_Unpack(2, buf, 24, u, 1, &consumed) == CANTP_OK);       /* extended flag mismatch */
}

/* ----------------------------------------------------------------------- */
static void test_canfd(void)
{
    double m[8], s[3 * 8];
    msgdef(m, 0x1ABCDEF0, 1, 20, 2, -1, 255, 0xCC);
    sigdef(s + 0, 0, 32, 0, 2, 1, 0, 0, 0);                 /* float32 at 0 */
    sigdef(s + 8, 64, 64, 0, 3, 1, 0, 0, 0);                /* float64 at byte 8 */
    sigdef(s + 16, 159, 24, 1, 0, 1, 0, 0, 0);              /* Motorola 24-bit at start 159 -> bytes 19,18? no: bytes 19..17 walking down; 159=byte19 bit7 -> bytes 19,18,17 -> wait sawtooth goes byte+1: 19,20,21 -> spills; use start 135 */
    s[16] = 135;                                             /* byte16 bit7 -> bytes 16,17,18 */
    CHECK(CanTp_Define(3, m, 8, s, 3) == CANTP_OK);
    CHECK(CanTp_FrameCount(3) == 1 && CanTp_OutputSize(3) == 40);      /* 20 is a valid FD length; record 24 + 16 */
    double v[3] = { 1.5, -2.25e10, 0x123456 };
    uint8_t out[80]; int32_t n;
    CHECK(CanTp_Pack(3, v, 3, 0, 0, out, 80, &n) == CANTP_OK && n == 40);
    CHECK(out[12] == 0x10 && out[15] == 20);
    float f; memcpy(&f, out + 16, 4); CHECK(f == 1.5f);
    double dd; memcpy(&dd, out + 24, 8); CHECK(dd == -2.25e10);
    CHECK(out[16 + 16] == 0x12 && out[16 + 17] == 0x34 && out[16 + 18] == 0x56);
    CHECK(out[16 + 4] == 0xCC && out[16 + 19] == 0xCC);                 /* pad byte */
    double u[3]; int32_t c;
    CHECK(CanTp_Unpack(3, out, 40, u, 3, &c) == CANTP_FOUND && u[0] == 1.5 && u[1] == -2.25e10 && u[2] == 0x123456);

    /* length 13 pads to 16 bytes; BRS type */
    msgdef(m, 0x7FF, 0, 13, 3, -1, 255, 0);
    CHECK(CanTp_Define(4, m, 8, NULL, 0) == CANTP_OK && CanTp_OutputSize(4) == 32);
    CHECK(CanTp_Pack(4, NULL, 0, 0, 0, out, 80, &n) == CANTP_OK && out[12] == 0x18 && out[15] == 16);
    msgdef(m, 0x7FF, 0, 49, 2, -1, 255, 0);
    CHECK(CanTp_Define(4, m, 8, NULL, 0) == CANTP_OK && CanTp_OutputSize(4) == 80);
    CHECK(CanTp_Pack(4, NULL, 0, 0, 0, out, 80, &n) == CANTP_OK && out[15] == 64);
}

/* ----------------------------------------------------------------------- */
static void test_bam(void)
{
    /* 44-byte message: 11 float32 signals (the v1.0 temp-controller layout) with SA placeholder */
    double m[8], s[11 * 8];
    msgdef(m, 0x98FF00FE, 1, 44, 1, -1, 255, 255);          /* DBC-style id with bit 31 set, SA 0xFE placeholder */
    for (int i = 0; i < 11; i++) sigdef(s + 8 * i, 32 * i, 32, 0, 2, 1, 0, 0, 0);
    CHECK(CanTp_Define(6, m, 8, s, 11) == CANTP_OK);
    CHECK(CanTp_FrameCount(6) == 8 && CanTp_OutputSize(6) == 192);

    double v[11] = { 100, 0, 60, 40, 50, 23.5, 1000, 500, 0, 1, 0 };
    uint8_t out[192]; int32_t n;
    uint64_t t0 = 133700000000000000ULL, sp = 500000ULL;
    CHECK(CanTp_Pack(6, v, 11, t0, sp, out, 100, &n) == CANTP_ERR_BUFFER && n == 192);
    CHECK(CanTp_Pack(6, v, 11, t0, sp, out, 192, &n) == CANTP_OK && n == 192);
    const uint8_t* f = out;
    CHECK(rd_u64(f) == t0);
    CHECK(rd_u32(f + 8) == (0x1CECFFFEu | CANTP_XNET_EXTENDED_ID_FLAG));     /* TP.CM prio 7, DA FF, SA FE */
    CHECK(f[16] == 0x20 && f[17] == 44 && f[18] == 0 && f[19] == 7 && f[20] == 0xFF && f[21] == 0x00 && f[22] == 0xFF && f[23] == 0x00);
    uint8_t asm_[49] = { 0 };
    for (int i = 0; i < 7; i++) {
        f = out + 24 * (i + 1);
        CHECK(rd_u64(f) == t0 + (uint64_t)(i + 1) * sp);
        CHECK(rd_u32(f + 8) == (0x1CEBFFFEu | CANTP_XNET_EXTENDED_ID_FLAG));
        CHECK(f[16] == i + 1);
        memcpy(asm_ + 7 * i, f + 17, 7);
    }
    float fv[11]; memcpy(fv, asm_, 44);
    CHECK(fv[5] == 23.5f && fv[9] == 1.0f && asm_[44] == 0xFF && asm_[48] == 0xFF);

    /* stateless unpack of the whole sequence */
    double u[11]; int32_t c;
    CHECK(CanTp_Unpack(6, out, 192, u, 11, &c) == CANTP_FOUND && c == 192 && u[5] == 23.5 && u[6] == 1000);
    /* partial buffer: no message yet */
    CHECK(CanTp_Unpack(6, out, 150, u, 11, &c) == CANTP_OK && c == 144);
    /* SA placeholder: a sender with SA 0x80 is accepted */
    uint8_t out2[192]; memcpy(out2, out, 192);
    for (int i = 0; i < 8; i++) out2[24 * i + 8] = 0x80;
    CHECK(CanTp_Unpack(6, out2, 192, u, 11, &c) == CANTP_FOUND && u[0] == 100);
    /* with an SA override only that sender is accepted */
    m[4] = 0x80;
    CHECK(CanTp_Define(7, m, 8, s, 11) == CANTP_OK);
    CHECK(CanTp_Unpack(7, out2, 192, u, 11, &c) == CANTP_FOUND);
    CHECK(CanTp_Unpack(7, out, 192, u, 11, &c) == CANTP_OK);
    uint8_t out3[192]; CHECK(CanTp_Pack(7, v, 11, 0, 0, out3, 192, &n) == CANTP_OK && out3[8] == 0x80 && out3[24 + 8] == 0x80);

    /* live feed: interleave other traffic, lose a packet, restart */
    memset(u, 0, sizeof u);
    uint8_t junk[24]; uint8_t jd[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
    CanTp_MakeRecord(0x0CF00400, 1, 0, jd, 8, 0, junk, 24);
    CHECK(CanTp_RxReset(6) == CANTP_OK);
    for (int i = 0; i < 7; i++) {
        CHECK(CanTp_RxFeed(6, out + 24 * i, 24, u, 11) == CANTP_OK);
        CHECK(CanTp_RxFeed(6, junk, 24, u, 11) == CANTP_OK);
    }
    CHECK(CanTp_RxFeed(6, out + 24 * 7, 24, u, 11) == CANTP_FOUND && u[5] == 23.5);
    /* lost packet 3: sequence abandoned, later complete sequence still decodes */
    CanTp_RxFeed(6, out, 24, u, 11);
    CanTp_RxFeed(6, out + 24, 24, u, 11);
    CanTp_RxFeed(6, out + 24 * 2, 24, u, 11);
    CanTp_RxFeed(6, out + 24 * 4, 24, u, 11);                 /* seq 4 after 2 */
    CHECK(CanTp_RxFeed(6, out + 24 * 7, 24, u, 11) == CANTP_OK);
    u[5] = 0;
    for (int i = 0; i < 7; i++) CanTp_RxFeed(6, out + 24 * i, 24, u, 11);
    CHECK(CanTp_RxFeed(6, out + 24 * 7, 24, u, 11) == CANTP_FOUND && u[5] == 23.5);
    /* stateless Unpack must not disturb a live session */
    CanTp_RxFeed(6, out, 24, u, 11); CanTp_RxFeed(6, out + 24, 24, u, 11);
    CHECK(CanTp_Unpack(6, out, 192, u, 11, &c) == CANTP_FOUND);
    for (int i = 2; i < 7; i++) CanTp_RxFeed(6, out + 24 * i, 24, u, 11);
    u[5] = 0; CHECK(CanTp_RxFeed(6, out + 24 * 7, 24, u, 11) == CANTP_FOUND && u[5] == 23.5);
    CHECK(CanTp_RxFeed(6, out, 5, u, 11) == CANTP_ERR_RECORD);

    /* short BAM (<= 8 bytes) is a single frame under the PGN; PDU1 PGN gets DA */
    double m2[8], s2[8];
    msgdef(m2, 0x18EA00FE, 1, 3, 1, 0x21, 255, 255);        /* PDU1: PF EA */
    sigdef(s2, 0, 24, 0, 0, 1, 0, 0, 0);
    CHECK(CanTp_Define(8, m2, 8, s2, 1) == CANTP_OK && CanTp_FrameCount(8) == 1);
    double pg[1] = { 0xFEE3 };
    CHECK(CanTp_Pack(8, pg, 1, 0, 0, out, 24, &n) == CANTP_OK && n == 24);
    CHECK(rd_u32(out + 8) == (0x18EAFF21u | CANTP_XNET_EXTENDED_ID_FLAG) && out[15] == 3 && out[16] == 0xE3 && out[17] == 0xFE);
    CHECK(CanTp_Unpack(8, out, 24, u, 1, &c) == CANTP_FOUND && u[0] == 0xFEE3);

    /* maximum length */
    msgdef(m2, 0x18FF1000, 1, 1785, 1, -1, 255, 255);
    sigdef(s2, 1785 * 8 - 8, 8, 0, 0, 1, 0, 0, 0);
    CHECK(CanTp_Define(9, m2, 8, s2, 1) == CANTP_OK && CanTp_FrameCount(9) == 256 && CanTp_OutputSize(9) == 256 * 24);
    static uint8_t big[256 * 24];
    double last[1] = { 0x5A };
    CHECK(CanTp_Pack(9, last, 1, 0, 0, big, sizeof big, &n) == CANTP_OK && big[19] == 255);
    CHECK(CanTp_Unpack(9, big, sizeof big, u, 1, &c) == CANTP_FOUND && u[0] == 0x5A && c == 256 * 24);
    s2[0] = 1785 * 8;
    CHECK(CanTp_Define(9, m2, 8, s2, 1) == CANTP_ERR_SIGDEF);
    msgdef(m2, 0x18FF1000, 1, 1786, 1, -1, 255, 255);
    CHECK(CanTp_Define(9, m2, 8, NULL, 0) == CANTP_ERR_MSGDEF);
}

/* ----------------------------------------------------------------------- */
static void test_ec1_like(void)
{
    /* EC1 from J1939_NGHD_V130.dbc: BO_ 2566841342 EC1: 40, a few of its signals */
    double m[8], s[4 * 8];
    msgdef(m, 2566841342.0, 1, 40, 1, 0x00, 255, 255);
    sigdef(s + 0, 312, 8, 0, 0, 1, -125, -125, 125);       /* EngineDefaultIdleTorqueLimit */
    sigdef(s + 8, 256, 16, 0, 0, 1, 0, 0, 64255);          /* EngDefaultTorqueLimit */
    sigdef(s + 16, 240, 16, 0, 0, 0.004, 0, 0, 257.02);    /* EngMomentOfInertia */
    sigdef(s + 24, 152, 16, 0, 0, 1, 0, 0, 64255);         /* EngReferenceTorque */
    CHECK(CanTp_Define(10, m, 8, s, 4) == CANTP_OK);
    CHECK(CanTp_FrameCount(10) == 7 && CanTp_OutputSize(10) == 168);     /* 1 CM + 6 DT (42 bytes) */
    double v[4] = { 10, 2500, 3.2, 2779 };
    uint8_t out[168]; int32_t n;
    CHECK(CanTp_Pack(10, v, 4, 0, 0, out, 168, &n) == CANTP_OK);
    CHECK(rd_u32(out + 8) == (0x1CECFF00u | CANTP_XNET_EXTENDED_ID_FLAG));
    CHECK(out[16] == 0x20 && out[17] == 40 && out[19] == 6 && out[21] == 0xE3 && out[22] == 0xFE && out[23] == 0x00);
    /* reassemble by hand and check byte 39 (start 312) = 135, bytes 32..33 = 2500 */
    uint8_t asm_[42];
    for (int i = 0; i < 6; i++) memcpy(asm_ + 7 * i, out + 24 * (i + 1) + 17, 7);
    CHECK(asm_[39] == 135 && asm_[32] == (2500 & 0xFF) && asm_[33] == (2500 >> 8));
    CHECK(asm_[30] == (800 & 0xFF) && asm_[31] == (800 >> 8));          /* 3.2 / 0.004 */
    CHECK(asm_[19] == (2779 & 0xFF) && asm_[20] == (2779 >> 8));
    CHECK(asm_[0] == 0xFF && asm_[40] == 0xFF && asm_[41] == 0xFF);       /* pad incl. last packet */
    double u[4]; int32_t c;
    CHECK(CanTp_Unpack(10, out, 168, u, 4, &c) == CANTP_FOUND);
    CHECKF(u[0], 10); CHECKF(u[1], 2500); CHECKF(u[2], 3.2); CHECKF(u[3], 2779);
}

int main(void)
{
    test_records();
    test_define_errors();
    test_classic_pack_unpack();
    test_canfd();
    test_bam();
    test_ec1_like();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
