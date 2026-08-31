# Release procedure

This document describes the procedure for preparing, publishing, and
verifying an OpenMC release.

## 1. Prepare the release

Before creating a new release, update the release metadata consistently:

- `VERSION`
- `CITATION.cff`
- `CHANGELOG.md`
- version references in `README.md` and the documentation, where applicable

The version number and release date must be consistent across these files.

For releases associated with publication experiments, verify that the
documentation also points to the corresponding archived reproducibility
dataset.

## 2. Check the repository

Before creating the release tag, verify that no local, generated, or
temporary artifacts are tracked in the repository.

In particular, the repository should not contain Python cache files,
locally compiled binaries, temporary backups, or local experimental data.

A useful check is:

```bash
find . -type f | grep -E \
'(__pycache__|\.pyc$|_ant\.(c|py)$|CHANGELOG\.md\.tmp)'
```

The command should produce no output.

Raw and processed experimental data associated with the SoftwareX
evaluation are archived separately in Zenodo and should not be duplicated
in the source-code repository.

## 3. Validate the source tree

Run the standard source-tree checks:

```bash
make check
```

Check the Python scripts used by the experimental and reproducibility
workflows:

```bash
python3 -m py_compile \
  scripts/collect_resources.py \
  scripts/set_bleo_delay.py \
  scripts/run_experiment.py \
  scripts/run_matrix.py \
  reproducibility/analysis/common.py \
  reproducibility/analysis/validate_runs.py \
  reproducibility/analysis/aggregate_results.py \
  reproducibility/analysis/generate_figures.py \
  reproducibility/analysis/freeze_fig2.py \
  reproducibility/analysis/freeze_fig3.py \
  reproducibility/validation/validate_instrumentation.py
```

## 4. Run the test suite

In a correctly configured OpenMC/bLEO experimental environment, run:

```bash
sudo ./tests/run_tests.sh
```

The test runner executes the smoke and baseline validation workflows
documented in `tests/README.md`.

Review the output and confirm that the tests complete successfully before
creating the release.

## 5. Verify release metadata

Check the version:

```bash
cat VERSION
```

Check the citation metadata:

```bash
grep -n -E 'version:|date-released:' CITATION.cff
```

Check the corresponding release entry in the changelog:

```bash
head -n 80 CHANGELOG.md
```

The version and release date should agree across `VERSION`,
`CITATION.cff`, and `CHANGELOG.md`.

## 6. Create the GitHub release

Once all validation checks have passed:

1. Open the GitHub repository.
2. Select **Releases**.
3. Select **Draft a new release**.
4. Create a new tag using the release version, for example `v0.1.1`.
5. Target the final validated commit on the `main` branch.
6. Use `OpenMC vX.Y.Z` as the release title.
7. Prepare the release notes from the corresponding `CHANGELOG.md` entry.
8. Review the tag, target commit, title, and release notes.
9. Publish the release.

GitHub automatically provides source-code archives for the tagged release.

## 7. Verify the published release

After publication, verify that the tag and release are publicly available
and point to the intended commit.

Using Git:

```bash
git fetch --tags
git tag --list
git show vX.Y.Z
```

Replace `vX.Y.Z` with the published release tag.

Also verify that the GitHub release page displays the expected version,
release notes, and source-code archives.

## 8. Publication reproducibility

For releases associated with the SoftwareX publication, the frozen raw and
processed experimental data are archived separately on Zenodo:

https://doi.org/10.5281/zenodo.22142700

The GitHub repository contains the OpenMC source code, experimental
orchestration, configurations, analysis scripts, validation tools, and
documentation. Zenodo provides the frozen experimental data and
publication-level results.

This separation keeps the software repository focused on maintained source
code while providing a persistent and independently archived record of the
experimental results.
