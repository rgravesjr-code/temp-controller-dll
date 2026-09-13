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
#define CANTP_VERSION_MINOR 2
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
#define CANTP_DONE            2   /* TxStart/TxFeed: the transmit session completed   */
#define CANTP_ERR_TIMEOUT   -10   /* session: peer did not answer in time (aborted)   */
#define CANTP_ERR_ABORTED   -11   /* session: peer aborted / overflow / bad sequence  */
#define CANTP_ERR_BUSY      -12   /* TxStart while a transmit session is running      */
#define CANTP_ERR_MUXDEF    -13   /* bad multiplex definition row                     */
#define CANTP_ERR_FLAT      -14   /* bytes are not a flattened J1939Msg(V4) cluster   */

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
    CANTP_TP_CLASSIC    = 0, /* no transport protocol: one classic CAN frame,
                                DLC = length (11- or 29-bit id)                 */
    CANTP_TP_J1939_BAM  = 1, /* J1939-21 BAM: length <= 8 -> single frame under
                                the PGN, else TP.CM(BAM) + TP.DT packets        */
    CANTP_TP_CANFD      = 2, /* one CAN FD frame (type 0x10)                    */
    CANTP_TP_CANFD_BRS  = 3, /* one CAN FD frame with bit-rate switch (0x18)    */
    CANTP_TP_J1939_RTS  = 4, /* J1939-21 RTS/CTS (destination-specific, DA != 255):
                                session API below; length <= 8 -> single frame  */
    CANTP_TP_ISOTP      = 5  /* ISO 15765-2 on classic CAN, normal addressing:
                                session API below; length <= 7 -> single frame  */
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

/* ------------------------------------------------------------------------ */
/* Multiplexed signals (release 2)                                           */
/* ------------------------------------------------------------------------ */
/*
 * Optional second table for a slot, one row of CANTP_MUXDEF_COLS doubles per
 * signal row of the definition (same order), loaded after CanTp_Define:
 *   [0] multiplexor row index: the row of the signal whose value selects this
 *       one (DBC `m<n>` refers to the `M` signal), or -1 when the signal is
 *       always present (plain signals and the multiplexor itself)
 *   [1] selector value: this signal is present when the multiplexor's raw
 *       value equals it (DBC `m3` -> 3)
 * Pack: unselected signals are left as pad. Unpack: they read as NaN.
 * A multiplexor may itself be multiplexed (extended multiplexing, one level
 * per row, cycles are rejected). CanTp_Define clears the table.
 */
#define CANTP_MUXDEF_COLS 2
CANTP_API int32_t CanTp_DefineMux(int32_t slot, const double* muxDefs, int32_t nSig);

/* ------------------------------------------------------------------------ */
/* Session transports: J1939 RTS/CTS and ISO 15765-2 (release 2)             */
/* ------------------------------------------------------------------------ */
/*
 * These transports need frames from the peer (CTS / flow control) and
 * timers, so a message is a session driven by the caller's loop:
 *
 *   sender:    CanTp_TxStart(values)      -> RTS / FirstFrame in `out`
 *              CanTp_TxFeed(frame, nowMs) -> for every received record (or
 *                                            frame = NULL to run the timers):
 *                                            DT / ConsecutiveFrames in `out`
 *              until CANTP_DONE or a negative code
 *   receiver:  CanTp_RxStep(frame, nowMs) -> for every received record (or
 *                                            NULL): CTS / EndOfMsgAck / flow
 *                                            control in `out`; CANTP_FOUND when
 *                                            the message is complete
 *
 * The record(s) written to `out` (0..N, see *bytesWritten) must be sent
 * whatever the return code; on a negative code they hold the abort frame and
 * the session is over. `out` sized with CanTp_OutputSize(slot) is always
 * enough for one call. nowMs is a free-running millisecond tick (LabVIEW
 * Tick Count); frame timestamps come from timestamp100ns (+ spacing100ns per
 * additional frame).
 *
 * Addresses: J1939 RTS/CTS uses the definition's SA (sender) and DA
 * (receiver) - the receiving node loads the same table and answers from DA.
 * With the SA placeholder 0xFE the receiver accepts any sender. ISO-TP
 * sends data on the definition's id and expects the peer's flow control on
 * the peer id (CanTp_SessionConfig; 29-bit ids default to the id with the
 * two address bytes swapped, 11-bit ids to id + 8, the UDS convention).
 *
 * BAM, classic and CAN FD messages also work through these calls (TxStart
 * emits the whole sequence and returns CANTP_DONE; RxStep behaves like
 * RxFeed), so one loop can serve every transport.
 */

