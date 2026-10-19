# 开发与构建

这是源码仓库，不包含开箱即用运行环境。先阅读 [上游 2.0.6 构建说明](BUILD_2.0.6.md) 和根目录 CMakeLists.txt。

- 需要 Windows x64、MSVC C++20、Windows SDK、CMake；QML 界面需要匹配的 Qt 6 与部署插件。
- 媒体解码、音频、ASS/SSA 依赖和 patched FFmpeg 要求见上游构建记录，保留其补丁与许可证。
- DLSS SR/NR/FG、NVOF、RTX Video、XeSS、FSR 按 `VEYRA_ENABLE_*` 和 `*_ROOT` 使用外部 SDK。源码未包含这些 SDK、DLL 或模型。
- SF310.9.16 的打包身份支持不等于提供 DLL 或完整运行环境；需要单独准备匹配依赖。
- 构建输出和测试 profile 使用独立目录，不要指向日常安装版及用户数据。

## 浏览器扩展

加载未打包的 `browser-extension/` 并运行支持本协议的原生程序，详见[扩展说明](../browser-extension/README.md)。扩展只传递元数据，不能独立增强，也不取得原始解码帧。

扩展 CPU 测试无需安装 npm 依赖，在仓库根目录运行：

```text
node --test browser-extension/tests/*.test.cjs
```

C++ 测试由 CMake 对应 target 提供，需明确依赖、显卡和验收范围。继承的部分工具仍带历史路径约束，执行前必须检查。13 个只为本机阶段环境编写的 UI/注册表验收脚本未发布；产品源码、单元测试和集成测试保留。恢复这些工具时应参数化输入并保留防误操作边界。

本次建仓不重新构建或运行产品，不将静态/扩展检查冒充新的 GPU 画质和显示帧率验收。
