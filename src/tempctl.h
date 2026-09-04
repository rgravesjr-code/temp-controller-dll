/*
 * tempctl.h - Temperature controller + J1939 BAM / NI-XNET raw frame encoder
 *
 * Plain C99, no dependencies beyond <string.h>. Builds as tempctl.dll (Windows)
 * and libtempctl.so (NI Linux RT x64, cRIO-9045). All exports use the C calling
 * convention (cdecl); on x64 that is the only convention, so LabVIEW's Call
 * Library Function Node must be set to "C" calling convention.
 *
 * LabVIEW CLFN mapping:
 *   const float* / float*    -> Array Data Pointer of SGL, plus an I32 length
 *   const double*            -> Array Data Pointer of DBL
 *   uint8_t*                 -> Array Data Pointer of U8
 *   int32_t*                 -> Pointer to Value (I32)
 *   int32_t / uint32_t       -> Numeric, Value
 *   uint64_t                 -> Numeric U64, Value
 *
 * Return value of every function is TC_OK (0), a negative TC_ERR_* code, or a
 * positive TC_WARN_* code (call succeeded, but something is worth a look).
 */
#ifndef TEMPCTL_H
#define TEMPCTL_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(TEMPCTL_BUILD)
#    define TC_API __declspec(dllexport)
#  else
#    define TC_API __declspec(dllimport)
#  endif
#else
#  define TC_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Version                                                                   */
/* ------------------------------------------------------------------------ */
#define TC_VERSION_MAJOR 1
#define TC_VERSION_MINOR 0
#define TC_VERSION_PATCH 0
/* Returns (major << 16) | (minor << 8) | patch. */
TC_API uint32_t TcVersion(void);

/* ------------------------------------------------------------------------ */
/* Return codes                                                              */
/* ------------------------------------------------------------------------ */
#define TC_OK                 0
#define TC_WARN_CONFIG        1   /* init/step: config not LoLimit < LoDeadband <= Setpoint <= HiDeadband < HiLimit, or negative timeout */
#define TC_ERR_ARG           -1   /* null pointer or bad length                */
#define TC_ERR_ZONE          -2   /* zone index out of range                   */
#define TC_ERR_ACTION        -3   /* unknown action                            */
#define TC_ERR_NOT_INIT      -4   /* step called before init                   */
#define TC_ERR_BUFFER        -5   /* output buffer too small; *bytesWritten = need */
#define TC_ERR_PAYLOAD       -6   /* payload > 1785 bytes (J1939 TP limit)     */
#define TC_ERR_SIGDEF        -7   /* generic packer: bad signal definition     */
#define TC_ERR_FRAMEDEF      -8   /* generic packer: bad frame definition      */

/* ------------------------------------------------------------------------ */
/* Temperature controller                                                    */
/* ------------------------------------------------------------------------ */

/* Signal order in the SGL array, in and out. */
enum TcSignal {
    TC_HI_LIMIT        = 0,  /* deg, above this for ErrorTimeout ms -> fault 1 */
    TC_LO_LIMIT        = 1,  /* deg, below this for ErrorTimeout ms -> fault 2 */
    TC_HI_DEADBAND     = 2,  /* deg, absolute; above for DeadbandTimeout -> cool */
    TC_LO_DEADBAND     = 3,  /* deg, absolute; below for DeadbandTimeout -> heat */
    TC_SETPOINT        = 4,  /* deg, heat/cool run until temp reaches this     */
    TC_ACTUAL_TEMP     = 5,  /* deg, measured                                  */
    TC_ERROR_TIMEOUT   = 6,  /* ms                                             */
    TC_DEADBAND_TIMEOUT= 7,  /* ms                                             */
    TC_COOLING_ACTIVE  = 8,  /* 0/1 (in: initial state at init; out: relay)    */
    TC_HEATING_ACTIVE  = 9,  /* 0/1 (in: initial state at init; out: relay)    */
    TC_ERROR_STATUS    = 10, /* out: 0 none, 1 hi limit, 2 lo limit, 3 bad reading */
    TC_SIGNAL_COUNT    = 11,
    /* Optional extra outputs, written only when outLen >= TC_SIGNAL_COUNT_EXT */
    TC_ERROR_REMAIN_MS = 11, /* remaining error countdown, 0 when not counting */
    TC_DB_REMAIN_MS    = 12, /* remaining deadband countdown, 0 when idle      */
    TC_SIGNAL_COUNT_EXT= 13
};

