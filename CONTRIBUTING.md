# Contributing

## Commit message convention
We enforce Conventional Commits in CI and via a local commit-msg hook.

Allowed types:
- feat, fix, chore, docs, refactor, perf, ci, build, style, revert, test

Rules:
- Format: `<type>(<scope>)?: <subject>`
- Subject line length: **max 72 characters**

Examples:
- `feat(cli): improve --help output`
- `fix: handle missing output path`

## Local setup (recommended)
We use pre-commit to install a commit-msg hook.

Option (script):
```
./scripts/setup-dev.sh
```

## CI
CI runs `scripts/ci/commit_checker.py` on every push and will fail if any commit
message is not Conventional or exceeds the subject length limit.
