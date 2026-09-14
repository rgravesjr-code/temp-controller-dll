/* test_release4.inc - v1.3.0: the flattened "XNET Frame CAN" cluster array:
 * CanTp_TimeToLabView / TimeFromLabView, XnetFrameCount, RecordsToXnet /
 * XnetToRecords, TransferXnet. Included by test_main.c. */

/* Scott's samples, flattened in LabVIEW on 2026-09-14 (tests/fixtures/xnet_*.bin):
 * one frame 0x18FEF100 payload 01..08, and an array of that frame plus 0x18FEF101 payload 10..80. */
static const uint8_t XNET_ONE[35] = {
    0x00,0x00,0x00,0x00,0xE6,0xCD,0xA5,0xA2, 0x77,0x86,0xB8,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x08, 0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x18,0xFE,0xF1,0x00, 0x00, 0x01, 0x00 };
static const uint8_t XNET_TWO[74] = {
    0x00,0x00,0x00,0x02,
    0x00,0x00,0x00,0x00,0xE6,0xCD,0xA5,0xA2, 0x77,0x86,0xB6,0x68,0x06,0x79,0x96,0xC4,
    0x00,0x00,0x00,0x08, 0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x18,0xFE,0xF1,0x00, 0x00, 0x01, 0x00,
    0x00,0x00,0x00,0x00,0xE6,0xCD,0xA5,0xA2, 0x77,0x86,0xB6,0x68,0x06,0x79,0x96,0xC4,
    0x00,0x00,0x00,0x08, 0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80,
    0x18,0xFE,0xF1,0x01, 0x00, 0x01, 0x00 };
#define SCOTT_TS 134338657624668993ull    /* 2026-09-14 13:22:42.4668993 UTC as 100 ns since 1601 (the two-frame sample) */
/* the one-frame sample carries the same instant with its fraction cut to 32 bits (0x7786B800_00000000 = .46687939...), which rounds one tick higher */
#define SCOTT_TS_ONE (SCOTT_TS + 1)

static uint32_t rd_be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t rd_u64_be(const uint8_t* p) { return ((uint64_t)rd_be32(p) << 32) | rd_be32(p + 4); }

static void test_labview_time(void)
{
    int64_t s; uint64_t f;
    CHECK(CanTp_TimeToLabView(0, &s, &f) == CANTP_OK && s == 0 && f == 0);
    CHECK(CanTp_TimeFromLabView(0, 0) == 0);
    CHECK(CanTp_TimeToLabView(1, NULL, &f) == CANTP_ERR_ARG);
    /* Unix epoch: 1970-01-01 = 2082844800 s after 1904 = 116444736000000000 (FILETIME) */
    CHECK(CanTp_TimeToLabView(116444736000000000ull, &s, &f) == CANTP_OK && s == 2082844800 && f == 0);
    CHECK(CanTp_TimeFromLabView(2082844800, 0) == 116444736000000000ull);
    /* half a second */
    CHECK(CanTp_TimeToLabView(116444736000000000ull + 5000000, &s, &f) == CANTP_OK && s == 2082844800 && f == 0x8000000000000000ull);
    CHECK(CanTp_TimeFromLabView(2082844800, 0x8000000000000000ull) == 116444736000000000ull + 5000000);
    /* Scott's frames: seconds 0xE6CDA5A2; fraction 0x7786B800_00000000 -> 4668994 x 100 ns (rounded), 0x7786B668067996C4 -> 4668993 */
    CHECK(CanTp_TimeFromLabView(0xE6CDA5A2, 0x7786B80000000000ull) == SCOTT_TS_ONE);
    CHECK(CanTp_TimeFromLabView(0xE6CDA5A2, 0x7786B668067996C4ull) == SCOTT_TS);      /* the two-frame sample, same instant */
    CHECK(CanTp_TimeToLabView(SCOTT_TS, &s, &f) == CANTP_OK && s == 0xE6CDA5A2 && f == 0x7786B668067996C4ull);  /* byte-exact with LabVIEW */
    /* round trip over a spread of values, incl. the last 100 ns of a second */
    uint64_t ts[] = { 1, 9999999, 10000000, 123456789012345678ull, 134338657629999999ull, 0xFFFFFFFFFFFFFFFull };
    for (size_t i = 0; i < sizeof ts / sizeof ts[0]; i++) {
        CanTp_TimeToLabView(ts[i], &s, &f);
        CHECK(CanTp_TimeFromLabView(s, f) == ts[i]);
    }
    CHECK(CanTp_TimeFromLabView(-9561628801, 0) == 0);                                  /* before 1601: clamps */
}

