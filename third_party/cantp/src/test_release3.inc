/* test_release3.inc - v1.2.0: CanTp_DefineFlat / FlatSize / GetDef / Defaults,
 * CanTp_FrameLengths, CanTp_Transfer. Included by test_main.c. */
#include "flat_fixtures.h"

/* ---- tiny big-endian LabVIEW flattener for synthetic clusters ------------ */
typedef struct { uint8_t* p; int n, cap; } FlatWr;
static void fw_u8(FlatWr* w, unsigned v)  { if (w->n < w->cap) w->p[w->n] = (uint8_t)v; w->n++; }
static void fw_u16(FlatWr* w, unsigned v) { fw_u8(w, v >> 8); fw_u8(w, v & 0xFF); }
static void fw_u32(FlatWr* w, uint32_t v) { fw_u16(w, v >> 16); fw_u16(w, v & 0xFFFF); }
static void fw_f64(FlatWr* w, double d)   { uint64_t b; memcpy(&b, &d, 8); fw_u32(w, (uint32_t)(b >> 32)); fw_u32(w, (uint32_t)b); }
static void fw_str(FlatWr* w, const char* s) { uint32_t n = (uint32_t)strlen(s); fw_u32(w, n); for (uint32_t i = 0; i < n; i++) fw_u8(w, (uint8_t)s[i]); }
typedef struct { int start, bits, dtype, order; double sf, off, mn, mx, dflt; int nLut; } FCh;
static void fw_channel(FlatWr* w, const FCh* c)
{
    fw_str(w, "ch"); fw_u32(w, (uint32_t)c->start); fw_u32(w, (uint32_t)c->bits); fw_u16(w, (unsigned)c->dtype); fw_u16(w, (unsigned)c->order);
    fw_f64(w, c->sf); fw_f64(w, c->off); fw_f64(w, c->mn); fw_f64(w, c->mx); fw_f64(w, c->dflt);
    fw_str(w, "unit"); fw_u16(w, c->nLut ? 1 : 0); fw_u32(w, (uint32_t)c->nLut);
    for (int k = 0; k < c->nLut; k++) { fw_f64(w, k); fw_str(w, "S"); fw_str(w, "A"); fw_str(w, "lookup entry description"); }
    fw_str(w, "channel description");
}
/* returns the flattened size (may exceed cap: the caller checks) */
static int fw_message(uint8_t* buf, int cap, uint32_t id, int ext, int numBytes, double rate, const FCh* ch, int nCh)
{
    FlatWr w = { buf, 0, cap };
    fw_str(&w, "MSG"); fw_u32(&w, id); fw_u32(&w, (id >> 8) & 0x3FFFF); fw_u8(&w, ext ? 1 : 0); fw_u32(&w, (uint32_t)numBytes);
    fw_str(&w, "description"); fw_f64(&w, rate); fw_f64(&w, 1.0); fw_u32(&w, (uint32_t)nCh);
    for (int i = 0; i < nCh; i++) fw_channel(&w, &ch[i]);
    fw_u8(&w, 0);
    return w.n;
}

