# Veyra 浏览器视频增强 0.2.3

扩展仅上报视频位置和播放状态。捕获、超分、画面重塑和补帧由 Veyra 本体处理。
扩展不读取视频像素、不启动应用，不修改旧 Magpie 扩展。

## 使用

在 Chrome/Edge 扩展页面开启开发者模式，加载本目录。首次安装关闭。
打开支持本协议的 Veyra，在扩展弹窗开启视频增强，或者在 Veyra 中开启浏览器增强。
只增强选定标签页中的主视频。浏览器前台时可正常绑定；Veyra 监控界面在前台时，仅继续原先绑定且仍可见的同一视频。ON 是等待状态，RUN 和“正在增强”以原生状态为准。
Veyra 关闭或关闭增强后同步关闭扩展状态，视频心跳不会启用或重启应用。
后台页面也可只读确认 Veyra 是否在线；后台不会提交视频捕获或开启增强。
弹窗区分原视频像素与实际捕获区域，显示原生状态、未增强原因和真实连接错误。

视频暂停或结束时停止增强并保留原片源身份和最近参数；新的同一片源播放报告可在监控界面前台恢复。切换应用、标签页、文档或片源，关闭增强或导航后，必须重新通过浏览器前台绑定。覆盖层仅位于浏览器正上方，始终处于监控界面和其他较高窗口下方；完全被遮挡或移出屏幕时停止增强。

## 接口

固定端点 `http://127.0.0.1:24861`。只有扩展 worker 访问，网页与 worker 只交换元数据。

- `POST /v1/status {schema:1}`：只读查询原生真实状态，必须返回布尔 `enabled`，不改变任何开关。
  扩展使用POST以保留浏览器生成的固定扩展Origin；GET可能省略Origin，不能依靠GET查询通过身份校验。
- `POST /v1/control`：仅 popup 显式操作，`{schema:1,enabled:bool}`。
- `POST /v1/video`：`schema/clientId/sessionId/sequence/type(video|stop)`。
  `rect` 为 CSS left/top/width/height；`viewport` 为 innerWidth/innerHeight/devicePixelRatio/pageZoom/visualViewport/screen/outer。
  `sourceSize` 是原视频分辨率；`windowInfo` 为 chrome.windows DIP left/top/width/height/focused，以及浏览器window/tab/document标识、页面标题和网址。
  可选 `mediaInfo:{videoId,sourceEpoch}` 不含地址：videoId 对每个 DOM video 元素稳定，sourceEpoch 为非负安全整数。
  真实换源/重新加载、seek递增epoch，普通播放/恢复、重复loadedmetadata及心跳不递增；原生据此清空时序历史。
  `uiRects` 为视频内归一化 left/top/right/bottom 数组；`frameRate` 为原视频 rVFC 实测值，未知为0。
  stop 仅携带身份、递增sequence、type、reason。
- 原生仅绑定 loopback，所有扩展请求核对固定扩展 Origin，拒绝网页和null Origin，JSON POST 支持 OPTIONS。
- 150ms首事件节流、1s元数据心跳及500ms页面轮询、单元素最新消息槽、400ms临时失焦宽限；原生仍须复核前台来源和报告有效期。
  worker的同包去重仅500ms，独立于2s只读status轮询；避免两层2s阈值加定时抖动超过原生2.5s有效期。
  已确认online/ON的视频报告不串行等待status；未知/OFF仍先读原生状态。status/video共用请求发起序号与已接受响应水位，旧响应/错误不能覆盖更新请求的已确认原生结果，迟到ON不能复活OFF。
- 只读status不受前台门控阻断；`POST /v1/video`必须通过原生前台和绑定来源复核。普通前台报告要求当前聚焦window/tab；监控续报要求原生`ownMonitorForeground:true`及精确匹配的`boundBrowserWindowId/boundTabId/boundDocumentId/boundVideoId/boundSourceEpoch`，授权最多保留2.5秒，worker不会在请求里声称自己有前台权限。
  未曾捕获的后台页面不会反复发送stop；另一后台浏览器窗口的tab激活不会误停当前源。
- 扩展本地诊断仅保留当前来源的检测/播放/前台状态、尺寸和时间，不保存地址或标题历史；不上传数据。
  `connection.workerVersion/statusMethod/lastStatusSuccessAt`可区分已加载worker版本与最近一次真实POST status成功。
  popup兼容原生`reasonCode/status/state`；status中的提交FPS和SDK计数不冒充实际显示FPS。
- 状态查询和控制不等待全标签广播或Chrome持久化；标签消息逐个800ms限时。
  发布只保留最新状态，最多一个实际存储写入；挂起写入不阻断当前真实状态，晚返回后补写最新控制结果。
  初始化读取500ms限时，失败可重试；网络请求1500ms限时，同时释放共享status等待。
- 弹窗读取状态2600ms、控制4000ms限时，失败明确显示原因并恢复可点击；可点“重新连接”。
  旧弹窗响应、旧ON广播和晚到的初始存储读取不能覆盖较新的OFF状态。

## 覆盖边界

当前支持普通 DOM video。iframe、Shadow DOM、DRM、非标准CSS transform以及video元素自身非零border/padding不作完整支持承诺；不支持的来源/几何须恢复原画面。
画中画表面不再对应页面视频矩形，进入画中画暂停增强，退出后恢复；未支持嵌入播放器不能表述成“没有视频”。
小型静音循环动画不会自动获得主视频资格。页面zoom由 tabs.getZoom 上报；DPR包含OS缩放与页面zoom。
visualViewport pinch或偏移由原生明确校验。网页控件、字幕和站点弹幕提供排除矩形。
rVFC只测原视频帧率，不代表补帧后实际显示FPS。

## 来源与许可

GPL-3.0-or-later，完整许可见 COPYING。
检测、object-fit裁剪、UI排除矩形和前台/revision机制改编自本机 Magpie-VideoEnhance。
来源：<https://github.com/SAOG0721/Magpie>，固定源树提交 `931be4541e339612f39c80bca224dd4687872fcd`。
本次移除nativeMessaging、decoded-frame、应用启动；增加loopbackHTTP、双向状态、DPR/pageZoom/visualViewport/窗口元数据、小动画筛选、单槽消息和真实状态弹窗。
各改编源文件 SHA-256 见 SOURCE_PROVENANCE.json。

## 开发检查

`node --test browser-extension/tests/*.test.cjs`，不需要安装额外依赖。
