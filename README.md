# AnyCloud（Switch 云盘客户端）

基于 NXKit 框架（borealis UI + NanoVG + libmpv + MuPDF + cpr）实现的网盘客户端，
主要面向 Nintendo Switch（同时支持 macOS 桌面调试）。

目前对接夸克网盘、阿里云盘、115 网盘、百度网盘，以及局域网 SMB 共享（网络邻居），
可在应用内随时切换。

## 功能

- **多网盘**：夸克 / 阿里 / 115 / 百度 / 网络邻居(smb)，顶部栏显示当前网盘，设置页一键切换
- **扫码登录**：在 Switch 上直接绘制二维码，用手机 App 扫码；凭据只保存在本机
- **网络邻居**：直接连接局域网 SMB/CIFS 共享（支持账号密码与匿名），浏览、上传、在线播放、看图与看文档都无需先下载到本机；共享配置填一次即记住，退出登录后仍保留，重连时表单自动填充
- **文件浏览**：最近 / 全部文件 / 传输 / 我的 四个栏目，逐级进入目录，右上角可刷新
- **视图与排序**：列表 / 网格切换，按时间或名称排序
- **文件操作**：新建文件夹、重命名、删除、跨目录移动
- **上传**：浏览整张 SD 卡选择文件上传到网盘，进度与结果在「传输管理」中查看，失败可重试
- **下载**：队列下载到 SD 卡，多线程分段，完成后可直接播放，支持 zip 解压
- **视频播放**：libmpv 硬件解码，清晰度切换、倍速、循环、画面比例，外挂字幕（SRT / ASS / SSA / VTT）与音轨切换，自动记忆播放进度
- **文档阅读**：直接打开 PDF（翻页 / 缩放 / 平移）与 EPUB（自动换行、插图、章节跳转）
- **图片查看**：原图加载，L / R 翻页
- **网络日志**：内置请求记录与状态，排查问题无需电脑
- **版本检测**：启动静默检查更新，「我的」页可手动检查，并按版本列出更新历史（从商店后台拉取）

桌面版额外提供下载目录选择等调试便利。

## 安装

| 渠道 | 说明 |
| --- | --- |
| [GitHub Releases](https://github.com/xbw12138/AnyCloud/releases) | 下载 `cloud.nro`，放到 SD 卡的 `switch/` 目录，用 hbmenu 启动 |
| [Homebrew App Store](https://hb-app.store/) | 已提交审核，通过后可直接在商店内搜索 AnyCloud 安装与更新 |
| 自建商店 [ecfun.cc](https://ecfun.cc) | 应用内「检查更新」也读取这里的最新版本 |

> 需要已破解（Atmosphere）的主机。首次运行需联网并登录网盘账号。

## 构建

### Switch

前置：

- [devkitPro](https://devkitpro.org/)（`DEVKITPRO`、`DEVKITARM` 环境变量）
- Switch 版 mpv 及其依赖：`switch-mpv`、`switch-ffmpeg`（需带 mbedTLS 的版本，否则 HTTPS 播放会失败）
- `switch-mupdf` / `switch-mupdf-third`（PDF 渲染，缺失时 PDF 页会提示不支持）
- `switch-libwebp`（夸克封面）、`zlib`（zip 解压）
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
| 通用 | L / R | 切换顶部栏目（最近 / 全部文件 / 传输 / 我的） |
| 通用 | B | 返回：目录内逐级返回上一层，根目录/无子页面时回到左侧版块 |
| 文件列表 | A | 打开文件或进入文件夹 |
| 文件列表 | X | 更多操作（重命名 / 删除 / 移动 / 下载到本机） |
| 文件列表 | Y | 排序菜单 |
| 文件列表 | 右上角按钮 | 刷新、切换列表/网格视图、上传、新建文件夹 |
| 传输管理 | Y | 进入 / 退出多选 |
| 传输管理 | X | 删除当前记录或选中的记录 |
| 传输管理 | A | 查看任务详情，失败的任务可重试 |
| 播放页 | A | 暂停 / 继续 |
| 播放页 | ← / → | 快退 / 快进 10 秒 |
| 播放页 | ↑ / ↓ | 音量 + / − 5% |
| 播放页 | ZL / ZR | 快退 / 快进 30 秒 |
| 播放页 | L / R | 上一集 / 下一集 |
| 播放页 | X / Y | 循环模式 / 清晰度 |
| 播放页 | - / + | 字幕 / 播放目录 |
| 播放页 | R3 / L3 | 画面比例 / 倍速 |
| 播放页 | B | 退出播放 |
| PDF | L / R | 上一页 / 下一页 |
| PDF | 方向键 | 放大后平移画面 |
| PDF | Y / X | 放大 / 缩小 |
| EPUB | L / R、方向键 | 翻页 |
| EPUB | ZL / ZR | 上一章 / 下一章 |
| 图片 | L / R | 上一张 / 下一张 |

## 目录结构

```
apps/cloud/          应用本体（登录、文件列表、传输管理、播放器、文档/图片查看、各网盘驱动）
core/                通用框架：borealis 扩展控件、HTTP 客户端、mpv 封装、i18n、本地存储
library/             第三方库（borealis、cpr、lunasvg、OpenCC 等）
resources/           字体、图标、i18n 文案（随 romfs 打包）
cmake/NXKitApp.cmake 应用打包宏（Switch 下生成 NACP + NRO）
```

## 版本与发布

- 版本号在根 [CMakeLists.txt](CMakeLists.txt) 的 `VERSION_MAJOR/MINOR/REVISION`
- 当前版本 `1.9.4`：「我的」页新增更新历史
- 发布包上传到自建商店 `https://ecfun.cc`（`POST /api/v1/admin/apps/cloud/versions`，form 字段 `file` / `version` / `changelog`）；应用内「检查更新」读取同一接口的 `latest_version`
- 商店详情里的功能介绍通过 `PUT /api/v1/admin/apps/cloud` 的 `description` 字段维护
- GitHub Release 以版本号作 tag（如 `1.8.0`）并附上 `cloud.nro`；Homebrew App Store 上架后会按 Release 自动抓取新版本，无需重复提交表单

## 更新日志

### 1.9.4

- 「我的」页新增「更新历史」：从商店后台拉取全部已发布版本，按版本号与日期
  列出更新说明，可直接在机内查看

### 1.9.2

- 播放页 ↑/↓ 改为调节音量（每次 5%，OSD 显示当前音量），进度仍可用
  ←/→ 10 秒、ZL/ZR 30 秒调节
- 播放页按键提示与「操作指南」同步更新

### 1.9.1

- 修复更新提示弹窗：更新说明不再挤成一行，长文本按对话框宽度自动换行，
  标题、更新内容与底部提示分层显示

### 1.9.0

- 新增 **网络邻居（SMB）** 数据源：直接连接局域网 SMB/CIFS 共享（账号密码或匿名），
  浏览 / 上传 / 新建文件夹 / 重命名 / 删除 / 移动，视频、音频、图片、PDF、EPUB 直接打开
- 播放与看图走本地 HTTP 桥接（127.0.0.1 + Range），拖动进度、按需读取，无需整份下载
- 共享配置保存到本机，重启或退出登录后仍会预填，重连只需按一次「连接」
- 修复登录成功后「我的」页面账号状态不刷新（需要切 tab 才更新）的问题
- 修复退出登录会把 SMB 共享配置一并清空的问题；退出提示改为「重新配置连接」

## 许可证

[MIT](LICENSE) © 2026 xbw12138

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
- [MuPDF](https://mupdf.com/) 提供 PDF 渲染
