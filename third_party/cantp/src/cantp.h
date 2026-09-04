/*
 * cantp.h - CanTp: generic CAN transport-protocol packer / unpacker
 *
 * Plain C99, no dependencies beyond <string.h>. Builds as cantp.dll (Windows
 * x64/x86) and libcantp.so (Linux x86_64 for cRIO-904x/905x/906x, aarch64 for
 * Raspberry Pi). All exports use the C calling convention (cdecl).
 *
 * Model (Scott's two-call scheme): a message definition - the equivalent of one
 * DBC `BO_` with its `SG_` rows and Vector attributes - is loaded ONCE into a
 * slot with CanTp_Define(). CanTp_Pack() then converts physical values into the
 * CAN frames of the transport sequence on every call; CanTp_Unpack() /
 * CanTp_RxFeed() do the reverse. No DBC is ever parsed inside the library: the
 * host-side tools/dbc2tables.py turns a .dbc into the flat DBL tables below.
 *
 * Frames in and out are NI-XNET raw frame records (the .ncl logfile event
 * layout), little-endian, concatenated in one U8 array:
 *
 *   offset size  field
 *   0      8     Timestamp, U64, 100 ns units since 1601-01-01 UTC (0 = none)
 *   8      4     Identifier; bit 29 (0x20000000) set = 29-bit extended ID
 *   12     1     Type: 0x00 CAN data, 0x10 CAN FD data, 0x18 CAN FD BRS data
 *   13     1     Flags (0)
 *   14     1     Info (0)
 *   15     1     PayloadLength 0..64
 *   16     8+    Payload; records are 24 bytes for PayloadLength <= 8, else
 *                24 + ((PayloadLength - 1) & ~7) bytes (8-byte units)
 *
 * LabVIEW CLFN mapping: const double* / double* -> Array Data Pointer of DBL
 * (+ I32 length); const float* -> SGL; uint8_t* -> U8; int32_t* -> Pointer to
 * Value; int32_t/uint32_t -> Numeric Value; uint64_t -> U64 Value.
 */
#ifndef CANTP_H
#define CANTP_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(CANTP_BUILD)
#    define CANTP_API __declspec(dllexport)
#  else
#    define CANTP_API __declspec(dllimport)
#  endif
#else
#  define CANTP_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CANTP_VERSION_MAJOR 1
#define CANTP_VERSION_MINOR 0
#define CANTP_VERSION_PATCH 0
/* (major << 16) | (minor << 8) | patch */
CANTP_API uint32_t CanTp_Version(void);

/* ------------------------------------------------------------------------ */
/* Return codes                                                              */
/* ------------------------------------------------------------------------ */
#define CANTP_OK              0   /* Unpack/RxFeed: 0 = no complete message (yet) */
#define CANTP_FOUND           1   /* Unpack/RxFeed: a message was decoded into values */
#define CANTP_ERR_ARG        -1   /* null pointer or bad length                      */
#define CANTP_ERR_SLOT       -2   /* slot outside 0..CANTP_MAX_SLOTS-1               */
#define CANTP_ERR_NOT_DEFINED -3  /* slot has no definition                          */
#define CANTP_ERR_MSGDEF     -4   /* bad message definition row                      */
#define CANTP_ERR_SIGDEF     -5   /* bad signal definition row (fit, type, factor 0) */
#define CANTP_ERR_BUFFER     -6   /* out too small; *bytesWritten = bytes needed     */
#define CANTP_ERR_TRANSPORT  -7   /* transport not supported in this release        */
#define CANTP_ERR_RECORD     -8   /* malformed raw frame record in the input         */
#define CANTP_ERR_TOO_MANY   -9   /* more signals than CANTP_MAX_SIGNALS             */

/* ------------------------------------------------------------------------ */
/* Limits                                                                    */
/* ------------------------------------------------------------------------ */
#define CANTP_MAX_SLOTS      32
#define CANTP_MAX_SIGNALS    128
#define CANTP_MAX_PAYLOAD    1785   /* J1939-21 transport limit                */
#define CANTP_RECORD_MIN     24     /* raw record with <= 8 payload bytes      */
#define CANTP_NCL_HEADER_SIZE 12
#define CANTP_XNET_EXTENDED_ID_FLAG 0x20000000u

