# JPEGView 触控板增强版

> 这是 [sylikc/jpegview](https://github.com/sylikc/jpegview) 的**个人分支**，不是官方版本。
> 在原版基础上增加了笔记本精密触控板的双指手势支持，其余功能与原版完全一致。

## 这是什么

JPEGView 是一个轻量、快速、高度可配置的图片查看与编辑工具，内置支持 JPEG、PNG、WEBP、JXL、HEIF/HEIC、AVIF、TIFF、PSD、TGA 以及各厂相机 RAW 等格式。

本分支只做了一件事：**让精密触控板的双指手势真正可用**。

## 新增的手势

| 手势 | 效果 |
|---|---|
| 双指拖动 | 平移画面，上下左右四个方向均可 |
| 双指捏合 | 缩放画面，张开放大、收拢缩小 |

鼠标操作完全不受影响，仍与原版一致。

## 为什么需要改

Windows 默认会把触控板的双指手势**提前合成为滚轮消息**再交给程序，所以在原版 JPEGView 里，双指拖动会被当成滚轮处理，结果是缩放而不是平移——手感完全不对。

本分支调用 `RegisterTouchpadCapableWindow`，让双指手势以 `WM_POINTER` 原始指针消息直接到达窗口，由程序自己决定如何处理。

不支持该 API 的环境（Windows 10 及更早）会自动退回原行为，不会报错。

## 改动清单

| 文件 | 说明 |
|---|---|
| `src/JPEGView/TouchpadHook.h` | 新增。触控板手势处理，纯头文件实现，无需登记到工程文件 |
| `src/JPEGView/MainDlg.h` | 消息映射新增 4 条 `WM_POINTER`，并声明处理函数 |
| `src/JPEGView/MainDlg.cpp` | 初始化时注册触控板，实现 3 个消息处理函数，关闭窗口时收尾日志 |

## 实现要点

调试过程中有三个结论值得记录，如果以后要改这块代码，能少走不少弯路。

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

现在日志默认关闭，且无论如何失败都不会中断功能。

## 环境变量

| 变量 | 作用 |
|---|---|
| `JPEGVIEW_TOUCHPAD_LOG=1` | 输出一次会话日志（默认关闭，不写任何文件） |
| `JPEGVIEW_NO_TOUCHPAD=1` | 完全禁用触控板手势 |
| `JPEGVIEW_TOUCHPAD_INVERT_Y=1` | 反转垂直平移方向 |

开启日志时，日志优先写入 exe 同目录；该目录不可写则写入 `%TEMP%`。

## 系统要求

- **Windows 11** —— `RegisterTouchpadCapableWindow` 的系统要求
- 编译需 Visual Studio 2022（v142 或 v143 工具集）

## 构建

向 `master` 推送会自动编译，也可以在 Actions 页面手动触发 **Build x64 Release**。产物是一个可直接运行的便携包，解压即用，无需安装。

## 原版文档

格式支持、配置文件、快捷键、命令行参数等完整说明，请见上游：

- [sylikc/jpegview](https://github.com/sylikc/jpegview)
- [原版 readme.html](https://htmlpreview.github.io/?https://github.com/sylikc/jpegview/blob/master/src/JPEGView/Config/readme.html)

## 许可

GPL-2.0-or-later，与原版一致。
