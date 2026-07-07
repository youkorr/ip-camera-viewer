# Regression tests for the RISC-V PR (tvlabs/edge264 #29)

Requested by Thibault in the issue #28 thread: a short clip for the
crash fix, and a test file for the scalar fast-path fix (commits 2-3 of
the PR) so his planned Q4-2026 multithreading rework doesn't silently
break them.

## 1. Crash repro: `crash-repro-8x8-riscv.264`

Minimal clip (QCIF, ~16 KB) that exercises the original bug: High
profile, CABAC, 8x8 transform enabled (`transform_size_8x8_flag`), a mix
of I/P slices. On RISC-V, `add_idct8x8` (and the other 8x8 kernels)
performed misaligned wide loads/stores that trap on that ISA — this
clip goes through that code path on essentially every macroblock.

```sh
cc -O2 -I.. -I../../../components/h264_hp/edge264/src -o repro \
   ../../../components/h264_hp/edge264/src/edge264.c -DDEBUG main.c # or your own driver
```
Simplest check: decode it with `riscv_regression_check` (below) built
for RISC-V — it must not crash and must produce 20 frame hashes.

## 2. Scalar fast-path regression: `riscv-scalar-fastpaths.264` + `.golden`

30 frames of a static scene (exercises the deblock bS=0 skip fast path
and the full-pel unweighted motion-compensation fast path — the
dominant case on unchanging/surveillance content) followed by 30 frames
of a synthetic pan (global motion, exercises the scalar 6-tap/bilinear
fractional motion-compensation kernels), High profile, CABAC, 8x8
transform. `riscv-scalar-fastpaths.golden` holds one FNV-1a hash per
output frame, recorded from an unmodified x86 SIMD build (ground
truth) — i.e. it is backend-independent, not "the RISC-V answer".

### Running the check

`riscv_regression_check.c` is a ~150-line, dependency-free harness
(only `edge264.h` + libc). Build it against edge264 in whatever
configuration you want to validate — the scalar CLANG backend, a real
RISC-V cross build, or just your native SIMD build as a sanity check —
and diff against the golden file:

```sh
cc -O2 -I<edge264-checkout> -o riscv_regression_check \
   riscv_regression_check.c <edge264-checkout>/src/edge264.c -lpthread -lm

./riscv_regression_check riscv-scalar-fastpaths.264 0 riscv-scalar-fastpaths.golden
# === OK: 60 frames bit-exact vs golden (n_threads=0) ===
```

Any future change that alters a single output pixel on this clip — an
accidental interaction between the multithreading rewrite and the
scalar RISC-V fast paths included in commits 2-3, for instance — makes
this exit 1 with the exact frame index and both hashes.

`n_threads` is forwarded to `edge264_alloc` as-is; we validated at 0
(single-threaded), which is the exact mode the RISC-V scalar fast paths
run in on the ESP32-P4 (`h264_hp_decoder.cpp` always calls `begin(0)` —
see the comment there about why: worker pthreads deadlock against
FreeRTOS on that target). We did not extend validation to `n_threads >
0` for this PR — see the note below.

To (re)record the golden file after an intentional behavioral change:
```sh
./riscv_regression_check riscv-scalar-fastpaths.264 0 > riscv-scalar-fastpaths.golden
```

## Note: unrelated observation, not part of this PR

While building this harness we ran it with `n_threads=1` and `2` out of
curiosity and hit a segfault in `worker_loop` -> `parse_slice_data_cabac`
-> `decode_inter`, reproducing on a completely unmodified checkout of
`master` with our own test clip above — so it looks unrelated to
anything in this PR (which never touches that code path: the scalar
kernels are `#ifdef __riscv`-gated and untouched on x86). We have **not**
root-caused it and can't rule out a misuse of the multithreaded API in
our minimal harness (missing setup, wrong teardown ordering, etc.) — it
is not included as a claim or a regression here, just a heads-up in
case it's useful. Happy to share the harness/repro separately if wanted,
but did not want to muddy this PR with an unverified, unrelated report.
