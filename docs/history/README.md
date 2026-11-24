# History

The feasibility study and the milestone pages, kept as they were written.
Each was the plan and the board log of one milestone; status lines in them
("not run on a board yet") are from before the board run that the end of the
page or the merge commit records. Current pages are linked from the
[README](../../README.md).

| Dates | File | What it is | Result |
|---|---|---|---|
| 2026-11-22 | [2026-11-22-feasibility.md](2026-11-22-feasibility.md) | Feasibility study, desk only: shape A (KMDF + charsiu port) against MCDM | shape A chosen |
| 2026-11-22 | [2026-11-22-m2.md](2026-11-22-m2.md) | M2: driver skeleton and `rknputest info` | passed on the TaishanPi 3M, 2026-11-22 |
| 2026-11-22 | [2026-11-22-m3.md](2026-11-22-m3.md) | M3: buffers, submission, interrupts, GEMM against the CPU | passed, 2026-11-22: exact on both cores up to 16x1024x1024 |
| 2026-11-22 to 2026-11-23 | [2026-11-22-m4.md](2026-11-22-m4.md) | M4: charsiu on Windows, Llama-3.2-1B against Linux | run 2026-11-24: CPU arm exact; NPU arm limited by contiguous memory below 4 GiB, which M6 removed |
| 2026-11-24 | [2026-11-24-m6.md](2026-11-24-m6.md) | M6: driver-managed IOMMU, cached buffers, both cores | passed 3 times, 2026-11-24; decode 12.12 tok/s |
| 2026-11-24 | [2026-11-24-m7.md](2026-11-24-m7.md) | M7: where the decode time goes against Linux, and the fixes | merged 2026-11-24: 14.23 tok/s with the defaults, 15.5 to 16.6 in the best arms, Linux 18.15 |
