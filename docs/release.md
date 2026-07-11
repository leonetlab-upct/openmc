# Release procedure

The prepared Git bundle contains the initial commit and annotated tag `v0.1.0`.

## Publish while preserving the tag

```bash
git clone openmc-v0.1.0.git.bundle openmc
cd openmc
git remote add origin https://github.com/leonetlab-upct/openmc.git
git push -u origin main
git push origin v0.1.0
```

## Verify

```bash
git status
git tag --list v0.1.0
git show v0.1.0
make check
./scripts/audit_phase3.sh
```

The annotated tag is not cryptographically signed.
