# reCamera Pro 二维码与条形码识别

部署完成后，在同一网络的浏览器打开 **http://192.168.42.1:8766/** 查看 WebRTC 画面和 WebSocket 识别结果。浏览器直接与 reCamera Pro 建立 WebRTC 连接，不需要 Windows、WSL nginx 或 Windows go2rtc。

也可以选择 Windows 或 Linux 电脑作为视频代理。两种配置使用电脑上的 FFmpeg 将设备 MJPEG 转为 H.264，再由 go2rtc 提供 WebRTC；先部署对应的 Nginx 配置和 go2rtc 配置，然后访问 `http://代理电脑IP:18888/?video=proxy`。Linux 示例配置在 `web/nginx-site.conf` 和 `web/go2rtc-linux.yaml`；Windows 配置在 `web/nginx.conf` 和 `web/go2rtc-windows.yaml`。本地完整操作 Wiki 保存在 `wiki/`，按要求不会提交到 Git。

当前硬编码路径复用 `rkipc` 的原始主码流，所以不含应用在 MJPEG 预览中绘制的检测框；扫码内容仍会在右侧实时显示。若必须在硬编码画面烧录框，需要另行接入 Rockchip MPP/RGA 的 OSD 或编码前合成，不能由 go2rtc 完成。

## 工作方式

- Pro 上由 `cpp/main.cpp` 构建的程序通过 GStreamer 从 `/dev/video13` 采集 1920×1080 NV12 画面。识别线程以最新帧为准，不积压旧帧；视频预览仍输出 960×540 MJPEG，地址是 `http://192.168.42.1:8766/video.mjpg`。
- ZBar 同时启用 QR 和常见一维码。video13 的有限范围亮度会先扩展、轻度去噪并二值化；若该路径未解码，再使用原始灰度图回退扫描。DataBar 已关闭，避免在当前纸质场景中产生数值误报。日志会记录识别类型，例如 `CODE-128`。
- 每个不同的数据内容独立维护最近出现时间。首次检出发送一次；持续在画面中或离开不到 60 秒再出现均不重复发送；离开超过 60 秒后再次检出才重新发送。MQTT 和 WebSocket 的消息内容只有码内原始数据。浏览器重新连接时，WebSocket 会回放当前冷却窗口内的结果。
- Pro 在 `ws://设备IP:8766/ws` 提供识别结果；设备首页直接连接该 WebSocket，不经过 nginx。
- Pro 通过 MQTT QoS 1 发布到 `recamera/qr/result`。本机 Mosquitto 只监听 `127.0.0.1:1883`，需要用户名和密码。Pro 通过 SSH 反向隧道访问它。离线期间程序在内存中暂存至多 1000 条待发布事件。
- 固件的 `rkipc` 已经使用 RV1126B 硬件编码器，并在 `127.0.0.1:5554/live/0` 输出 H.264。`S97qr_go2rtc` 启动第二个、应用专用的 go2rtc，只做 H.264 封装/转发：API 信令在 TCP 1986，WebRTC 在 TCP/UDP 8557。它不读取 V4L2、不运行 FFmpeg，因而不会与 `rkipc` 争用摄像头或退化为 CPU 编码。
- 设备固件自带的 go2rtc（554/8555）保持不变；应用实例使用独立端口，避免修改厂商服务。外部访问固件 RTSP 554 需要认证，这也是不能把它直接作为无配置浏览器入口的原因。

## 已安装的位置

| 组件 | 位置 |
| --- | --- |
| Pro 程序 | `/userdata/qr_rec/qr_pro` |
| Pro 运行日志与 PID | `/userdata/qr_rec/qr_pro.log`、`/var/run/qr_pro.pid` |
| Pro MQTT 凭据 | `/userdata/qr_mqtt.env`，权限 600 |
| Pro 网页 | `http://设备IP:8766/`（由 `qr_pro` 提供） |
| Pro go2rtc 配置 | `/userdata/qr_rec/go2rtc.yaml` |
| Pro go2rtc 日志 | `/userdata/qr_rec/qr_go2rtc.log` |
| 本机 MQTT 服务 | `mosquitto.service`，仅回环地址且需要认证 |

MQTT 的 SSH 反向隧道仍仅用于把扫码事件发到本机 Mosquitto；它不影响视频或浏览器预览。视频服务在设备重启后会由 init 自动恢复。

