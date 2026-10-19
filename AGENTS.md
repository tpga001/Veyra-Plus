# Veyra Plus contributor instructions

This is tpga001/Veyra-Plus, a GPL-3.0 fork of Likely7/Veyra-NRVideo. Maintenance branch: browser-enhance; initial upstream base: 2.0.6 / e1f6952a6ab11483fc6ff67177d8d390a8faea2f.

Read README.md, docs/FORK_STATUS.md, docs/BUILD_FORK.md and docs/ROADMAP.md. Inspect Git state before editing. Upstream historical authorizations apply only to their original context, not to this fork.

- Preserve local changes, active applications and user configuration.
- Keep sources on the shared FrameSource -> EnhanceGraph -> FrameSink architecture, with existing texture/color/synchronization/reset contracts.
- Make narrow changes with relevant tests; do not introduce unrelated refactoring or GPU stress/competing workloads.
- Record commands, results, failed attempts and unverified boundaries in docs/WORKLOG.md.
- Inspect inherited test/desktop tools before running; never use the installed application or real user profile as an implicit fixture.
- Distinguish initialization, evaluation, GPU time, submitted FPS and actual displayed FPS. A build or counters cannot prove stable 60 FPS, visual equivalence or physical display behavior.
- Retain LICENSE and per-component attribution in THIRD_PARTY_NOTICES.md.
- Never add proprietary SDKs/headers/samples, DLLs, LIB/PDB files, model weights, archives, credentials, user profiles or private media to Git or Git LFS.
- Do not patch, disguise or re-sign binary files on disk. Component version/hash/signature records must reflect actual bytes. A source fork grants no additional proprietary redistribution rights.
- Commit/push/public release/runtime distribution require authorization in the current task. Historical upstream worklogs do not grant it.
- Keep source publication separate from installation updates; do not stop or replace a running player just to publish source.