static void test_define_flat_real(void)
{
    double md[8], sd[8 * 8], dflt[8];
    /* ZNVW: 18FD67FE, 8 bytes, one 2-bit unsigned Intel channel -> J1939 single frame under the PGN */
    CHECK(CanTp_FlatSize(FLAT_ZNVW, sizeof FLAT_ZNVW) == (int)sizeof FLAT_ZNVW);
    CHECK(CanTp_FlatSize(FLAT_ZNVW, sizeof FLAT_ZNVW + 100) == (int)sizeof FLAT_ZNVW);        /* extra bytes after the cluster are fine */
    CHECK(CanTp_FlatSize(FLAT_ZNVW, sizeof FLAT_ZNVW - 1) == CANTP_ERR_FLAT);
    CHECK(CanTp_DefineFlat(12, FLAT_ZNVW, sizeof FLAT_ZNVW, -1, -1) == CANTP_OK);
    CHECK(CanTp_GetDef(12, md, 8, sd, 8) == 1);
    CHECK(md[0] == 0x18FD67FE && md[1] == 1 && md[2] == 8 && md[3] == CANTP_TP_J1939_BAM && md[4] == -1 && md[5] == 255 && md[6] == 255 && md[7] == 0);
    CHECK(sd[0] == 0 && sd[1] == 2 && sd[2] == 0 && sd[3] == 0 && sd[4] == 1 && sd[5] == 0 && sd[6] == 0 && sd[7] == 3);
    CHECK(CanTp_SignalCount(12) == 1 && CanTp_PayloadLength(12) == 8 && CanTp_FrameCount(12) == 1);
    double v[1] = { 2 }; uint8_t out[24]; int32_t n, c; double u[1];
    CHECK(CanTp_Pack(12, v, 1, 0, 0, out, 24, &n) == CANTP_OK && n == 24);
    CHECK(rd_u32(out + 8) == (0x18FD67FEu | CANTP_XNET_EXTENDED_ID_FLAG) && out[15] == 8 && out[16] == 0xFE && out[17] == 0xFF);
    /* placeholder SA FE in the cluster: any sender matches on receive */
    out[8] = 0x21;
    CHECK(CanTp_Unpack(12, out, 24, u, 1, &c) == CANTP_FOUND && u[0] == 2);
    /* SA override */
    CHECK(CanTp_DefineFlat(12, FLAT_ZNVW, sizeof FLAT_ZNVW, -1, 0x80) == CANTP_OK);
    CHECK(CanTp_GetDef(12, md, 8, NULL, 0) == 1 && md[0] == 0x18FD6780 && md[4] == 0x80);
    CHECK(CanTp_Unpack(12, out, 24, u, 1, &c) == CANTP_OK);                                    /* SA 0x21 no longer matches */

    /* ET1: 6 channels with offsets, defaults = the offsets */
    CHECK(CanTp_DefineFlat(13, FLAT_ET1, sizeof FLAT_ET1, -1, -1) == CANTP_OK);
    CHECK(CanTp_GetDef(13, md, 8, sd, 8) == 6 && md[0] == 0x18FEEEFE && md[2] == 8);
    CHECK(sd[0 * 8 + 5] == -40 && sd[4 * 8 + 0] == 16 && sd[4 * 8 + 1] == 16 && sd[4 * 8 + 4] == 0.03125 && sd[4 * 8 + 5] == -273);
    CHECK(CanTp_Defaults(13, dflt, 8) == 6 && dflt[0] == -40 && dflt[3] == 0 && dflt[4] == -273);
    double et[6] = { 90, 45, 30, 50, 100.5, 120 }; uint8_t o2[24]; double u6[6];
    CHECK(CanTp_Pack(13, et, 6, 0, 0, o2, 24, &n) == CANTP_OK && o2[16] == 130 && o2[17] == 85 && o2[22] == 70 && o2[23] == 125);
    CHECK(CanTp_Unpack(13, o2, 24, u6, 6, &c) == CANTP_FOUND); CHECKF(u6[4], 100.5); CHECKF(u6[5], 120);
    CHECK(CanTp_GetDef(13, NULL, 0, NULL, 0) == 6);
    CHECK(CanTp_GetDef(13, md, 7, NULL, 0) == CANTP_ERR_ARG);

    /* DM19: PDU1 PGN D300 with PS FE (placeholder destination), 20 bytes -> BAM, not RTS/CTS */
    CHECK(CanTp_DefineFlat(14, FLAT_DM19, sizeof FLAT_DM19, -1, -1) == CANTP_OK);
    CHECK(CanTp_GetDef(14, md, 8, NULL, 0) == 5 && md[3] == CANTP_TP_J1939_BAM && md[5] == 255 && CanTp_FrameCount(14) == 4);
    /* forced transports */
    CHECK(CanTp_DefineFlat(14, FLAT_DM19, sizeof FLAT_DM19, CANTP_TP_J1939_RTS, -1) == CANTP_OK);
    CHECK(CanTp_GetDef(14, md, 8, NULL, 0) == 5 && md[3] == CANTP_TP_J1939_RTS && md[5] == 0xFE);
    CHECK(CanTp_DefineFlat(14, FLAT_DM19, sizeof FLAT_DM19, CANTP_TP_CANFD, -1) == CANTP_OK && CanTp_OutputSize(14) == 40);
    CHECK(CanTp_DefineFlat(14, FLAT_DM19, sizeof FLAT_DM19, CANTP_TP_CLASSIC, -1) == CANTP_ERR_MSGDEF);   /* 20 bytes do not fit */

    /* argument errors */
    CHECK(CanTp_DefineFlat(-1, FLAT_ZNVW, sizeof FLAT_ZNVW, -1, -1) == CANTP_ERR_SLOT);
    CHECK(CanTp_DefineFlat(15, NULL, 100, -1, -1) == CANTP_ERR_ARG);
    CHECK(CanTp_DefineFlat(15, FLAT_ZNVW, 10, -1, -1) == CANTP_ERR_ARG);
    CHECK(CanTp_DefineFlat(15, FLAT_ZNVW, sizeof FLAT_ZNVW, 6, -1) == CANTP_ERR_ARG);
    CHECK(CanTp_DefineFlat(15, FLAT_ZNVW, sizeof FLAT_ZNVW, -1, 254) == CANTP_ERR_ARG);
    CHECK(CanTp_DefineFlat(15, FLAT_ZNVW, 100, -1, -1) == CANTP_ERR_FLAT && CanTp_SignalCount(15) == CANTP_ERR_NOT_DEFINED);
    uint8_t junk[400]; memset(junk, 0xFF, sizeof junk);
    CHECK(CanTp_DefineFlat(15, junk, sizeof junk, -1, -1) == CANTP_ERR_FLAT);                 /* string length -1 */
    memset(junk, 0, sizeof junk); junk[2] = 0x10;
    CHECK(CanTp_DefineFlat(15, junk, sizeof junk, -1, -1) == CANTP_ERR_FLAT);                 /* name (4096 bytes) longer than the buffer */
    memset(junk, 0, sizeof junk); junk[3] = 200;
    CHECK(CanTp_DefineFlat(15, junk, sizeof junk, -1, -1) == CANTP_OK && CanTp_SignalCount(15) == 0);   /* parses: 11-bit id 0, 0 bytes, 0 channels */
    CanTp_Clear(15);
    CHECK(CanTp_Defaults(15, dflt, 8) == CANTP_ERR_NOT_DEFINED && CanTp_Defaults(13, NULL, 8) == CANTP_ERR_ARG);
    /* table-defined slot: defaults are 0, GetDef round-trips */
    double m2[8], s2[8], m3[8], s3[8];
    msgdef(m2, 0x123, 0, 8, 0, -1, 255, 0); sigdef(s2, 7, 12, 1, 1, 0.5, -10, -100, 100);
    CHECK(CanTp_Define(15, m2, 8, s2, 1) == CANTP_OK && CanTp_Defaults(15, dflt, 8) == 1 && dflt[0] == 0);
    CHECK(CanTp_GetDef(15, m3, 8, s3, 1) == 1 && memcmp(m2, m3, sizeof m2) == 0 && memcmp(s2, s3, sizeof s2) == 0);
    msgdef(m2, 0x18FEE3FE, 1, 40, 1, -1, 255, 255);
    CHECK(CanTp_Define(15, m2, 8, s2, 1) == CANTP_OK && CanTp_GetDef(15, m3, 8, NULL, 0) == 1 && m3[4] == -1 && m3[0] == 0x18FEE3FE);
    msgdef(m2, 0x18FEE3FE, 1, 40, 1, 0x21, 255, 255);
    CHECK(CanTp_Define(15, m2, 8, s2, 1) == CANTP_OK && CanTp_GetDef(15, m3, 8, NULL, 0) == 1 && m3[4] == 0x21 && m3[0] == 0x18FEE321);
}

