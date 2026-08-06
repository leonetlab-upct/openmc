# Contributing to OpenMC

Thank you for your interest in contributing to OpenMC.

OpenMC is an open-source software framework for packet-level experimentation with Multi-Connectivity (MC) and Forward Erasure Correction (FEC) over real and emulated Non-Terrestrial Networks (NTNs).

We welcome bug reports, documentation improvements, feature requests, and code contributions.

---

## Reporting Issues

Please use GitHub Issues to report:

- software bugs;
- documentation errors;
- installation problems;
- unexpected behaviour;
- feature requests.

When reporting a bug, please include:

- OpenMC version;
- operating system;
- compiler version;
- execution profile;
- steps to reproduce the problem;
- complete error messages.

---

## Contributing Code

The preferred workflow is:

1. Fork the repository.
2. Create a feature branch.
3. Commit your changes with descriptive commit messages.
4. Submit a Pull Request.

Please keep Pull Requests focused on a single improvement.

---

## Coding Style

OpenMC follows:

- C11 for native components;
- Python 3 for monitoring utilities;
- Bash for deployment and validation scripts.

Please keep the coding style consistent with the existing source code.

---

## Validation

Before submitting a Pull Request, please ensure that:

```bash
make check
```

runs successfully.

When applicable, execute the baseline validation:

```bash
sudo ./scripts/validate_phase3.5.sh
```

and verify that the expected results are obtained.

---

## Questions

For questions regarding the project, please contact:

Juan Pedro Muñoz-Gea

juanp.gea@upct.es
