# Validation

## Portable checks

```bash
make check
./scripts/audit_phase3.sh
bash -n scripts/*.sh
python3 -m py_compile src/monitoring/path_monitor.py
```

## End-to-end validation

With bLEO running:

```bash
sudo ./scripts/validate_phase3.5.sh
```

The validator compares legacy positional arguments, explicit long-form arguments, and reusable runtime profiles for both backends.

## Acceptance criteria

```text
Generated datagrams     2000
Delivered datagrams     2000
Completed blocks         250
Decode failures            0
```

Timing values can vary slightly between hosts. Packet delivery, block completion, and decoding failures are the strict functional criteria.
