# Contributing to Lantest OS

Thanks for your interest! Lantest OS is an independent x86_64 SMP kernel
written from scratch — no Linux source code, no Linux ABI.

## Ground rules

1. **Independent implementation.** Do not copy code from Linux, xv6, or any
   other kernel. Design inspiration is welcome; source or ABI compatibility
   is explicitly out of scope (see `README.md`).
2. **Keep the tests green.** `make test` (fresh disk), `make test-persist`,
   and `make test-exception` must pass before you open a PR. The build must
   stay **warning-free** (`-Wall -Wextra`).
3. **One variable per commit.** For concurrency or scheduler changes, apply
   one patch at a time and run the fresh-disk test at least 3x (10x for
   lock/scheduler work). Note the exact numbers in the commit message —
   honest failure rates are welcome; unverifiable claims are not.
4. **No secrets, no machine paths.** Never commit tokens, private keys,
   personal filesystem paths, or disk images. See `SECURITY.md`.

## Environment

```bash
sudo apt install build-essential qemu-system-x86 grub-pc-bin xorriso mtools
make && make test
```

CI runs the same tests under QEMU TCG with `QEMU_TIMEOUT=240`.

## Workflow

- Fork → branch → commit (see commit-message style in `git log`) → PR.
- PRs link the milestone doc they touch (`docs/M4-*.md`); update or add a
  docs file when behaviour changes.
- By submitting a patch you agree to license it under Apache-2.0
  (`LICENSE` §5).

## Reporting bugs

Open an issue with: QEMU version, host, `make test` output (or the
`serial.log` artifact), and whether the failure reproduces on a fresh
`disk.img` (`rm -f disk.img && truncate -s 16M disk.img`).
