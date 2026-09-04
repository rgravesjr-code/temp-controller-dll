/* cantp_internal.h - private declarations shared by the CanTp sources. */
#ifndef CANTP_INTERNAL_H
#define CANTP_INTERNAL_H
#include <stdint.h>
#include <string.h>
#include "cantp.h"

#define XNET_TYPE_CAN_DATA    0x00
#define XNET_TYPE_CANFD_DATA  0x10
#define XNET_TYPE_CANFDBRS    0x18

#define J1939_TP_CM_PGN   0xEC00u
#define J1939_TP_DT_PGN   0xEB00u
#define J1939_TP_PRIORITY 7u
#define J1939_GLOBAL_DA   0xFFu
#define J1939_BAM_CTRL    0x20u
#define J1939_SA_ANY      0xFEu   /* Vector DBC placeholder */

typedef struct {
    int      start, len, motorola, type;
    double   factor, offset, min, max;
} CanTpSig;

typedef struct {
    int      total;                        /* expected bytes, 0 = idle */
    int      packets;
    int      nextSeq;
    uint8_t  sa;                           /* sender of the session */
    uint8_t  buf[CANTP_MAX_PAYLOAD];
} CanTpRx;

typedef struct {
    int      used;
    uint32_t id;                           /* bare 11/29-bit id, SA already substituted */
    int      ext;
    int      len;
    int      transport;
    int      saAny;                        /* 29-bit id with SA 0xFE and no override */
    uint8_t  sa, da, pad;
    double   cycleMs;
    int      nSig;
    CanTpSig sig[CANTP_MAX_SIGNALS];
    CanTpRx  rx;
} CanTpMsg;

/* xnet.c */
void     xnet_put_u16(uint8_t* p, uint16_t v);
void     xnet_put_u32(uint8_t* p, uint32_t v);
void     xnet_put_u64(uint8_t* p, uint64_t v);
uint32_t xnet_get_u32(const uint8_t* p);
uint64_t xnet_get_u64(const uint8_t* p);
int      xnet_record_size(int payloadLen);
int      xnet_write_record(uint8_t* out, int outLen, uint64_t ts, uint32_t id, int ext,
                           int type, const uint8_t* payload, int payloadLen);

/* bits.c */
int      bits_last_pos(int start, int len, int motorola);
void     bits_place(uint8_t* buf, int start, int len, int motorola, uint64_t raw);
uint64_t bits_extract(const uint8_t* buf, int start, int len, int motorola);
int      bits_encode(const CanTpSig* s, double phys, uint64_t* raw);   /* -> CANTP_OK / ERR */
double   bits_decode(const CanTpSig* s, uint64_t raw);
int      canfd_pad_len(int len);                                       /* next valid FD length */

#endif