static void test_define_flat_synthetic(void)
{
    uint8_t buf[4096]; double md[8], sd[4 * 8], dflt[4];
    /* 11-bit id, 8 bytes, signed Motorola + float + lookup table -> classic; enum mapping */
    FCh ch[4] = {
        { 7, 12, 0, 1, 0.5, -10, -100, 100, 12.5, 3 },   /* signed, Motorola, with a 3-entry lookup table */
        { 16, 32, 2, 0, 1, 0, 0, 0, 1.25, 0 },           /* IEEE float 32 */
        { 48, 8, 1, 0, 1, 0, 0, 255, 7, 0 },             /* unsigned */
        { 56, 8, 1, 0, 1, 0, 0, 255, 0, 0 },
    };
    int n = fw_message(buf, sizeof buf, 0x123, 0, 8, 100.0, ch, 4);
    CHECK(n > 0 && n < (int)sizeof buf);
    CHECK(CanTp_FlatSize(buf, n) == n && CanTp_FlatSize(buf, n - 1) == CANTP_ERR_FLAT);
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_OK);
    CHECK(CanTp_GetDef(16, md, 8, sd, 4) == 4);
    CHECK(md[0] == 0x123 && md[1] == 0 && md[2] == 8 && md[3] == CANTP_TP_CLASSIC && md[4] == -1 && md[5] == 255 && md[6] == 0 && md[7] == 100);
    CHECK(sd[0] == 7 && sd[1] == 12 && sd[2] == 1 && sd[3] == 1 && sd[4] == 0.5 && sd[5] == -10 && sd[6] == -100 && sd[7] == 100);
    CHECK(sd[8 + 3] == 2 && sd[16 + 3] == 0 && sd[16 + 7] == 255);
    CHECK(CanTp_Defaults(16, dflt, 4) == 4 && dflt[0] == 12.5 && dflt[1] == 1.25 && dflt[2] == 7 && dflt[3] == 0);
    double v[4] = { 20.5, -3.75, 200, 1 }, u[4]; uint8_t out[24]; int32_t w, c;
    CHECK(CanTp_Pack(16, v, 4, 0, 0, out, 24, &w) == CANTP_OK && rd_u32(out + 8) == 0x123 && out[22] == 200 && out[23] == 1);
    CHECK(CanTp_Unpack(16, out, 24, u, 4, &c) == CANTP_FOUND); CHECKF(u[0], 20.5); CHECKF(u[1], -3.75); CHECKF(u[2], 200);
    /* a double float channel */
    FCh f64 = { 0, 64, 2, 0, 1, 0, 0, 0, 0, 0 };
    n = fw_message(buf, sizeof buf, 0x124, 0, 8, -1, &f64, 1);
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_OK && CanTp_GetDef(16, md, 8, sd, 1) == 1 && sd[3] == 3 && md[7] == 0);
    /* auto transport: 11-bit with 20 bytes -> CAN FD; 29-bit 8 bytes -> J1939 single; 29-bit PDU1 with a real DA and 20 bytes -> RTS/CTS */
    FCh u8 = { 0, 8, 1, 0, 1, 0, 0, 255, 0, 0 };
    n = fw_message(buf, sizeof buf, 0x125, 0, 20, -1, &u8, 1);
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_OK && CanTp_GetDef(16, md, 8, NULL, 0) == 1 && md[3] == CANTP_TP_CANFD && CanTp_OutputSize(16) == 40);
    n = fw_message(buf, sizeof buf, 0x18EA2180, 1, 20, -1, &u8, 1);       /* PGN EA00 (request), DA 0x21, SA 0x80 */
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_OK && CanTp_GetDef(16, md, 8, NULL, 0) == 1);
    CHECK(md[3] == CANTP_TP_J1939_RTS && md[5] == 0x21 && md[0] == 0x18EA2180 && md[4] == 0x80);   /* GetDef reports the effective SA */
    n = fw_message(buf, sizeof buf, 0x18EAFF80, 1, 20, -1, &u8, 1);       /* PDU1 to the global address -> BAM */
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_OK && CanTp_GetDef(16, md, 8, NULL, 0) == 1 && md[3] == CANTP_TP_J1939_BAM && md[5] == 255);
    n = fw_message(buf, sizeof buf, 0x18EA2180, 1, 8, -1, &u8, 1);        /* PDU1 to one node, fits one frame -> classic, id verbatim */
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_OK && CanTp_GetDef(16, md, 8, NULL, 0) == 1 && md[3] == CANTP_TP_CLASSIC && md[5] == 0x21 && CanTp_FrameCount(16) == 1);
    { double one[1] = { 9 }; uint8_t o[24]; int32_t w2;
      CHECK(CanTp_Pack(16, one, 1, 0, 0, o, 24, &w2) == CANTP_OK && rd_u32(o + 8) == (0x18EA2180u | CANTP_XNET_EXTENDED_ID_FLAG) && o[17] == 0xFF); }
    n = fw_message(buf, sizeof buf, 0x98FEE3FE, 0, 8, -1, &u8, 1);        /* DBC-style bit 31, extended flag 0 -> still extended */
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_OK && CanTp_GetDef(16, md, 8, NULL, 0) == 1 && md[1] == 1 && md[0] == 0x18FEE3FE);
    n = fw_message(buf, sizeof buf, 0x18FEE3FE, 0, 8, -1, &u8, 1);        /* id > 0x7FF with extended flag 0 (seen in ECD files) */
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_OK && CanTp_GetDef(16, md, 8, NULL, 0) == 1 && md[1] == 1);
    /* bad channel content */
    FCh bad = { 0, 8, 3, 0, 1, 0, 0, 255, 0, 0 };
    n = fw_message(buf, sizeof buf, 0x125, 0, 8, -1, &bad, 1);
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_ERR_SIGDEF);
    bad.dtype = 1; bad.order = 2;
    n = fw_message(buf, sizeof buf, 0x125, 0, 8, -1, &bad, 1);
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_ERR_SIGDEF);
    bad.order = 0; bad.start = 60;
    n = fw_message(buf, sizeof buf, 0x125, 0, 8, -1, &bad, 1);
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_ERR_SIGDEF);                        /* past the payload */
    /* too many channels */
    static FCh many[CANTP_MAX_SIGNALS + 1]; static uint8_t big[64 * 1024];
    for (int i = 0; i <= CANTP_MAX_SIGNALS; i++) { FCh fc = { i, 1, 1, 0, 1, 0, 0, 1, 0, 0 }; many[i] = fc; }
    n = fw_message(big, sizeof big, 0x18FF0080, 1, 40, -1, many, CANTP_MAX_SIGNALS + 1);
    CHECK(n < (int)sizeof big && CanTp_DefineFlat(16, big, n, -1, -1) == CANTP_ERR_TOO_MANY);
    n = fw_message(big, sizeof big, 0x18FF0080, 1, 40, -1, many, CANTP_MAX_SIGNALS);
    CHECK(CanTp_DefineFlat(16, big, n, -1, -1) == CANTP_OK && CanTp_SignalCount(16) == CANTP_MAX_SIGNALS && CanTp_FlatSize(big, n) == n);
    /* zero channels, zero bytes (DM11-style request-only message) */
    n = fw_message(buf, sizeof buf, 0x18FED3FE, 0, 0, -1, NULL, 0);
    CHECK(CanTp_DefineFlat(16, buf, n, -1, -1) == CANTP_OK && CanTp_SignalCount(16) == 0 && CanTp_OutputSize(16) == 24);
    /* two clusters back to back: FlatSize walks them */
    int n1 = fw_message(buf, sizeof buf, 0x18FEE3FE, 1, 8, -1, &u8, 1);
    int n2 = fw_message(buf + n1, sizeof buf - n1, 0x18FEEEFE, 1, 8, -1, ch, 4);
    CHECK(CanTp_FlatSize(buf, n1 + n2) == n1 && CanTp_FlatSize(buf + n1, n2) == n2);
    CHECK(CanTp_DefineFlat(16, buf + n1, n2, -1, -1) == CANTP_OK && CanTp_SignalCount(16) == 4);
    CanTp_Clear(16);
}