static void test_xnet_convert(void)
{
    uint8_t rec[96], back[128]; int32_t n;
    /* the two-frame sample -> records */
    CHECK(CanTp_XnetFrameCount(XNET_TWO, sizeof XNET_TWO) == 2);
    CHECK(CanTp_XnetFrameCount(XNET_ONE, sizeof XNET_ONE) == 0);                         /* a bare frame reads as an empty array (count 0, trailing bytes ignored) */
    CHECK(CanTp_XnetFrameCount(XNET_TWO, sizeof XNET_TWO - 1) == CANTP_ERR_RECORD);
    CHECK(CanTp_XnetFrameCount(XNET_TWO, 3) == CANTP_ERR_RECORD);
    CHECK(CanTp_XnetToRecords(XNET_TWO, sizeof XNET_TWO, rec, sizeof rec, &n) == 2 && n == 48);
    CHECK(rd_u64(rec) == SCOTT_TS && rd_u32(rec + 8) == (0x18FEF100u | CANTP_XNET_EXTENDED_ID_FLAG) && rec[12] == 0 && rec[15] == 8);
    CHECK(rec[16] == 1 && rec[23] == 8);
    CHECK(rd_u32(rec + 32) == (0x18FEF101u | CANTP_XNET_EXTENDED_ID_FLAG) && rec[40] == 0x10 && rec[47] == 0x80);
    CHECK(CanTp_XnetToRecords(XNET_TWO, sizeof XNET_TWO, rec, 47, &n) == CANTP_ERR_BUFFER && n == 48);
    CHECK(CanTp_XnetToRecords(XNET_TWO, sizeof XNET_TWO, NULL, 0, &n) == CANTP_ERR_BUFFER && n == 48);   /* size query */
    /* records -> array, byte-identical to LabVIEW's flatten */
    CHECK(CanTp_RecordsToXnet(rec, 48, back, sizeof back, &n) == 2 && n == 74);
    CHECK(memcmp(back, XNET_TWO, 74) == 0);
    CHECK(CanTp_RecordsToXnet(rec, 48, back, 73, &n) == CANTP_ERR_BUFFER && n == 74);
    CHECK(CanTp_RecordsToXnet(rec, 47, back, sizeof back, &n) == CANTP_ERR_RECORD);       /* truncated record */
    CHECK(CanTp_RecordsToXnet(rec, 0, back, sizeof back, &n) == 0 && n == 4 && rd_be32(back) == 0);   /* empty array */
    /* a lone frame from the one-frame sample: prefix the count */
    uint8_t one[39]; one[0] = one[1] = one[2] = 0; one[3] = 1; memcpy(one + 4, XNET_ONE, 35);
    CHECK(CanTp_XnetToRecords(one, 39, rec, sizeof rec, &n) == 1 && n == 24 && rd_u64(rec) == SCOTT_TS_ONE && rec[15] == 8 && rec[23] == 8);
    /* CAN FD record, CAN 2.0 type and 0 timestamp survive both ways; type byte verbatim */
    uint8_t d[64]; for (int i = 0; i < 64; i++) d[i] = (uint8_t)i;
    int32_t s1 = CanTp_MakeRecord(0x123, 0, 0x18, d, 20, 0, rec, sizeof rec);
    int32_t s2 = CanTp_MakeRecord(0x7FF, 0, 0x00, d, 3, 0, rec + s1, sizeof rec - s1);
    rec[s1 + 12] = 8;                                                                      /* CAN 2.0 Data */
    CHECK(s1 == 40 && s2 == 24);
    CHECK(CanTp_RecordsToXnet(rec, 64, back, sizeof back, &n) == 2 && n == 4 + 47 + 30);
    CHECK(rd_be32(back) == 2 && rd_be32(back + 4 + 16) == 20 && rd_be32(back + 4 + 20 + 20) == 0x123 && back[4 + 44] == 0x18 && back[4 + 45] == 0 && back[4 + 46] == 0);
    CHECK(rd_u64(back + 4) == 0 && rd_u64(back + 12) == 0);                                /* 0 -> 1904-01-01 */
    CHECK(back[4 + 47 + 24 + 3] == 8 && rd_be32(back + 4 + 47 + 20 + 3) == 0x7FF);
    uint8_t rec2[64];
    CHECK(CanTp_XnetToRecords(back, n, rec2, sizeof rec2, &n) == 2 && n == 64 && memcmp(rec, rec2, 64) == 0);
    /* a J1939 Data frame over 64 bytes has no record form */
    uint8_t big[4 + 27 + 100]; memset(big, 0, sizeof big); big[3] = 1; big[4 + 19] = 100; big[4 + 27 + 100 - 3] = 192;
    CHECK(CanTp_XnetFrameCount(big, sizeof big) == 1);
    CHECK(CanTp_XnetToRecords(big, sizeof big, rec, sizeof rec, &n) == CANTP_ERR_RECORD);
    /* negative count / payload length */
    big[0] = 0xFF; CHECK(CanTp_XnetFrameCount(big, sizeof big) == CANTP_ERR_RECORD); big[0] = 0;
    big[4 + 16] = 0x80; CHECK(CanTp_XnetFrameCount(big, sizeof big) == CANTP_ERR_RECORD);
}

