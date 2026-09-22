#pragma once

#include <QImage>
#include <QSize>
#include <QVector>

#include "ICamera.h"
#include "IPuffAlgorithm.h"

namespace FrameConverter {

QImage ColorToQImage(const CameraFrame& frame);

// 零拷贝只读视图（P3，2026-09-22）。
//
// 与 `ColorToQImage` 的唯一差别：**不做 `QImage::copy()`**，直接把 `frame.colorData`
// 包成一个只读 `QImage`。省掉每帧 6.2 MB（1920×1080×3）的分配 + 拷贝——预览路径把这张
// 图缩到显示尺寸（约 460×280）后就不再需要全分辨率副本了。
//
// 依据 Qt 6 文档（`QImage(const uchar*, int, int, qsizetype, Format)`）：
// 「uses an existing read-only memory buffer」且「will never alter the contents of the
// buffer」——`bits()` 一类会返回深拷贝，写操作（如 `QPainter`）在 `detach()` 里先拷进
// 自有内存。**不会写穿到 `frame`**，这正是本项目需要的语义（帧是 `shared_ptr<const>`，
// 且 `AutoRunPage` / `VisionTestPage` 可能同时持有同一帧）。
//
// ⚠ **生命周期**：返回值与 `frame` 同寿，**不得跨帧缓存**。调用方须保证 `frame` 活过
// 整段使用（`RenderTiles` 用局部 `CameraFramePtr` 显式固定住）。
//
// ⚠ **前置条件**：缓冲须 32-bit 对齐，且 `bytesPerLine = width * 3` 亦为 4 的倍数。
// 1920×3 = 5760 ✓。宽度不是 4 的倍数时该重载不可用，须回落 `ColorToQImage`。
QImage ColorToQImageView(const CameraFrame& frame);

// 深度伪彩。
//
// `targetSize` 非空时**直接产出目标尺寸**（最近邻步长采样），跳过全分辨率中间图——
// 该循环每像素要做 float 除法 + 分支 + `GradientColor` 的 4 段线性搜索，
// 1920×1080 = 2.07 M 次迭代，而显示尺寸只有约 460×280（P3-2，2026-09-22）。
// 视觉等价：改造前也是「全分辨率 → `Fast` 最近邻降采样」，只是多生成了一张中间图。
QImage DepthToQImage(const CameraFrame& frame, float depthMin = 0.0f, float depthMax = 500.0f,
                     const QSize& targetSize = QSize());

void   DrawOverlays(QImage& image, const QVector<PuffResult>& results, double confidenceThreshold = 0.0);

}