static void test_transfer(void)
{
    /* BAM message: 40 bytes, 7 frames */
    double m[8], s[2 * 8];
    msgdef(m, 0x18FEE3FE, 1, 40, 1, -1, 255, 255);
    sigdef(s, 256, 16, 0, 0, 1, 0, 0, 64255); sigdef(s + 8, 312, 8, 0, 0, 1, -125, -125, 125);
    CHECK(CanTp_Define(17, m, 8, s, 2) == CANTP_OK);
    double v[2] = { 2500, 10 }, u[2] = { 0, 0 }; uint8_t out[168], lens[8]; int32_t used, nf;
    memset(lens, 0xAA, sizeof lens);
    CHECK(CanTp_Transfer(17, CANTP_MODE_WRITE, v, 2, out, sizeof out, lens, 8, 1000, 500000, &used, &nf) == CANTP_OK);
    CHECK(used == 168 && nf == 7);
    for (int i = 0; i < 7; i++) CHECK(lens[i] == 8);
    CHECK(lens[7] == 0xAA);
    CHECK(rd_u64(out) == 1000 && rd_u64(out + 24 * 6) == 1000 + 6 * 500000);
    CHECK(CanTp_FrameLengths(out, 168, NULL, 0) == 7);
    uint8_t l3[3];
    CHECK(CanTp_FrameLengths(out, 168, l3, 3) == 7 && l3[2] == 8);                          /* count even when the array is short */
    CHECK(CanTp_FrameLengths(out, 167, l3, 3) == CANTP_ERR_RECORD);
    /* write with a short length array: sizes are reported */
    CHECK(CanTp_Transfer(17, CANTP_MODE_WRITE, v, 2, out, sizeof out, l3, 3, 0, 0, &used, &nf) == CANTP_ERR_BUFFER && nf == 7 && used == 168);
    CHECK(CanTp_Transfer(17, CANTP_MODE_WRITE, v, 2, out, 100, lens, 8, 0, 0, &used, &nf) == CANTP_ERR_BUFFER && used == 168 && nf == 0);
    /* frameLens optional on write */
    CHECK(CanTp_Transfer(17, CANTP_MODE_WRITE, v, 2, out, sizeof out, NULL, 0, 0, 0, &used, &nf) == CANTP_OK && nf == 7);

    /* read back with and without the length array */
    CHECK(CanTp_Transfer(17, CANTP_MODE_READ, u, 2, out, 168, lens, 7, 0, 0, &used, &nf) == CANTP_FOUND);
    CHECK(used == 168 && nf == 7 && u[0] == 2500 && u[1] == 10);
    u[0] = 0;
    CHECK(CanTp_Transfer(17, CANTP_MODE_READ, u, 2, out, 168, NULL, 0, 0, 0, &used, &nf) == CANTP_FOUND && nf == 7 && u[0] == 2500);
    /* the caller's list stops the walk: 6 of 7 frames -> nothing complete */
    CHECK(CanTp_Transfer(17, CANTP_MODE_READ, u, 2, out, 168, lens, 6, 0, 0, &used, &nf) == CANTP_OK && used == 144 && nf == 6);
    /* a length that disagrees with the record header */
    lens[3] = 7;
    CHECK(CanTp_Transfer(17, CANTP_MODE_READ, u, 2, out, 168, lens, 7, 0, 0, &used, &nf) == CANTP_ERR_RECORD && used == 72 && nf == 3);
    lens[3] = 8;
    /* the live receive state is untouched by the stateless read */
    CHECK(CanTp_RxFeed(17, out, 24, u, 2) == CANTP_OK && CanTp_RxState(17) == 1);
    CHECK(CanTp_Transfer(17, CANTP_MODE_READ, u, 2, out, 168, lens, 7, 0, 0, &used, &nf) == CANTP_FOUND && CanTp_RxState(17) == 1);
    CanTp_RxReset(17);
    /* bad mode / args */
    CHECK(CanTp_Transfer(17, 2, u, 2, out, 168, lens, 7, 0, 0, &used, &nf) == CANTP_ERR_ARG);
    CHECK(CanTp_Transfer(17, CANTP_MODE_READ, u, 2, out, 168, lens, 7, 0, 0, NULL, &nf) == CANTP_ERR_ARG);
    CHECK(CanTp_Transfer(17, CANTP_MODE_READ, u, 2, out, 168, NULL, 7, 0, 0, &used, &nf) == CANTP_ERR_ARG);
    CHECK(CanTp_Transfer(18, CANTP_MODE_READ, u, 2, out, 168, lens, 7, 0, 0, &used, &nf) == CANTP_ERR_NOT_DEFINED);
    CHECK(CanTp_Transfer(99, CANTP_MODE_READ, u, 2, out, 168, lens, 7, 0, 0, &used, &nf) == CANTP_ERR_SLOT);

    /* SGL variant */
    float fv[2] = { 1234, -5 }, fu[2] = { 0, 0 };
    CHECK(CanTp_TransferSgl(17, CANTP_MODE_WRITE, fv, 2, out, sizeof out, lens, 8, 0, 0, &used, &nf) == CANTP_OK && nf == 7);
    CHECK(CanTp_TransferSgl(17, CANTP_MODE_READ, fu, 2, out, used, lens, nf, 0, 0, &used, &nf) == CANTP_FOUND && fu[0] == 1234 && fu[1] == -5);

    /* classic frame shorter than 8 bytes and a CAN FD frame: lengths reflect the DLC */
    double m2[8], s2[8], one[1] = { 5 };
    msgdef(m2, 0x321, 0, 3, 0, -1, 255, 0); sigdef(s2, 0, 8, 0, 0, 1, 0, 0, 0);
    CHECK(CanTp_Define(18, m2, 8, s2, 1) == CANTP_OK);
    CHECK(CanTp_Transfer(18, CANTP_MODE_WRITE, one, 1, out, sizeof out, lens, 8, 0, 0, &used, &nf) == CANTP_OK && used == 24 && nf == 1 && lens[0] == 3);
    msgdef(m2, 0x18ABCDEF, 1, 20, 3, -1, 255, 0);
    CHECK(CanTp_Define(18, m2, 8, s2, 1) == CANTP_OK);
    CHECK(CanTp_Transfer(18, CANTP_MODE_WRITE, one, 1, out, sizeof out, lens, 8, 0, 0, &used, &nf) == CANTP_OK && used == 40 && nf == 1 && lens[0] == 20 && out[12] == 0x18);
    CHECK(CanTp_Transfer(18, CANTP_MODE_READ, u, 1, out, 40, lens, 1, 0, 0, &used, &nf) == CANTP_FOUND && u[0] == 5 && used == 40);
    /* mixed buffer: FD record then a 24-byte one */
    uint8_t mix[64]; uint8_t d[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }; uint8_t ml[4];
    memcpy(mix, out, 40);
    CHECK(CanTp_MakeRecord(0x100, 0, 0, d, 5, 0, mix + 40, 24) == 24);
    CHECK(CanTp_FrameLengths(mix, 64, ml, 4) == 2 && ml[0] == 20 && ml[1] == 5);
    CanTp_Clear(17); CanTp_Clear(18);
}
