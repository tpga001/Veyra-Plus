# Veyra Plus

A personal GPL-3.0 fork of [Likely7/Veyra-NRVideo](https://github.com/Likely7/Veyra-NRVideo), focused on browser-video enhancement and local playback. Original copyrights and third-party notices are retained.

[中文](README.md) · [Status](docs/FORK_STATUS.md) · [Build notes](docs/BUILD_FORK.md) · [Roadmap](docs/ROADMAP.md)

Included: Chrome/Edge metadata extension and native region capture/overlay; resident browser service with independent local-player processes; GPU preview/split comparison/monitoring; a D3D12 port of GPUPixel basic beauty; local subtitles and ASSRT integration; compact/fullscreen/tray controls; source-sized browser RTX SR; SF310.9.16 NR packaging identity support.

The maintenance branch, **browser-enhance**, starts from upstream **2.0.6 / e1f6952a6ab11483fc6ff67177d8d390a8faea2f** with local changes carried forward. `main` retains upstream at fork time. This is not a complete 2.0.9 source integration. **No installer release has been published by this fork.**

The extension sends metadata, not decoded video frames, and needs the native application running. Intermittent stutter remains under investigation. Submission FPS and GPU time do not prove displayed FPS or stable 60 FPS. ASSRT needs a user-supplied token; live-account end-to-end validation is pending.

Private media, SDKs, runtime binaries, models, user profiles, logs and machine-specific acceptance tools are excluded from this publication. See build notes before preparing dependencies or running inherited desktop tools.

[GPL-3.0](LICENSE) · [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Proprietary components retain their separate terms. This project does not claim NVIDIA certification, partnership or complete official DLSS 5 integration.

Optional PS5/Chiaki combined builds must also follow the AGPL-3.0-only terms and upstream OpenSSL exception recorded in the third-party notices; the root GPL file alone is not their complete license record.
