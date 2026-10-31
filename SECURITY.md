# Security Policy

## Reporting

Please report suspected vulnerabilities privately to **nanjuong@outlook.com**
(or via GitHub Security Advisories: "Security" tab -> "Report a vulnerability")
instead of a public issue.

## Never commit secrets

This repository must not contain credentials, tokens, private keys, personal
e-mail addresses, machine-specific paths, or disk images. `.gitignore` already
blocks the usual suspects (`.env*`, `*.pem`, `*.key`, `id_*`, `secrets/`,
build artifacts, QEMU logs, `disk.img`).

If you accidentally commit a secret:

1. rotate/revoke it immediately at the provider;
2. rewrite history before pushing (or ask a maintainer to do it);
3. never rely on deleting a later commit - the blob stays in history.

## Scope

The kernel runs with full privileges in QEMU; only run test images you built
yourself. Do not boot untrusted `lantest.iso` images.