```bash
ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 \
  -R 127.0.0.1:1883:127.0.0.1:1883 root@192.168.42.1
```

MQTT 凭据没有写进工程文件。需要订阅时可从 Pro 的 `/userdata/qr_mqtt.env` 读取用户名和密码。隧道不在时，视频和 WebSocket 仍可使用，MQTT 事件只在程序内存中排队；重启程序会清空队列。

## 构建与更新

`cpp/` 按职责拆分：

| 文件 | 职责 |
| --- | --- |
| `main.cpp` | 参数、信号、工作线程生命周期 |
| `camera.cpp` | video13 1080p GStreamer 采集、识别线程、网页帧缩放 |
| `barcode.cpp` | ZBar 解码、框坐标和 JPEG 输出 |
| `services.cpp` | 去重、MQTT、WebSocket 和 MJPEG HTTP 服务 |
| `app_state.cpp` | 跨线程运行状态和最新视频帧 |

对应接口声明放在同名头文件中。

目标必须是 RV1126B 的 aarch64。使用现有匹配设备的 SDK/sysroot：

```bash
cmake -S cpp -B cpp/build \
  -DCMAKE_TOOLCHAIN_FILE=/home/yylin/recamera_pro/recamera-pro-sdk/cmake/recamera-pro-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j4
```

首次安装/升级时，将二进制、网页和设备端 go2rtc 配置一起复制，并注册两个 init 脚本：

```bash
ssh root@192.168.42.1 'mkdir -p /userdata/qr_rec'
scp cpp/build/qr_pro web/index.html web/go2rtc-recamerapro.yaml \
  scripts/S97qr_go2rtc scripts/S98qr_pro root@192.168.42.1:/userdata/qr_rec/
ssh root@192.168.42.1 '
  set -e
  cd /userdata/qr_rec
  mv go2rtc-recamerapro.yaml go2rtc.yaml
  mkdir -p www && mv index.html www/index.html
  chmod 755 qr_pro S97qr_go2rtc S98qr_pro
  cp S97qr_go2rtc S98qr_pro /etc/init.d/
  /etc/init.d/S97qr_go2rtc restart
  /etc/init.d/S98qr_pro restart
'
```

`qr_pro` 的 HTTP 服务会从 `/userdata/qr_rec/www/index.html` 提供首页。若网页在其他目录，设置 `QR_WEB_ROOT` 即可。确认防火墙没有拦截设备的 TCP 1986 以及 TCP/UDP 8557；它们必须从浏览器所在网段可达。

## 验证记录

- 在设备上验证 `/dev/video13` 可与原有摄像头服务同时采集；无需停止 `rkipc`。
- 使用两张二维码组成的 960×540 测试图，设备程序分别识别 `QR-PRO-TEST-A`、`QR-PRO-TEST-B`。WebSocket 和 MQTT 各收到两条不同内容，连续重复扫描未重复发送。
- 设备上的 `blur100.png` 样图识别为 `CODE-128`，并解出 `test000002`、`test000006`。
- 当前 video13 实时画面中的条形码识别为 `CODE-128 / test000007`，单次扫描约 214–390ms。
- 将用户提供的 `image/download.png` 上传到 Pro 的 `/userdata/test_barcode.png`，设备程序读出 `test000008`。在 960×540 白底测试画面中，条形码宽 133 像素时可识别，宽 100 像素时漏检。改为原始 1920×1080 亮度画面识别后，4 秒 MJPEG 抓流得到 34 帧；相机画面中尚未出现这张实体条形码，实际摆放条件仍需复测。
- 媒体拓扑显示 `sc850sl` 传感器和 ISP 工作在 3840×2160；`/dev/video13` 是 ISP mainpath，`/dev/video26`、`/dev/video27` 是可并行打开的 4K VPSS 输出。当前正式程序使用 video13，以保证扫码延迟。
- 旧的 Windows 中转方案已验证过浏览器 WebRTC；本次设备直连改动尚需在设备安装后验证：`ss -lntup` 应显示 1986 与 8557，浏览器打开 `http://设备IP:8766/` 后 PeerConnection 应为 `connected`。`qr_go2rtc.log` 中不应出现 FFmpeg 或 V4L2 编码日志。

go2rtc 的 WebRTC 用法参见其[官方 WebRTC 文档](https://github.com/AlexxIT/go2rtc/blob/master/internal/webrtc/README.md)。