/* ------------------------------------------------------------------------ */
/* Message definition row (CANTP_MSGDEF_COLS doubles) - one DBC BO_          */
/* ------------------------------------------------------------------------ */
#define CANTP_MSGDEF_COLS 8
enum CanTpMsgDef {
    CANTP_MSG_ID        = 0, /* CAN identifier as written in the DBC (11-bit, or
                                29-bit with priority/PGN/SA; DBC bit 31 ignored) */
    CANTP_MSG_EXTENDED  = 1, /* 0 standard 11-bit, 1 extended 29-bit            */
    CANTP_MSG_LENGTH    = 2, /* payload bytes: 0..8 classic, 0..64 CAN FD,
                                1..1785 J1939 transport                         */
    CANTP_MSG_TRANSPORT = 3, /* CanTpTransport                                  */
    CANTP_MSG_SA        = 4, /* source address override 0..253, or -1 to use the
                                low byte of the ID (J1939 only)                 */
    CANTP_MSG_DA        = 5, /* destination address, 255 = broadcast (J1939)    */
    CANTP_MSG_PAD       = 6, /* fill byte for unused payload (J1939: 255)       */
    CANTP_MSG_CYCLE_MS  = 7  /* informational (DBC GenMsgCycleTime), not used   */
};
enum CanTpTransport {
    CANTP_TP_CLASSIC    = 0, /* one classic CAN frame, DLC = length            */
    CANTP_TP_J1939_BAM  = 1, /* J1939-21 BAM: length <= 8 -> single frame under
                                the PGN, else TP.CM(BAM) + TP.DT packets        */
    CANTP_TP_CANFD      = 2, /* one CAN FD frame (type 0x10)                    */
    CANTP_TP_CANFD_BRS  = 3, /* one CAN FD frame with bit-rate switch (0x18)    */
    CANTP_TP_J1939_RTS  = 4, /* reserved: J1939 RTS/CTS (release 2)             */
    CANTP_TP_ISOTP      = 5  /* reserved: ISO 15765-2 (release 2)               */
};

/* ------------------------------------------------------------------------ */
/* Signal definition row (CANTP_SIGDEF_COLS doubles) - one DBC SG_           */
/* ------------------------------------------------------------------------ */
#define CANTP_SIGDEF_COLS 8
enum CanTpSigDef {
    CANTP_SIG_START     = 0, /* DBC start bit. Intel: LSB position, counted
                                linearly over the whole payload (byte*8+bit).
                                Motorola: MSB position, sawtooth numbering, exactly
                                the number after "SG_ name :" in the .dbc       */
    CANTP_SIG_LENGTH    = 1, /* 1..64 bits                                      */
    CANTP_SIG_ORDER     = 2, /* 0 Intel/little-endian (@1), 1 Motorola (@0)     */
    CANTP_SIG_TYPE      = 3, /* 0 unsigned, 1 signed, 2 IEEE float32 (len 32),
                                3 IEEE float64 (len 64)                         */
    CANTP_SIG_FACTOR    = 4, /* physical = raw * factor + offset (factor != 0)  */
    CANTP_SIG_OFFSET    = 5,
    CANTP_SIG_MIN       = 6, /* physical clamp, ignored when max <= min         */
    CANTP_SIG_MAX       = 7
};

/*
 * Load a message definition into a slot (replaces any previous one).
 *   msgDef   CANTP_MSGDEF_COLS doubles
 *   sigDefs  nSig * CANTP_SIGDEF_COLS doubles, row-major; nSig may be 0
 * Returns CANTP_OK or a negative code; on error the slot is left undefined.
 */
CANTP_API int32_t CanTp_Define(int32_t slot,
                               const double* msgDef, int32_t msgDefLen,
                               const double* sigDefs, int32_t nSig);
CANTP_API int32_t CanTp_Clear(int32_t slot);

