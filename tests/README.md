# Host regression tests

Run both production-code contract tests from the repository root:

```bash
make -C tests clean test
```

The UC8179 command-sequence test compiles `EPD/UC8179.c` with GPIO/SPI mocks
and checks the Datasheet-defined partial-refresh contract: five-byte PWR,
two-byte CDI, KW register-LUT mode, DTM1=OLD, DTM2=NEW, balanced
PON/DRF/PTOUT/POF sequencing, and strict PTL validation.

The clock differential test compiles `EPD/epd_clock_diff.c` with renderer and
driver mocks. It checks both default clock layouts, ignores the
`UI_FLAG_PARTIAL_ONLY` mirror, verifies tiled full-HH:MM OLD/NEW writes followed
by one refresh, and confirms a failed refresh does not advance cached OLD state.
