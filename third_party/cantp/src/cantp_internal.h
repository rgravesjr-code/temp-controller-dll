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
#define J1939_RTS_CTRL    0x10u
#define J1939_CTS_CTRL    0x11u
#define J1939_EOMA_CTRL   0x13u
#define J1939_ABORT_CTRL  0xFFu
#define J1939_SA_ANY      0xFEu   /* Vector DBC placeholder */
#define J1939_T1_MS       750u    /* receiver: wait for the next DT */
#define J1939_T3_MS       1250u   /* sender: wait for CTS / EndOfMsgAck */
#define ISOTP_N_MS        1000u   /* N_Bs / N_Cr */
#define ISOTP_PCI_SF      0x0u
#define ISOTP_PCI_FF      0x1u
#define ISOTP_PCI_CF      0x2u
#define ISOTP_PCI_FC      0x3u

typedef struct {
    int      start, len, motorola, type;
    double   factor, offset, min, max;
    double   dflt;                         /* channel default (flat cluster), 0 for tables */
    int      muxRow;                       /* -1 = always present */
    uint64_t muxValue;
} CanTpSig;

typedef struct {
    int      total;                        /* expected bytes, 0 = idle */
    int      packets;
    int      nextSeq;
    int      kind;                         /* RX_BAM / RX_RTS / RX_ISOTP */
    int      windowLeft;                   /* packets still expected before the next CTS / FC */
    int      cfSeq;                        /* ISO-TP: expected next CF sequence (0..15) */
    int      received;                     /* bytes stored (ISO-TP) */
    uint8_t  sa;                           /* sender of the session */
    uint8_t  rtsMaxPackets;                /* sender's limit from the RTS */
    int      retries;                      /* RTS/CTS: re-requests issued for missing packets */
    int      windowEnd;                    /* RTS/CTS: last packet number of the current CTS window */
    uint8_t  seen[256];                    /* RTS/CTS: packets received (index = sequence number) */
    uint32_t lastMs;                       /* time of the last frame, for T1 / N_Cr */
    uint8_t  buf[CANTP_MAX_PAYLOAD];
} CanTpRx;

typedef struct {
    int      state;                        /* TX_IDLE / TX_WAIT / TX_DONE, or negative error */
    int      packets;                      /* total DT / CF packets */
    int      nextSeq;                      /* next packet number to send (1-based) */
    int      sent;                         /* packets sent so far (progress) */
    int      cfSeq;                        /* ISO-TP: next CF sequence number (0..15) */
    uint32_t lastMs;
    uint8_t  peerSa;                       /* J1939: who answered (for placeholder SA) */
    uint8_t  payload[CANTP_MAX_PAYLOAD];
} CanTpTx;

typedef struct {
    uint32_t peerId;                       /* ISO-TP flow control / CTS id (bare) */
    int      peerExt;
    int      blockSize;                    /* receiver: packets per CTS / BS */
    int      stMin;                        /* receiver: ISO-TP STmin (ms) */
    uint32_t timeoutMs;                    /* 0 = protocol default */
    int      maxPerCts;                    /* sender: RTS max packets per CTS */
} CanTpSession;

enum { RX_NONE = 0, RX_BAM = 1, RX_RTS = 2, RX_ISOTP = 3 };
enum { TX_IDLE = 0, TX_WAIT = 1, TX_DONE = 2 };

typedef struct {
    int      used;
    uint32_t id;                           /* bare 11/29-bit id, SA already substituted */
    int      ext;
    int      len;
    int      transport;
    int      saAny;                        /* 29-bit id with SA 0xFE and no override */
    uint8_t  sa, da, pad;
    double   cycleMs;
    int32_t  flatBytes;                    /* bytes of the flattened cluster that defined the slot, 0 for tables */
    int      nSig;
    CanTpSig sig[CANTP_MAX_SIGNALS];
    CanTpRx  rx;
    CanTpTx  tx;
    CanTpSession ses;
} CanTpMsg;

/* cantp.c helpers used by session.c / flat.c */
CanTpMsg* cantp_slot(int32_t slot);
int32_t  cantp_feed_record(CanTpMsg* m, const uint8_t* rec, int recLen, double* values, int32_t nValues); /* no responses */                                        /* slot 0..MAX-1, no checks */
int      is_pdu1(uint32_t pgn);
uint32_t pgn_of_id(uint32_t id);
uint32_t j1939_id(uint32_t pgn, uint8_t sa, uint8_t da, uint8_t priority);
int32_t  pack_values(const CanTpMsg* m, const double* values, int32_t nValues, uint8_t* payload);
void     decode_values(const CanTpMsg* m, const uint8_t* payload, double* values, int32_t nValues);
int      id_matches(const CanTpMsg* m, uint32_t rawId);
int      packets_for(const CanTpMsg* m);                                   /* DT / CF packets of the message */
int32_t  emit_frames(const CanTpMsg* m, const uint8_t* payload, uint64_t ts, uint64_t spacing,
                     uint8_t* out, int32_t outLen, int32_t* bytesWritten);   /* classic / FD / BAM */

/* session.c */
int32_t  session_rx_record(CanTpMsg* m, const uint8_t* rec, int recLen, uint32_t nowMs, uint64_t ts,
                           double* values, int32_t nValues, uint8_t* out, int32_t outLen, int32_t* written);
int32_t  session_rx_timer(CanTpMsg* m, uint32_t nowMs, uint64_t ts, uint8_t* out, int32_t outLen, int32_t* written);
int32_t  session_tx_start(CanTpMsg* m, uint32_t nowMs, uint64_t ts, uint64_t spacing,
                          uint8_t* out, int32_t outLen, int32_t* written);
int32_t  session_tx_feed(CanTpMsg* m, const uint8_t* rec, int recLen, uint32_t nowMs, uint64_t ts, uint64_t spacing,
                         uint8_t* out, int32_t outLen, int32_t* written);
void     session_defaults(CanTpMsg* m);

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
