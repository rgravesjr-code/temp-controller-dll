# Fixture model checks

Run `dotnet run --project tests/TempSim.FixtureChecks -c Release` from the
repository root. No third-party test framework is required.

Checks cover energy balance, ambient equilibrium, pump-off heat delivery,
motor-count heat scaling, fan behavior, unit conversion, repeatability,
coast-down, independent pump animation, invalid parameters, JSON persistence,
and isolation of the 16 existing controller scenarios. Failure exits nonzero.

Also run `TempSim.Cli --scenario all` (110 expectations) and the WPF
`--screenshot ... --scenario fixture` / `--scenario failover` gates.
