# JPEGView 触控板增强版

> 这是 [sylikc/jpegview](https://github.com/sylikc/jpegview) 的**个人分支**，不是官方版本。
> 在原版基础上增加了笔记本精密触控板的双指手势支持，并优化了超大图的加载速度，其余功能与原版一致。

## 这是什么

JPEGView 是一个轻量、快速、高度可配置的图片查看与编辑工具，内置支持 JPEG、PNG、WEBP、JXL、HEIF/HEIC、AVIF、TIFF、PSD、TGA 以及各厂相机 RAW 等格式。

本分支做两件事：**让精密触控板的双指手势真正可用**，以及**减少超大图的加载开销**。

## 新增的手势

| 手势 | 效果 |
|---|---|
| 双指拖动 | 平移画面，上下左右四个方向均可 |
| 双指捏合 | 缩放画面，张开放大、收拢缩小 |

鼠标操作完全不受影响，仍与原版一致。

## 为什么需要改手势

Windows 默认会把触控板的双指手势**提前合成为滚轮消息**再交给程序，所以在原版 JPEGView 里，双指拖动会被当成滚轮处理，结果是缩放而不是平移——手感完全不对。

本分支调用 `RegisterTouchpadCapableWindow`，让双指手势以 `WM_POINTER` 原始指针消息直接到达窗口，由程序自己决定如何处理。

不支持该 API 的环境（Windows 10 及更早）会自动退回原行为，不会报错。

## 大图加载优化

针对几百 MP 的极端大图（355 MP 的 PNG、389 MP 的 JPEG）做了两处改动。实测 355 MP PNG 的首帧时间 **从 8.2 秒降到 6.4 秒（约 −21%）**，解码结果与旧版逐字节一致。

**1. 静止 PNG 直接解码到返回缓冲区**

原实现把整图解到 `p_frame`，再拷到 `p_image`，最后再分配一块 1.4 GB 的内存拷一次——355 MP 的图要白白搬运 2.8 GB。现在静止图（没有 `acTL` 块）直接解进 `p_image` 并把所有权交给调用方，省掉两次拷贝和一次大分配。

带 `acTL` 的图一律走原路径：即使是单帧 APNG，也可能带帧偏移和 blend 操作，不能按静止图处理。

**2. 无 alpha 的 PNG 跳过混合循环**

源图既没有 alpha 通道、也没有 `tRNS` 块时，libpng 填入的 alpha 恒为 `0xFF`，而 `AlphaBlendBackground` 对全不透明像素会直接返回原值。这段「逐像素乘一次」的循环对 355 MP 要花约 1 秒，结果却是原样写回，属于纯浪费。

**3. JPEG 解码降采样：保留但默认关闭**

libjpeg-turbo 可以在 IDCT 阶段直接输出 1/2、1/4、1/8 的缩小图，能大幅降低内存占用。但它**永久丢弃像素**，放大到 100% 会糊——所以默认关闭，只在内存吃紧时手动开启。

| 场景 | 建议 |
|---|---|
| 日常看图 | 什么都不用设，全尺寸解码，放大清晰 |
| 内存吃紧 / 只浏览不看细节 | `JPEGVIEW_DOWNSAMPLE=1` |

## 改动清单

| 文件 | 说明 |
|---|---|
| `src/JPEGView/TouchpadHook.h` | 新增。触控板手势处理，纯头文件实现，无需登记到工程文件 |
| `src/JPEGView/MainDlg.h` | 消息映射新增 4 条 `WM_POINTER`，并声明处理函数 |
| `src/JPEGView/MainDlg.cpp` | 初始化时注册触控板，实现 3 个消息处理函数，关闭窗口时收尾日志 |
| `src/JPEGView/TJPEGWrapper.cpp` | JPEG 可选降采样（默认关闭），并修正像素数比较的 32 位溢出 |
| `src/JPEGView/PNGWrapper.cpp` / `.h` | 静止 PNG 快路径、上报是否有 alpha、分段计时 |
| `src/JPEGView/ImageLoadThread.cpp` | 无 alpha 时跳过混合循环；加载各阶段计时 |
| `src/JPEGView/LoadTimer.h` | 新增。加载与首帧的跨线程计时（默认完全静默） |
| `src/JPEGView/JPEGImage.cpp` | 首帧渲染阶段打点（4 通道转换 / 重采样 / LUT） |

## 实现要点

调试过程中有几个结论值得记录，如果以后要改这块代码，能少走不少弯路。

### 1. 手势判定用两指位移的夹角余弦

拖动时两根手指**同向**移动（余弦接近 +1），捏合时**反向**移动（余弦接近 −1）。实测两者的数据区间完全不重叠，所以不需要任何经验阈值，也就不存在「调大误伤捏合、调小误伤拖动」的两难。

```cpp
d0 = 手指0 本帧位移;   d1 = 手指1 本帧位移;
cos = (d0 · d1) / (|d0| × |d1|);

if (cos >  0.3) 拖动证据 += 位移;    // 同向
if (cos < -0.3) 捏合证据 += 位移;    // 反向
// 证据够了就按多数决锁定，整段手势不再改变
```

### 2. 必须做帧同步

`WM_POINTERUPDATE` 每次只带**一根**手指的位置。如果收到一条消息就立刻计算中点和间距，拿到的是一份「一根新、一根旧」的快照——两指间距会凭空抖动半个手指的距离，这个抖动足以被误判成捏合。

正确做法是靠 `frameId` 判断帧边界，等**整帧坐标都到齐**再结算。代价是延迟一帧（约 10~16 毫秒），肉眼基本无感。

### 3. 日志绝不能影响功能

程序安装到 `Program Files` 时，普通用户对该目录没有写权限。最初的代码是「日志打不开就直接返回」，导致触控板注册代码从未执行——功能整体失效，而且看起来像「怎么移动位置就不能用了」。

现在所有日志默认关闭，且无论如何失败都不会中断功能。

### 4. 性能数字必须重复测量

同一台机器、同一张图、同一份脚本，隔半小时测出的 inflate 速度能差 30%（系统缓存、杀毒扫描、笔记本降频都会影响）。因此：

- 不同时段的数字**不能相减**
- 对比两个版本要在同一时段**交替跑，各 3 次取中位数**
- 用「减法」得到的耗时只能当量级参考，不能当结论

### 5. 改动像素处理必须验证像素一致

判断「优化后画面是否相同」不能靠肉眼。本项目用两个开关做自证：

```cmd
set JPEGVIEW_LOAD_LOG=1
set JPEGVIEW_PNG_HASH=1      :: 对解码缓冲区算 FNV-1a 并写日志
set JPEGVIEW_PNG_SLOW=1      :: 强制走旧路径
```

两条路径的 `png.hash` 完全一致，才算优化没有改变结果。

## 环境变量

| 变量 | 作用 |
|---|---|
| `JPEGVIEW_TOUCHPAD_LOG=1` | 触控板会话日志（默认关闭） |
| `JPEGVIEW_NO_TOUCHPAD=1` | 完全禁用触控板手势 |
| `JPEGVIEW_TOUCHPAD_INVERT_Y=1` | 反转垂直平移方向 |
| `JPEGVIEW_LOAD_LOG=1` | 加载各阶段耗时（默认关闭） |
| `JPEGVIEW_DOWNSAMPLE=1` | JPEG 解码降采样，省内存但放大会糊 |
| `JPEGVIEW_PNG_HASH=1` | 输出解码缓冲区的 FNV-1a 哈希，用于验证像素一致 |
| `JPEGVIEW_PNG_SLOW=1` | 强制 PNG 走旧解码路径，用于对照测试 |

所有开关默认全部关闭，正常使用不写任何文件。开启日志时，优先写入 exe 同目录；该目录不可写则写入 `%TEMP%`。

## 系统要求

- **Windows 11** —— `RegisterTouchpadCapableWindow` 的系统要求（其余优化在更早版本上同样有效）
- 编译需 Visual Studio 2022（v142 或 v143 工具集）

## 构建

向 `master` 推送会自动编译，也可以在 Actions 页面手动触发 **Build x64 Release**。产物是一个可直接运行的便携包，解压即用，无需安装。

## 原版文档

格式支持、配置文件、快捷键、命令行参数等完整说明，请见上游：

- [sylikc/jpegview](https://github.com/sylikc/jpegview)
- [原版 readme.html](https://htmlpreview.github.io/?https://github.com/sylikc/jpegview/blob/master/src/JPEGView/Config/readme.html)

## 许可

GPL-2.0-or-later，与原版一致。