/* Session parameters (CANTP_SESSION_COLS doubles); defaults apply until set:
 *   [0] peer id: identifier carrying the peer's flow control / CTS (ISO-TP;
 *       ignored for J1939 where it is derived from SA/DA). -1 = default
 *   [1] peer id extended (0/1); -1 = same as the definition
 *   [2] receiver block size: CTS packets per round (J1939, 1..255; 0 = all,
 *       capped by the RTS's own limit) / ISO-TP BS (0 = all)
 *   [3] receiver STmin in ms announced in ISO-TP flow control (0..127)
 *   [4] timeout ms waiting for the peer (default 1250 J1939 T3 / 1000 ISO-TP
 *       N_Bs, N_Cr; the DT wait uses 750 for J1939)
 *   [5] sender max packets per CTS offered in the RTS (J1939, 1..255; 0 = 255)
 */
#define CANTP_SESSION_COLS 6
CANTP_API int32_t CanTp_SessionConfig(int32_t slot, const double* cfg, int32_t cfgLen);

CANTP_API int32_t CanTp_TxStart(int32_t slot, const double* values, int32_t nValues, uint32_t nowMs,
                                uint64_t timestamp100ns, uint64_t spacing100ns,
                                uint8_t* out, int32_t outLen, int32_t* bytesWritten);
CANTP_API int32_t CanTp_TxStartSgl(int32_t slot, const float* values, int32_t nValues, uint32_t nowMs,
                                   uint64_t timestamp100ns, uint64_t spacing100ns,
                                   uint8_t* out, int32_t outLen, int32_t* bytesWritten);
CANTP_API int32_t CanTp_TxFeed(int32_t slot, const uint8_t* frame, int32_t frameLen, uint32_t nowMs,
                               uint64_t timestamp100ns, uint64_t spacing100ns,
                               uint8_t* out, int32_t outLen, int32_t* bytesWritten);
/* 0 idle, 1 waiting for the peer, 2 done (until the next TxStart); negative = last error */
CANTP_API int32_t CanTp_TxState(int32_t slot);
CANTP_API int32_t CanTp_TxReset(int32_t slot);

CANTP_API int32_t CanTp_RxStep(int32_t slot, const uint8_t* frame, int32_t frameLen, uint32_t nowMs,
                               uint64_t timestamp100ns, double* values, int32_t nValues,
                               uint8_t* out, int32_t outLen, int32_t* bytesWritten);
CANTP_API int32_t CanTp_RxStepSgl(int32_t slot, const uint8_t* frame, int32_t frameLen, uint32_t nowMs,
                                  uint64_t timestamp100ns, float* values, int32_t nValues,
                                  uint8_t* out, int32_t outLen, int32_t* bytesWritten);
/* 0 idle, 1 a transfer is in progress */
CANTP_API int32_t CanTp_RxState(int32_t slot);

