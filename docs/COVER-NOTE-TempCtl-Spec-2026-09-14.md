Subject: Re: Cur Temp controller spec

Scott,

Attached is the requirements spec for the temperature controller as it is in
the build you are running (TempCtl v2.0.1, controller unchanged since v2.0.0).
It is written from the header and the tests, so it describes what the DLL
actually does today, not what I hope it does. Everything is numbered (R5.3,
R7.2, ...) so you can mark up by number.

What it covers, in your order:

- Inputs (17) and outputs (27), with the array positions and units.
- Behaviour around HiLimit / LoLimit, the deadband (HiBand / LoBand and
  DeadbandTimeout), the ErrorTimeout countdown and how it restarts, relay
  feedback, and Init / Reset.
- Every ErrorStatus bit (0..9), what latches it and what it does to control.
- Every TempStatus code and the priority when several apply.
- Every return code of TcStep.

Section 10 maps the seven TempSim scenarios to the rules they exercise, so
you can tie what you see on screen back to a requirement.

Section 11 lists the seven places where I expect you will want "extra rules
around when faults occur": sensor auto-recovery, whether a feedback mismatch
should stop the controller, what to do on disagreement, TempStatus priority,
relays dropping on a NaN before the sensor is declared failed, whether a bad
configuration should force the relays off, and the drive-to-setpoint
behaviour. Marking those up first would let me turn the rest around quickly.

Sources if you want to check anything against the code:

- Header (the normative spec):
  https://github.com/rgravesjr-code/temp-controller-dll/blob/main/src/tempctl.h
- This document in the repo:
  https://github.com/rgravesjr-code/temp-controller-dll/blob/main/docs/TEMPCTL-SPEC-v2.0.1.md
- Release you are running:
  https://github.com/rgravesjr-code/temp-controller-dll/releases/tag/v2.0.1

Roger
