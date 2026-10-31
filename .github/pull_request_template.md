## What does this PR do?

<!-- one paragraph; link the docs/ milestone it touches if any -->

## Verification

Paste actual results (counts welcome, honest failure rates welcome):

```
make (warnings):        0
make test fresh-disk:   N/M
make test-persist:      PASS/FAIL
make test-exception:    PASS/FAIL
```

## Checklist

- [ ] Build stays warning-free (`-Wall -Wextra`)
- [ ] `make test` passes on a **fresh** `disk.img` (not a reused one)
- [ ] `test-persist` and `test-exception` still pass
- [ ] No secrets, tokens, or machine-specific paths committed
- [ ] Concurrency/scheduler changes: one variable per commit, ≥10 fresh-disk runs measured and reported
- [ ] `docs/` updated when behaviour changes

## Notes for reviewers

<!-- lock-order changes, TLB/IPI assumptions, anything a reviewer should poke -->
