# Veyra Plus

基于 [Likely7/Veyra-NRVideo](https://github.com/Likely7/Veyra-NRVideo) 的个人维护分支，侧重浏览器视频增强与本地播放体验。保留原作者版权及第三方许可证。

[English](README_EN.md) · [功能状态](docs/FORK_STATUS.md) · [开发与构建](docs/BUILD_FORK.md) · [后续计划](docs/ROADMAP.md)

## 目前包含

- **浏览器区域增强**：Chrome/Edge 扩展识别主视频，原生程序捕获视频区域并覆盖显示，保持网页布局与鼠标操作。
- **常驻与独立播放**：一个托盘常驻实例服务浏览器；本地视频使用独立进程，隐藏最近播放与继续播放记录。
- **画质与监控**：共享 SR、NR、FG 管线，阶段耗时、处理尺寸、预览与增强前后分割对比；区分提交 FPS 与实际显示 FPS。
- **基础美颜**：GPUPixel 方程的 D3D12 移植，包含磨皮和细化后的美白量程，附 Apache-2.0 许可与归因。
- **字幕与桌面体验**：本地字幕、ASSRT 在线字幕入口、极简固定底栏、全屏控制栏、最小化收入托盘及用户级文件关联。
- **浏览器 RTX 性能选项**：按页面报告的原片尺寸处理，再超分到视频显示区域，保持区域大小不变。
- **NR 更新支持**：打包脚本已记录 SF310.9.16 的身份；社区实验 DLL 不存放于源码中。

## 分支和版本

维护分支为 **browser-enhance**，初始基线是上游 **2.0.6 / e1f6952a6ab11483fc6ff67177d8d390a8faea2f**，承接本地功能改动。`main` 保留 Fork 时的上游分支。

本次没有合并上游 2.0.9 全部源码；NR 运行库更新与完整版本升级不同。**本 Fork 当前只发布源码，尚未发布安装包。** 上游发行版不等同于本 Fork。

## 使用入口与限制

浏览器扩展见 [扩展说明](browser-extension/README.md)，必须同时运行支持本协议的 Veyra 原生程序。扩展只传元数据，尚未获得浏览器原始解码帧。

- 偶发卡顿仍待定位；平均 FPS、提交计数和 GPU 时间不能证明稳定 60 FPS。
- ASSRT 需要用户自己的 Token；真实账户的公网端到端验收待完成。
- 源码不含 SDK、运行库、模型、私人媒体或用户配置。克隆仓库不等于获得完整安装环境。
- 本机阶段报告、截图、日志和机器专用验收脚本未上传。保留通用测试；构建边界见 [开发说明](docs/BUILD_FORK.md)。

## 许可与来源

保持 [GPL-3.0](LICENSE)，第三方来源见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。[上游固定基线介绍](https://github.com/Likely7/Veyra-NRVideo/blob/e1f6952a6ab11483fc6ff67177d8d390a8faea2f/README.md)。

SDK、运行库和模型各自的许可独立适用，GPL 不会扩大其分发权。本 Fork 不宣称 NVIDIA 官方认证、合作或完整官方 DLSS 5 集成。

可选 PS5/Chiaki 组合构建还需遵守第三方说明中的 AGPL-3.0-only 与上游 OpenSSL 例外，不能只以根目录 GPL 文件代替组合程序的许可记录。