enum TcAction {
    TC_ACTION_INIT  = 0,  /* clear fault/timers, take relay state from in[8..9] */
    TC_ACTION_STEP  = 1,  /* run one control tick                               */
    TC_ACTION_RESET = 2   /* clear fault/timers, relays off, keep config        */
};

#define TC_MAX_ZONES 16

/*
 * One controller tick.
 *   zone    0..TC_MAX_ZONES-1, independent controller instances.
 *   action  TcAction.
 *   nowMs   free-running millisecond tick (LabVIEW Tick Count (ms)). The
 *           library differences successive values, wrap-around is handled.
 *   in      TC_SIGNAL_COUNT SGL values, order per TcSignal. All configuration
 *           fields are read on every call, so a setpoint or limit change takes
 *           effect on the next step with no re-init.
 *   out     TC_SIGNAL_COUNT (or TC_SIGNAL_COUNT_EXT) SGL values. May be the
 *           same array as `in` (in-place update).
 *
 * Behaviour (STEP):
 *   - Fault latched: once ActualTemp has been above HiLimit (or below LoLimit,
 *     or NaN/Inf) continuously for ErrorTimeout ms, ErrorStatus = 1/2/3, both
 *     relays off, and nothing changes until RESET (or INIT).
 *   - A NaN/Inf reading also drops both relays immediately while the error
 *     countdown runs.
 *   - Countdowns restart from their full value whenever their condition clears.
 *   - Idle: if temp > HiDeadband for DeadbandTimeout ms -> cooling on;
 *           if temp < LoDeadband for DeadbandTimeout ms -> heating on.
 *   - Heating stays on until temp >= Setpoint, cooling until temp <= Setpoint.
 *   - Heating and cooling are mutually exclusive.
 */
TC_API int32_t TcStep(int32_t zone, int32_t action, uint32_t nowMs,
                      const float* in, int32_t inLen,
                      float* out, int32_t outLen);

/* ------------------------------------------------------------------------ */
/* NI-XNET raw frame / .ncl output                                           */
/* ------------------------------------------------------------------------ */
/*
 * Every frame produced by the encoders below is one NI-XNET raw frame record,
 * identical to an NI-XNET logfile (.ncl) event record, little-endian:
 *
 *   offset size  field
 *   0      8     Timestamp, U64, 100 ns units (NI epoch 1601-01-01 UTC; 0 = none)
 *   8      4     Identifier, U32; bit 29 (0x20000000) set = 29-bit extended ID
 *   12     1     Type   (0x00 = CAN data frame)
 *   13     1     Flags  (0)
 *   14     1     Info   (0)
 *   15     1     PayloadLength (0..8 for classic CAN)
 *   16     8     Payload, unused bytes 0
 *
 * TC_RAW_FRAME_SIZE bytes per classic CAN frame. The concatenated records can
 * be handed to XNET Write (Frame Output Stream, raw) or appended to a file
 * after the 12-byte header from TcNclHeader().
 */
#define TC_RAW_FRAME_SIZE 24
#define TC_NCL_HEADER_SIZE 12
#define TC_XNET_EXTENDED_ID_FLAG 0x20000000u

/* Writes the 12-byte NI-XNET logfile header (little-endian events). */
TC_API int32_t TcNclHeader(uint8_t* out, int32_t outLen);

