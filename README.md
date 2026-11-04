# AnyCloud · 夸克网盘（Switch 客户端）

基于 NXKit 框架（borealis UI + NanoVG + libmpv + cpr）实现的网盘客户端，
主要面向 Nintendo Switch（同时支持 macOS 桌面调试）。

当前对接的是夸克网盘：扫码登录、浏览文件、在线播放视频/音乐、查看图片。

## 功能

- **扫码登录**：获取夸克二维码 token，在 Switch 上直接绘制二维码，用手机夸克 App 扫码；Cookie 保存到本地
- **文件浏览**：左侧版块导航（最近观看 / 全部文件 / 分享文件 / 云收藏 / 我的），支持逐级进入目录
- **视图与排序**：列表 / 网格切换，按时间或名称排序
- **文件操作**：新建文件夹、重命名、删除、移动
- **视频播放**：libmpv 播放 HLS 转码流，可切换 4K / 超清 / 高清，支持快进快退与进度显示
- **图片查看**：远程图片下载后按屏幕比例显示
- **最近观看**：本地记录打开过的音视频/图片（上限 50 条）

## 构建

### Switch

前置：

- [devkitPro](https://devkitpro.org/)（`DEVKITPRO`、`DEVKITARM` 环境变量）
- Switch 版 mpv 及其依赖：`switch-mpv`、`switch-ffmpeg`（需带 mbedTLS 的版本，否则 HTTPS 播放会失败）
- libnx、curl、cpr 等 portlibs

```sh
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
cmake -S . -B build_switch -DPLATFORM_SWITCH=ON -DPYTHON_EXECUTABLE=/usr/bin/python3
cmake --build build_switch --target cloud.nro -j8
```

产物：`build_switch/apps/cloud/cloud.nro`

> 必须显式指定 `PYTHON_EXECUTABLE`，OpenCC 的构建脚本依赖它。

### macOS 桌面

需要 GLFW、mpv、curl 等依赖。

```sh
cmake -S . -B build_desktop -DPLATFORM_DESKTOP=ON
cmake --build build_desktop --target cloud -j8
```

### 推送到 Switch

通过 USB 直接写入 SD 卡（需要设备端部署 SwLink sysmodule）：

```sh
swlink-cli put build_switch/apps/cloud/cloud.nro /switch/cloud.nro
```

## 按键

| 界面 | 按键 | 作用 |
| --- | --- | --- |
| 通用 | B | 返回：目录内逐级返回上一层，根目录/无子页面时回到左侧版块 |
| 文件列表 | A | 打开文件或进入文件夹 |
| 文件列表 | X | 更多操作（重命名 / 删除 / 移动） |
| 文件列表 | Y | 排序菜单 |
| 文件列表 | 右上角按钮 | 切换列表/网格视图、新建文件夹 |
| 播放页 | A | 暂停 / 继续 |
| 播放页 | ← / → | 快退 / 快进 10 秒 |
| 播放页 | ↑ / ↓ | 快退 / 快进 60 秒 |
| 播放页 | Y | 清晰度选择 |
| 播放页 | B | 退出播放 |

## 目录结构

```
apps/cloud/          应用本体（登录页、文件列表、播放器、图片查看、驱动实现）
core/                通用框架：borealis 扩展控件、HTTP 客户端、mpv 封装、i18n、本地存储
library/             第三方库（borealis、cpr、lunasvg、OpenCC 等）
resources/           字体、图标、i18n 文案（随 romfs 打包）
cmake/NXKitApp.cmake 应用打包宏（Switch 下生成 NACP + NRO）
```

## 运行时数据

| 平台 | 路径 |
| --- | --- |
| Switch | `sdmc:/switch/nxkit/` |
| 桌面 | 运行目录下的 `nxkit_data/` |

包含：

- `config.json`：Cookie、设置
- `recent.json`：最近观看记录
- `qr_debug.log` / `mpv.log`：调试日志（排查播放问题时非常有用）

## 文案与图标

- 文案：`resources/i18n/{zh-Hans,en-US}/cloud.json`
- 应用图标：`resources/img/cloud-icon.jpg`（256×256 JPEG）
- 应用名与作者在 `apps/cloud/CMakeLists.txt` 中通过 `DISPLAY_NAME` / `AUTHOR` 指定

## 致谢

- NXKit 框架
- [borealis](https://github.com/xfangfang/borealis) UI 库
- [wiliwili](https://github.com/xfangfang/wiliwili) 的播放与构建方案参考