static void test_transfer_xnet(void)
{
    double m[8], s[8 * 4];
    uint8_t xnet[512], rec[256]; int32_t used, nf, n; double v[4], u[4];
    /* CCVS-like 8-byte PG 0xFEF1 from SA 0: 4 unsigned Intel signals covering all 8 bytes; read Scott's frames */
    msgdef(m, 0x18FEF100, 1, 8, 1, -1, 255, 255);
    sigdef(s + 0,  0, 8, 0, 0, 1, 0, 0, 255);
    sigdef(s + 8,  8, 16, 0, 0, 1, 0, 0, 65535);
    sigdef(s + 16, 24, 8, 0, 0, 1, 0, 0, 255);
    sigdef(s + 24, 32, 32, 0, 0, 1, 0, 0, 4294967295.0);
    CHECK(CanTp_Define(14, m, 8, s, 4) == CANTP_OK);
    CHECK(CanTp_TransferXnet(14, CANTP_MODE_READ, u, 4, (uint8_t*)XNET_TWO, sizeof XNET_TWO, 0, 0, 0, &used, &nf) == CANTP_FOUND);
    CHECK(u[0] == 1 && u[1] == 0x0302 && u[2] == 4 && u[3] == 0x08070605 && used == 39 && nf == 1);   /* stops at the first match */
    /* write: the same frame back, timestamp given, byte-identical to Scott's except echo/timestamp fields we control */
    v[0] = 1; v[1] = 0x0302; v[2] = 4; v[3] = 0x08070605;
    CHECK(CanTp_TransferXnet(14, CANTP_MODE_WRITE, v, 4, xnet, sizeof xnet, CANTP_XNET_FRAMES, SCOTT_TS, 0, &used, &nf) == CANTP_OK);
    CHECK(used == 39 && nf == 1 && memcmp(xnet + 4, XNET_TWO + 4, 20 + 8 + 7) == 0);          /* == frame 1 of the sample, bit for bit */
    CHECK(rd_be32(xnet) == 1);
    /* the sample's second frame decodes on a slot for SA 1 */
    msgdef(m, 0x18FEF101, 1, 8, 1, -1, 255, 255);
    CHECK(CanTp_Define(15, m, 8, s, 4) == CANTP_OK);
    CHECK(CanTp_TransferXnet(15, CANTP_MODE_READ, u, 4, (uint8_t*)XNET_TWO, sizeof XNET_TWO, 0, 0, 0, &used, &nf) == CANTP_FOUND);
    CHECK(u[0] == 0x10 && u[1] == 0x3020 && u[2] == 0x40 && u[3] == 2154848336.0 && used == 74 && nf == 2);
    /* buffer too small: sizes needed */
    CHECK(CanTp_TransferXnet(14, CANTP_MODE_WRITE, v, 4, xnet, 38, CANTP_XNET_FRAMES, 0, 0, &used, &nf) == CANTP_ERR_BUFFER && used == 39 && nf == 1);
    CHECK(CanTp_TransferXnet(14, CANTP_MODE_WRITE, v, 4, NULL, 0, CANTP_XNET_FRAMES, 0, 0, &used, &nf) == CANTP_ERR_BUFFER && used == 39 && nf == 1);
    CHECK(CanTp_TransferXnet(14, CANTP_MODE_WRITE, v, 4, xnet, sizeof xnet, 2, 0, 0, &used, &nf) == CANTP_ERR_ARG);
    CHECK(CanTp_TransferXnet(14, 2, v, 4, xnet, sizeof xnet, 0, 0, 0, &used, &nf) == CANTP_ERR_ARG);
    CHECK(CanTp_TransferXnet(14, 0, v, 4, xnet, sizeof xnet, 0, 0, 0, NULL, &nf) == CANTP_ERR_ARG);
    CHECK(CanTp_TransferXnet(31, 0, v, 4, xnet, sizeof xnet, 0, 0, 0, &used, &nf) == CANTP_ERR_NOT_DEFINED);
    CHECK(CanTp_TransferXnet(32, 0, v, 4, xnet, sizeof xnet, 0, 0, 0, &used, &nf) == CANTP_ERR_SLOT);
    CHECK(CanTp_TransferXnet(14, CANTP_MODE_READ, u, 4, (uint8_t*)XNET_TWO, 70, 0, 0, 0, &used, &nf) == CANTP_ERR_RECORD);

    /* BAM: 20-byte PG 0xFF20 from SA 0x80 -> TP.CM + 3 TP.DT as CAN Data frames == RecordsToXnet(Pack) */
    msgdef(m, 0x18FF2080, 1, 20, 1, -1, 255, 255);
    sigdef(s + 0, 0, 16, 0, 0, 0.5, 0, 0, 30000);
    sigdef(s + 8, 152, 8, 0, 0, 1, -40, -40, 210);
    CHECK(CanTp_Define(16, m, 8, s, 2) == CANTP_OK && CanTp_FrameCount(16) == 4);
    v[0] = 1234.5; v[1] = 85;
    CHECK(CanTp_TransferXnet(16, CANTP_MODE_WRITE, v, 2, xnet, sizeof xnet, CANTP_XNET_FRAMES, 77, 5, &used, &nf) == CANTP_OK);
    CHECK(nf == 4 && used == 4 + 4 * 35);
    CHECK(CanTp_Pack(16, v, 2, 77, 5, rec, sizeof rec, &n) == CANTP_OK && n == 96);
    uint8_t x2[256]; int32_t n2;
    CHECK(CanTp_RecordsToXnet(rec, 96, x2, sizeof x2, &n2) == 4 && n2 == used && memcmp(x2, xnet, used) == 0);
    CHECK(rd_be32(xnet + 4 + 28) == 0x1CECFF80 && xnet[4 + 32] == 0 && xnet[4 + 33] == 1 && xnet[4 + 34] == 0);   /* TP.CM id, CAN Data, extended, no echo */
    CHECK(xnet[4 + 20] == 0x20 && xnet[4 + 21] == 20 && xnet[4 + 23] == 3);                                       /* BAM, 20 bytes, 3 packets */
    CHECK(rd_be32(xnet + 4 + 35 + 28) == 0x1CEBFF80 && xnet[4 + 35 + 20] == 1);                                 /* TP.DT #1 */
    CHECK(CanTp_TimeFromLabView((int64_t)rd_u64_be(xnet + 4), rd_u64_be(xnet + 12)) == 77 && CanTp_TimeFromLabView((int64_t)rd_u64_be(xnet + 4 + 35), rd_u64_be(xnet + 4 + 35 + 8)) == 82);
    CHECK(CanTp_TransferXnet(16, CANTP_MODE_READ, u, 2, xnet, used, 0, 0, 0, &used, &nf) == CANTP_FOUND && u[0] == 1234.5 && u[1] == 85 && nf == 4 && used == 144);
    /* a remote frame and an echo of a foreign id in front are skipped; TP.DT before TP.CM -> nothing complete */
    uint8_t mixed[512]; memcpy(mixed, xnet, 4); mixed[3] = 6;
    uint8_t remote[27]; memset(remote, 0, 27); remote[20 + 3] = 0x22; remote[24] = 1;                              /* type Remote, id 0x22 */
    uint8_t foreign[27 + 8]; memset(foreign, 0, sizeof foreign); foreign[19] = 8; foreign[20 + 8 + 3] = 0x33; foreign[24 + 8] = 0; foreign[26 + 8] = 1;   /* echo?=1 */
    memcpy(mixed + 4, remote, 27); memcpy(mixed + 31, foreign, 35); memcpy(mixed + 66, xnet + 4, 140);
    CHECK(CanTp_XnetFrameCount(mixed, 206) == 6);
    CHECK(CanTp_TransferXnet(16, CANTP_MODE_READ, u, 2, mixed, 206, 0, 0, 0, &used, &nf) == CANTP_FOUND && nf == 6 && used == 206 && u[1] == 85);
    CHECK(CanTp_TransferXnet(16, CANTP_MODE_READ, u, 2, xnet, used, 0, 0, 0, &used, &nf) == CANTP_FOUND);           /* stateless: again */
    memcpy(mixed, xnet, 4); mixed[3] = 3; memcpy(mixed + 4, xnet + 4 + 35, 105);                                     /* DT only */
    CHECK(CanTp_TransferXnet(16, CANTP_MODE_READ, u, 2, mixed, 109, 0, 0, 0, &used, &nf) == CANTP_OK && nf == 3);

    /* J1939 Data mode: one frame, type 192, the whole 20-byte payload under the PG id */
    CHECK(CanTp_TransferXnet(16, CANTP_MODE_WRITE, v, 2, xnet, sizeof xnet, CANTP_XNET_J1939, 0, 0, &used, &nf) == CANTP_OK);
    CHECK(nf == 1 && used == 4 + 27 + 20 && rd_be32(xnet) == 1 && rd_be32(xnet + 4 + 16) == 20);
    CHECK(rd_be32(xnet + 4 + 40) == 0x18FF2080 && xnet[4 + 44] == 192 && xnet[4 + 45] == 1 && xnet[4 + 46] == 0);
    CHECK(xnet[4 + 20] == (2469 & 0xFF) && xnet[4 + 21] == (2469 >> 8) && xnet[4 + 39] == 125 && xnet[4 + 22] == 0xFF);   /* 1234.5/0.5, 85+40, pad */
    CHECK(CanTp_TransferXnet(16, CANTP_MODE_READ, u, 2, xnet, used, 0, 0, 0, &used, &nf) == CANTP_FOUND && u[0] == 1234.5 && u[1] == 85 && nf == 1);
    CHECK(CanTp_TransferXnet(16, CANTP_MODE_WRITE, v, 2, xnet, 50, CANTP_XNET_J1939, 0, 0, &used, &nf) == CANTP_ERR_BUFFER && used == 51 && nf == 1);
    xnet[4 + 40 + 3] = 0x81;                                                                                        /* another SA: not ours */
    CHECK(CanTp_TransferXnet(16, CANTP_MODE_READ, u, 2, xnet, 51, 0, 0, 0, &used, &nf) == CANTP_OK && nf == 1);
    CHECK(CanTp_XnetToRecords(xnet, 51, rec, sizeof rec, &n) == 1 && rec[12] == 192 && rec[15] == 20);            /* <= 64 bytes: has a record form */
    /* RTS/CTS message in J1939 Data mode needs no session */
    msgdef(m, 0x18E02180, 1, 30, 4, -1, 0x21, 255);
    CHECK(CanTp_Define(17, m, 8, s, 2) == CANTP_OK);
    CHECK(CanTp_Pack(17, v, 2, 0, 0, rec, sizeof rec, &n) == CANTP_ERR_TRANSPORT);
    CHECK(CanTp_TransferXnet(17, CANTP_MODE_WRITE, v, 2, xnet, sizeof xnet, CANTP_XNET_J1939, 0, 0, &used, &nf) == CANTP_OK && nf == 1 && used == 61);
    CHECK(rd_be32(xnet + 4 + 50) == 0x18E02180 && xnet[4 + 54] == 192);
    CHECK(CanTp_TransferXnet(17, CANTP_MODE_READ, u, 2, xnet, used, 0, 0, 0, &used, &nf) == CANTP_FOUND && u[0] == 1234.5);
    CHECK(CanTp_TransferXnet(17, CANTP_MODE_WRITE, v, 2, xnet, sizeof xnet, CANTP_XNET_FRAMES, 0, 0, &used, &nf) == CANTP_ERR_TRANSPORT);
    /* classic 11-bit message refuses J1939 mode; frames mode gives one CAN Data frame with the true DLC */
    msgdef(m, 0x123, 0, 3, 0, -1, 255, 0);
    CHECK(CanTp_Define(18, m, 8, s, 1) == CANTP_OK);
    CHECK(CanTp_TransferXnet(18, CANTP_MODE_WRITE, v, 1, xnet, sizeof xnet, CANTP_XNET_J1939, 0, 0, &used, &nf) == CANTP_ERR_TRANSPORT);
    CHECK(CanTp_TransferXnet(18, CANTP_MODE_WRITE, v, 1, xnet, sizeof xnet, CANTP_XNET_FRAMES, 0, 0, &used, &nf) == CANTP_OK && nf == 1 && used == 34);
    CHECK(rd_be32(xnet + 4 + 16) == 3 && rd_be32(xnet + 4 + 23) == 0x123 && xnet[4 + 27] == 0 && xnet[4 + 28] == 0);
    /* CAN FD 20 bytes: type 16, payload length 20 */
    msgdef(m, 0x456, 0, 20, 2, -1, 255, 0xAA);
    CHECK(CanTp_Define(19, m, 8, s, 2) == CANTP_OK);
    CHECK(CanTp_TransferXnet(19, CANTP_MODE_WRITE, v, 2, xnet, sizeof xnet, CANTP_XNET_FRAMES, 0, 0, &used, &nf) == CANTP_OK && nf == 1 && used == 51);
    CHECK(rd_be32(xnet + 4 + 16) == 20 && xnet[4 + 44] == 16 && xnet[4 + 45] == 0);
    CHECK(CanTp_TransferXnet(19, CANTP_MODE_READ, u, 2, xnet, used, 0, 0, 0, &used, &nf) == CANTP_FOUND && u[0] == 1234.5 && u[1] == 85);
    xnet[4 + 44] = 8;                                                                                               /* CAN 2.0 Data type decodes too (on a classic slot) */
    msgdef(m, 0x456, 0, 8, 0, -1, 255, 0);
    CHECK(CanTp_Define(19, m, 8, s, 1) == CANTP_OK);
    CHECK(CanTp_TransferXnet(19, CANTP_MODE_READ, u, 1, xnet, used, 0, 0, 0, &used, &nf) == CANTP_FOUND && u[0] == 1234.5);
    /* SGL twin */
    float fv[2] = { 1234.5f, 85.0f }, fu[2];
    CHECK(CanTp_TransferXnetSgl(16, CANTP_MODE_WRITE, fv, 2, xnet, sizeof xnet, CANTP_XNET_FRAMES, 0, 0, &used, &nf) == CANTP_OK && nf == 4);
    CHECK(CanTp_TransferXnetSgl(16, CANTP_MODE_READ, fu, 2, xnet, used, 0, 0, 0, &used, &nf) == CANTP_FOUND && fu[0] == 1234.5f && fu[1] == 85.0f);
    /* the longest BAM (1785 bytes = 1 CM + 255 DT) fits the internal record buffer */
    msgdef(m, 0x18FF3080, 1, 1785, 1, -1, 255, 255);
    CHECK(CanTp_Define(20, m, 8, s, 1) == CANTP_OK && CanTp_FrameCount(20) == 256);
    static uint8_t bigx[4 + 256 * 35];
    CHECK(CanTp_TransferXnet(20, CANTP_MODE_WRITE, v, 1, bigx, sizeof bigx, CANTP_XNET_FRAMES, 0, 0, &used, &nf) == CANTP_OK && nf == 256 && used == (int32_t)sizeof bigx);
    CHECK(CanTp_TransferXnet(20, CANTP_MODE_READ, u, 1, bigx, used, 0, 0, 0, &used, &nf) == CANTP_FOUND && u[0] == 1234.5 && nf == 256);
    CHECK(CanTp_TransferXnet(20, CANTP_MODE_WRITE, v, 1, bigx, sizeof bigx, CANTP_XNET_J1939, 0, 0, &used, &nf) == CANTP_OK && nf == 1 && used == 4 + 27 + 1785);
    CHECK(CanTp_TransferXnet(20, CANTP_MODE_READ, u, 1, bigx, used, 0, 0, 0, &used, &nf) == CANTP_FOUND && u[0] == 1234.5);
    for (int i = 14; i <= 20; i++) CanTp_Clear(i);
}
