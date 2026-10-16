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

// 深度**原始值**（16-bit 灰度，单位 mm）。
//
// 与 `DepthToQImage` 的分工是**语义**上的，不是画质上的：
//   `DepthToQImage` = 给人看的**伪彩可视化**（8-bit RGB，`0` 值画深色、有效值映射到渐变）；
//   本函数 = 给工具/算法吃的**物理量**（每像素一个 `uint16` 毫米值，不缩放、不归一化）。
//
// ⚠ 伪彩图**不可逆**：渐变把 [0,500] mm 压到 8-bit 三通道，既丢了绝对刻度、又受
// `depthMin/depthMax` 影响，无法从中还原 mm。因此**凡是要「深度值」的落盘都必须走本函数**
// ——存图路径曾误用 `DepthToQImage`，落盘的是伪彩图，`DepthViewer` 一类的 16-bit 深度
// 查看器读到的全是 0（2026-10-14 用户实测；TR-125）。参考实现
// （`robot-single-thread` 的 `cv2.imwrite(depth_uint16)`）存的是原生 16-bit 灰度，本函数与之对齐。
//
// 尺寸口径与 `DepthToQImage` **完全一致**（`depthWidth/depthHeight`，0 回落 `width/height`，
// 并按 `depthData.size()` 守卫）；无效深度（`<= 0`，即无回波/超量程）写 0。
QImage DepthToQImage16(const CameraFrame& frame);

// 检测结果叠加（TR-132，2026-10-16 改版）：每个通过阈值的 `PuffResult` 画
//   ① 绿色包围框 = `(bboxX, bboxY, width, height)`（**精确**，不再假设「以某点为中心」）；
//   ② 青色十字 = `(pixelU, pixelV)`，语义为**球顶**——与 `z` 同源，即 `PixelToRobot` 的输入点；
//   ③ 文字 = 序号 + `u/v/w/h` + `z` + `conf`（随各自的框走）。
//   ④ **左上角 HUD**（2026-10-16）= 排序后第一个（= 最高）目标的**同款两行文本**，固定在
//      图像左上角内缩 6 px 处、画在最上层。动机：框上文字随框走（框贴顶还会被上推），
//      缩放 / 裁图后不便定位；角上 HUD 位置固定、一眼可读。`results` 为空时无 HUD。
//
// ⚠ 改造前文字打的是 `x`/`y`（机器人坐标）：契约 §十 裁定 2 已弃用、算法侧**恒为 0**，
//   属无效信息；且框以 `(pixelU, pixelV)` 为框心——`pixelU/pixelV` 改为球顶后，
//   与框中心相差 ~47 px，照旧画会罩不住目标。
// ⚠ 坐标口径 = **彩色系**（`PuffResult` 契约）；字号随图宽自适应（存图是全分辨率）。
void   DrawOverlays(QImage& image, const QVector<PuffResult>& results, double confidenceThreshold = 0.0);

/// 存图标注（TR-128，2026-10-15）：画出 ROI 检测区域框；`targetFound=false` 时在框
/// 左上角叠加「未找到目标」文案标签。
///
/// ⚠ 只用于**落盘彩图**（RGB / Overlay）。16-bit 深度图存的是算法吃的物理量，
/// 画上标注就毁了数据语义，**不得**调用本函数（与 TR-125 的落盘格式裁定同理）。
///
/// ⚠ 坐标口径：`RoiRect` 的坐标系 = **深度图**（`IPuffAlgorithm.h` 契约）。D2C = SW
/// 对齐时两系分辨率相同、直接可用；D2C 关闭时按**数值**直绘到彩图坐标并裁到图内
/// ——这是已知的口径差异，本函数不做任何坐标变换（与预览叠加框同一口径）。
///
/// `roi.w` / `roi.h` ≤ 0（未配置 ROI = 全图）或 ROI 与图像完全不相交时，回落为
/// 整幅图内缩一圈的边框——保证「未找到目标」标签在任何情况下都有锚点。
void   DrawRoiAnnotation(QImage& image, const RoiRect& roi, bool targetFound);

}
