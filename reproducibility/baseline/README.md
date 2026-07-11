# Reproducible baseline

This directory reproduces the validated OpenMC v0.1.0 loss-free dual-path baseline over bLEO.

## Run

```bash
sudo ./reproducibility/baseline/run.sh
```

## Expected functional results

```text
Generated datagrams     2000
Delivered datagrams     2000
Completed blocks         250
Decode failures            0
```

The reference profiles are bLEO-specific and can be adapted under `config/`.
