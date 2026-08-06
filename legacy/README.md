# Legacy Source Code

The `legacy/` directory contains the original validated implementation from which OpenMC v0.1.0 evolved.

This directory is intentionally preserved for three reasons:

1. Reproducibility of the experiments reported in the accompanying SoftwareX publication.
2. Backward compatibility with previously validated execution scripts.
3. Historical reference during the evolution of the project.

The current OpenMC release still relies on this validated implementation.

Future releases will progressively migrate functionality into a more modular architecture while preserving backward compatibility and experimental reproducibility.

Users are encouraged to use the current implementation contained in `src/`.