/* ------------------------------------------------------------------------ */
/* Release 3 (v1.2.0): flattened LabVIEW cluster input, frame-length arrays, */
/* one-call read/write                                                       */
/* ------------------------------------------------------------------------ */
/*
 * CanTp_DefineFlat: define a slot from the bytes LabVIEW's "Flatten To
 * String" produces for one "J1939Msg(V4).ctl" cluster (big-endian, array
 * and string sizes prepended - the defaults), which is also the per-message
 * record inside an Eaton .ecd database (tools/ecdflat.py extracts them).
 * Signal order = channel order in the cluster. Used from the cluster:
 * message ID, extended flag, NumDataBytes, UpdateRate (cycle); per channel
 * start bit, number of bits, data type (0 Signed, 1 Unsigned, 2 IEEE Float),
 * byte order (0 Intel, 1 Motorola), scaling factor, offset, min, max,
 * default. Names, descriptions, units and lookup tables are skipped.
 *   transport  CanTpTransport, or -1 = derive: 11-bit id -> classic (CAN FD
 *              above 8 bytes); 29-bit id -> J1939: transport 1 (single frame
 *              under the PGN up to 8 bytes, BAM above), except a PDU1 PGN
 *              addressed to one node (PS not 0xFF / 0xFE): classic frame with
 *              the id verbatim up to 8 bytes, RTS/CTS above
 *   sa         source address override 0..253, or -1 = the id's low byte
 * Priority and destination come from the id as stored in the cluster; pad is
 * 0xFF for 29-bit ids. Returns CANTP_OK, CANTP_ERR_FLAT when the bytes do not
 * parse, or the CanTp_Define codes. CanTp_FlatSize returns the number of
 * bytes one cluster occupies at the start of `flat` (to walk a flattened
 * array of clusters) or an error.
 */
CANTP_API int32_t CanTp_DefineFlat(int32_t slot, const uint8_t* flat, int32_t flatLen,
                                   int32_t transport, int32_t sa);
CANTP_API int32_t CanTp_FlatSize(const uint8_t* flat, int32_t flatLen);

/* Read back the definition of a slot as CanTp_Define rows (either pointer may
 * be NULL). Returns the signal count; at most nSigMax rows are written. */
CANTP_API int32_t CanTp_GetDef(int32_t slot, double* msgDef, int32_t msgDefLen,
                               double* sigDefs, int32_t nSigMax);
/* Channel default values from the cluster (0 for table definitions); returns the signal count. */
CANTP_API int32_t CanTp_Defaults(int32_t slot, double* values, int32_t nValues);

/* Payload length (DLC bytes, 0..64) of every record in `frames`, in order.
 * Returns the record count even when lensLen is smaller (then only the first
 * lensLen are written), or CANTP_ERR_RECORD on a malformed buffer. */
CANTP_API int32_t CanTp_FrameLengths(const uint8_t* frames, int32_t framesLen,
                                     uint8_t* lens, int32_t lensLen);

/*
 * One entry point for both directions, with the frame-length array:
 *   mode CANTP_MODE_WRITE (0): values in -> frames out (the records of one
 *        sequence, TP.CM first, like CanTp_Pack), frameLens out (payload
 *        length per frame), *bytesUsed = bytes written, *nFrames = frames
 *        written. frameLens may be NULL/0. CANTP_ERR_BUFFER when frames or
 *        frameLens is too small (*bytesUsed / *nFrames = sizes needed).
 *   mode CANTP_MODE_READ (1): frames in (records, TP.CM first) -> values out,
 *        like CanTp_Unpack; when frameLensLen > 0 the records are walked with
 *        the caller's lengths (which must agree with the record headers,
 *        else CANTP_ERR_RECORD) and at most frameLensLen records are read.
 *        Returns CANTP_FOUND / CANTP_OK / error; *bytesUsed = bytes
 *        consumed, *nFrames = records consumed.
 * Every array is passed as an Array Data Pointer in LabVIEW; which ones are
 * read or written depends on mode.
 */
#define CANTP_MODE_WRITE 0
#define CANTP_MODE_READ  1
CANTP_API int32_t CanTp_Transfer(int32_t slot, int32_t mode, double* values, int32_t nValues,
                                 uint8_t* frames, int32_t framesLen, uint8_t* frameLens, int32_t frameLensLen,
                                 uint64_t timestamp100ns, uint64_t spacing100ns,
                                 int32_t* bytesUsed, int32_t* nFrames);
CANTP_API int32_t CanTp_TransferSgl(int32_t slot, int32_t mode, float* values, int32_t nValues,
                                    uint8_t* frames, int32_t framesLen, uint8_t* frameLens, int32_t frameLensLen,
                                    uint64_t timestamp100ns, uint64_t spacing100ns,
                                    int32_t* bytesUsed, int32_t* nFrames);

#ifdef __cplusplus
}
#endif
#endif /* CANTP_H */
