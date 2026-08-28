Vendored from https://github.com/philippwaller/esphome-epaper-spectra6-133,
tag `v0.4.0` (commit `b601838`), licensed Apache License 2.0 (see `LICENSE`
in this directory).

## Local changes

- `epaper_spectra6_133.cpp`: `REFRESH_BUSY_TIMEOUT_US` bumped from 20s to 60s.
  Upstream's value was too short for the Good Display GDEB0709E01 panel on
  real hardware — see the in-file comment at the constant, and
  `../../reference/esphome-t133a01-driver/NOTES.md` for the full writeup.

Switch back to the upstream `type: git` external_components source (see the
commented block in `../../gdeb0709e01-first-test.yaml`) once this fix lands
there, or drop this vendored copy in favor of a proper fork if more local
changes accumulate.