/* ------------------------------------------------------------------------ */
/* J1939 broadcast transport (BAM)                                           */
/* ------------------------------------------------------------------------ */
/*
 * Builds the J1939 frames that carry `payload` from source address `sa`
 * under parameter group `pgn`:
 *   len <= 8   : one frame with the PGN itself (PDU2 assumed, priority `priority`)
 *   len 9..1785: TP.CM BAM (PGN 0xEC00, DA 0xFF) followed by N TP.DT
 *                (PGN 0xEB00) packets of 7 data bytes, last packet padded 0xFF.
 *                TP frames use priority 7 as J1939-21 specifies.
 * Frame i gets timestamp `timestamp100ns + i * spacing100ns` (pass 0/0 to
 * leave timestamps zero). J1939-21 asks for 50..200 ms between BAM packets;
 * the caller paces the actual bus writes, XNET ignores timestamps for normal
 * stream output.
 *
 * out must hold TcJ1939BamFrameCount(len) * TC_RAW_FRAME_SIZE bytes; on
 * TC_ERR_BUFFER, *bytesWritten receives the required size.
 */
TC_API int32_t TcJ1939BamFrameCount(int32_t payloadLen);
TC_API int32_t TcJ1939Bam(uint32_t pgn, uint8_t sa, uint8_t priority,
                          const uint8_t* payload, int32_t payloadLen,
                          uint64_t timestamp100ns, uint64_t spacing100ns,
                          uint8_t* out, int32_t outLen, int32_t* bytesWritten);

/*
 * Temperature-controller convenience: sends the SGL array as raw IEEE-754
 * little-endian floats (4 bytes each, so 11 signals = 44 bytes = 1 TP.CM +
 * 7 TP.DT). Same framing rules and arguments as TcJ1939Bam.
 */
TC_API int32_t TcEncodeFrames(const float* signals, int32_t n,
                              uint32_t pgn, uint8_t sa, uint8_t priority,
                              uint64_t timestamp100ns, uint64_t spacing100ns,
                              uint8_t* out, int32_t outLen, int32_t* bytesWritten);

/* Defaults used by the README / examples (Proprietary B, PGN 65280). */
#define TC_DEFAULT_PGN       0xFF00u
#define TC_DEFAULT_SA        0x80u
#define TC_DEFAULT_PRIORITY  6u

/* ------------------------------------------------------------------------ */
/* Generic DBC-style packer                                                  */
/* ------------------------------------------------------------------------ */
/*
 * sigDefs: nSig rows of TC_SIGDEF_COLS doubles (DBC semantics):
 *   [0] frameIndex   row in frameDefs this signal lives in
 *   [1] startBit     DBC start bit (Intel: LSB position; Motorola: MSB position,
 *                    sawtooth numbering, exactly as in a .dbc file)
 *   [2] bitLength    1..64
 *   [3] byteOrder    0 = Intel / little-endian, 1 = Motorola / big-endian
 *   [4] valueType    0 = unsigned, 1 = signed (two's complement),
 *                    2 = IEEE float32 (bitLength 32), 3 = IEEE float64 (64)
 *   [5] factor       physical = raw * factor + offset
 *   [6] offset
 *   [7] min          physical clamp, ignored when max <= min
 *   [8] max
 * frameDefs: nFrames rows of TC_FRAMEDEF_COLS doubles:
 *   [0] arbitrationId  11- or 29-bit CAN identifier (no XNET flag bit)
 *   [1] extended       0 = standard, 1 = extended
 *   [2] dlc            0..8 payload bytes
 *   [3] cycleMs        reserved (echoed nowhere yet), pass 0
 * values: nSig physical values, same order as sigDefs.
 *
 * Every frame is emitted (unused bits 0), in frameDefs order, each one a
 * TC_RAW_FRAME_SIZE record with timestamp `timestamp100ns`.
 */
#define TC_SIGDEF_COLS   9
#define TC_FRAMEDEF_COLS 4
TC_API int32_t TcCanPack(const double* sigDefs, int32_t nSig,
                         const double* frameDefs, int32_t nFrames,
                         const float* values, int32_t nValues,
                         uint64_t timestamp100ns,
                         uint8_t* out, int32_t outLen, int32_t* bytesWritten);

#ifdef __cplusplus
}
#endif
#endif /* TEMPCTL_H */
