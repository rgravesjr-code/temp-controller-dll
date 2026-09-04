# Testing Guide — tempctl

Release gate for v1.0.0: `build.bat all` must finish with `154 passed, 0
failed` for **both** the x64 and the x86 test executables, the Linux build
must succeed, and `tests\oracle_test.py` must print `ALL OK`. `package_dist.bat`
re-runs the Windows gates and records them in `TESTLOG.txt`; it refuses to
package a failing build.

## What the gates cover

### `tests\test_main.c` (154 checks, compiled with the library sources)

Controller (`TcStep`):

- deadband → heat and deadband → cool with countdown values visible in the
  13-element output, relay held until setpoint is reached or crossed
- countdown restarts when the condition clears before expiry
- error countdown, latched fault 1/2, no change while latched, Reset clears,
  countdown restarts from full after Reset with the condition still present
- fault while a relay is running (heater failing, temperature falling
  through LoLimit), and overshoot past the far limit ending a cycle at setpoint
- NaN / Inf readings: relays off at once, ErrorStatus 3 after ErrorTimeout,
  recovery when a valid reading returns in time
- initial relay state from Init (heating wins if both set), live setpoint
  change without re-Init, input relay fields ignored on Step
- zero timeouts act on first observation; 32-bit tick wrap-around
- argument errors (−1..−4), config warning on Init and on every Step,
  in-place operation with `in` and `out` the same 11-element array

J1939 BAM (`TcJ1939Bam`, `TcEncodeFrames`, `TcJ1939BamFrameCount`):

- frame counts for 8, 9, 44, 1785 bytes and rejection of 1786
- TP.CM bytes (control 0x20, size, packet count, 0xFF, PGN LE), TP.CM/TP.DT
  identifiers with priority 7 and DA 0xFF, sequence numbers, per-frame
  timestamp spacing, 0xFF padding, byte-exact reassembly of 11 floats
- single-frame path for PDU2 and PDU1 PGNs, `TC_ERR_BUFFER` reports the
  required size, `.ncl` header bytes, version code

Generic packer (`TcCanPack`):

- Intel and Motorola placement including the DBC sawtooth start bit,
  signed, unsigned, float32, factor/offset, min/max clamp, saturation of
  out-of-range values, DLC overflow rejection, factor 0 and DLC > 8
  rejection, buffer-size error

### `tests\oracle_test.py` (independent implementations)

Loads `tempctl.dll` through ctypes and checks:

- every payload length 0..39 plus 63/64/65/100/255/256/700/1784/1785 is
  reassembled to the original bytes by **pretty_j1939**'s
  `J1939TransportTracker` with the right SA and PGN
- 3000 random single-signal layouts (Intel/Motorola, 1–32 bits, signed,
  unsigned, float32, scaled) produce the same 8 bytes as **cantools**
  `Message.encode`, plus one multi-signal EEC1-style DBC message
- an end-to-end 11-signal encode/decode and a controller smoke test

```bat
pip install cantools pretty_j1939
python tests\oracle_test.py [path\to\tempctl.dll] [--iters 3000] [--seed 1]
```

### On the cRIO

`linux-x64\test_tempctl` is the same 154-check program compiled for x86_64
NI Linux RT. Run it once on the target after copying the .so
(see `LABVIEW_INTEGRATION.md` §2). The .so itself is inspected at package
time with `tools\elfinfo.py` (machine, exports, GLIBC symbol versions); the
result is in `DEPENDENCIES.txt`.

## Regenerating the example log

```bat
python examples\make_sample_ncl.py --dll build\win-x64\tempctl.dll --out examples\sample_tempctl.ncl
```

Runs a 120 s simulated plant through the controller at 100 ms, including a
live setpoint change, a 4 s stuck sensor above HiLimit (fault at 3 s), an
operator reset and recovery, and writes every tick's state as a BAM into an
`.ncl` file plus a CSV trace of temperature, relays and countdowns.