/* Queries on a defined slot (negative = error). */
CANTP_API int32_t CanTp_SignalCount(int32_t slot);
CANTP_API int32_t CanTp_PayloadLength(int32_t slot);  /* message bytes         */
CANTP_API int32_t CanTp_FrameCount(int32_t slot);     /* frames per Pack        */
CANTP_API int32_t CanTp_OutputSize(int32_t slot);     /* bytes per Pack         */

/*
 * Pack physical values (one per signal row, in row order) into the raw frame
 * records of one complete transport sequence.
 *   NaN value           -> J1939 "not available": all raw bits 1 (float types: NaN)
 *   value outside min/max (when max > min) -> clamped
 *   raw outside the bit width             -> saturated
 * Frame i gets timestamp `timestamp100ns + i * spacing100ns`.
 * out must hold CanTp_OutputSize(slot) bytes; on CANTP_ERR_BUFFER *bytesWritten
 * receives the size needed.
 */
CANTP_API int32_t CanTp_Pack(int32_t slot, const double* values, int32_t nValues,
                             uint64_t timestamp100ns, uint64_t spacing100ns,
                             uint8_t* out, int32_t outLen, int32_t* bytesWritten);
CANTP_API int32_t CanTp_PackSgl(int32_t slot, const float* values, int32_t nValues,
                                uint64_t timestamp100ns, uint64_t spacing100ns,
                                uint8_t* out, int32_t outLen, int32_t* bytesWritten);

/*
 * Stateless decode over a buffer of raw records (a log, or everything one
 * XNET Read returned). Scans from the start, ignores records that do not
 * belong to the slot's message, reassembles a J1939 transport sequence, and
 * on completion writes one physical value per signal row.
 * Returns CANTP_FOUND (1) and *bytesConsumed = offset just past the completing
 * record, CANTP_OK (0) if no complete message is in the buffer, or an error.
 * Call again from *bytesConsumed to find the next one.
 * Identifier matching: exact for 11-bit and for 29-bit with a source address
 * override; for a 29-bit ID with SA placeholder 0xFE (Vector convention) and
 * no override, any source address matches.
 */
CANTP_API int32_t CanTp_Unpack(int32_t slot, const uint8_t* frames, int32_t framesLen,
                               double* values, int32_t nValues, int32_t* bytesConsumed);
CANTP_API int32_t CanTp_UnpackSgl(int32_t slot, const uint8_t* frames, int32_t framesLen,
                                  float* values, int32_t nValues, int32_t* bytesConsumed);

/*
 * Stateful decode for live buses: feed one raw record per call (frameLen =
 * that record's size). Returns CANTP_FOUND when the record completed a message
 * (values filled), CANTP_OK otherwise. Reassembly state is per slot; a new
 * TP.CM restarts it; CanTp_RxReset discards it.
 */
CANTP_API int32_t CanTp_RxFeed(int32_t slot, const uint8_t* frame, int32_t frameLen,
                               double* values, int32_t nValues);
CANTP_API int32_t CanTp_RxReset(int32_t slot);

/* ------------------------------------------------------------------------ */
/* Raw record helpers                                                        */
/* ------------------------------------------------------------------------ */
/* Size in bytes of the record starting at `rec` (from its PayloadLength), or
 * CANTP_ERR_RECORD if fewer than 24 bytes are available / length is invalid.
 * Lets a caller walk a U8 array of variable-length records. */
CANTP_API int32_t CanTp_RecordSize(const uint8_t* rec, int32_t avail);
/* Size of a record for a given payload length (0..64). */
CANTP_API int32_t CanTp_RecordSizeFor(int32_t payloadLen);
/* Build one raw record from parts (id without the XNET flag bit). */
CANTP_API int32_t CanTp_MakeRecord(uint32_t id, int32_t extended, int32_t frameType,
                                   const uint8_t* payload, int32_t payloadLen,
                                   uint64_t timestamp100ns,
                                   uint8_t* out, int32_t outLen);
/* 12-byte NI-XNET logfile (.ncl) header, little-endian events. */
CANTP_API int32_t CanTp_NclHeader(uint8_t* out, int32_t outLen);

#ifdef __cplusplus
}
#endif
#endif /* CANTP_H */
