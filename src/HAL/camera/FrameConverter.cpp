#include "FrameConverter.h"

#include <algorithm>

#include <QPainter>

namespace FrameConverter {

QImage ColorToQImage(const CameraFrame& frame)
{
    if (frame.colorData.empty() || frame.width <= 0 || frame.height <= 0)
        return {};
    // 尺寸守卫（TR-116，2026-09-23）：`colorData` 必须容得下 `width×height×3`，否则下面的
    // `QImage` 会按该尺寸读越界。与 `DepthToQImage` 的同类守卫**对称**——后者缺守卫正是
    // 用例 11.35 崩溃（`0xC0000005`）的根因，彩色这条路结构完全相同，只是尚未被触发。
    if (frame.colorData.size() <
        static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3u)
        return {};

    int bytesPerLine = frame.width * 3;
    return QImage(frame.colorData.data(),
                  frame.width, frame.height,
                  bytesPerLine,
                  QImage::Format_RGB888).copy();
}

QImage ColorToQImageView(const CameraFrame& frame)
{
    if (frame.colorData.empty() || frame.width <= 0 || frame.height <= 0)
        return {};
    // 同 `ColorToQImage` 的尺寸守卫；**零拷贝视图尤其要挡**——它不复制，越界读的是
    // `frame.colorData` 的堆内存。
    if (frame.colorData.size() <
        static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3u)
        return {};

    // 与 `ColorToQImage` 同参，只去掉末尾的 `.copy()`——走 `const uchar*` 重载，
    // 是**只读视图**而非拷贝（语义见头文件）。
    const qsizetype bytesPerLine = static_cast<qsizetype>(frame.width) * 3;
    return QImage(frame.colorData.data(),
                  frame.width, frame.height,
                  bytesPerLine,
                  QImage::Format_RGB888);
}

namespace {

struct ColorStop { float t; QRgb rgb; };

const ColorStop kDepthGradient[] = {
    { 0.00f, qRgb(  0,   0,  90) },
    { 0.25f, qRgb(  0, 180, 200) },
    { 0.50f, qRgb(  0, 220,  60) },
    { 0.75f, qRgb(250, 220,  30) },
    { 1.00f, qRgb(240,  40,  40) },
};

QRgb GradientColor(float t)
{
    if (t <= 0.0f) return kDepthGradient[0].rgb;
    if (t >= 1.0f) return kDepthGradient[4].rgb;
    for (int i = 0; i < 4; ++i) {
        if (t >= kDepthGradient[i].t && t < kDepthGradient[i + 1].t) {
            float lo = kDepthGradient[i].t;
            float hi = kDepthGradient[i + 1].t;
            float k  = (hi > lo) ? (t - lo) / (hi - lo) : 0.0f;
            const auto& a = kDepthGradient[i];
            const auto& b = kDepthGradient[i + 1];
            return qRgb(static_cast<int>(a.rgb >> 16 & 0xFF) * (1 - k) + static_cast<int>(b.rgb >> 16 & 0xFF) * k,
                        static_cast<int>(a.rgb >> 8 & 0xFF)  * (1 - k) + static_cast<int>(b.rgb >> 8 & 0xFF)  * k,
                        static_cast<int>(a.rgb & 0xFF)        * (1 - k) + static_cast<int>(b.rgb & 0xFF)        * k);
        }
    }
    return kDepthGradient[4].rgb;
}

}

QImage DepthToQImage(const CameraFrame& frame, float depthMin, float depthMax, const QSize& targetSize)
{
    // 深度图**自带尺寸**（TR-116，2026-09-23）。改造前这里用 `frame.width/height`（彩色尺寸）
    // 索引 `depthData`——D2C 开启时二者相同（`ob::Align` 把深度重采样到彩色分辨率）故一直
    // 没暴露；**D2C 关闭时深度是原生分辨率**（常用档 848×480 vs 彩色 1280×720）⇒
    // 按彩色宽度跨行，最大索引 719×1280+1279 = 921599 远超 `depthData.size() = 407040`
    // ⇒ 越界读约 2 MB ⇒ 0xC0000005（用例 11.35 实测，崩溃偏移 `+0x387` 正落在下方 `row[sx]`）。
    const int w = (frame.depthWidth  > 0) ? frame.depthWidth  : frame.width;
    const int h = (frame.depthHeight > 0) ? frame.depthHeight : frame.height;
    if (frame.depthData.empty() || w <= 0 || h <= 0)
        return {};
    // 防御性校验：数据量不足即视为「无有效深度图」——宁可不出图也不越界。
    // 将来若有新的相机实现忘了填 `depthWidth/depthHeight`，这里只降级、不崩溃。
    if (frame.depthData.size() < static_cast<size_t>(w) * static_cast<size_t>(h))
        return {};

    const float range = (depthMax > depthMin) ? (depthMax - depthMin) : 1.0f;

    // P3-2（2026-09-22）：`targetSize` 有效且确实比源小 → 直接按最近邻步长采样产出目标
    // 尺寸，不生成 1920×1080 中间图。采样口径与「全分辨率 → `Qt::FastTransformation`
    // 降采样」一致（都是最近邻），故视觉等价，但迭代次数从 2.07 M 降到约 0.13 M。
    const bool decimate = targetSize.isValid() &&
                          targetSize.width() > 0 && targetSize.height() > 0 &&
                          (targetSize.width() < w || targetSize.height() < h);
    const int outW = decimate ? targetSize.width()  : w;
    const int outH = decimate ? targetSize.height() : h;

    QImage img(outW, outH, QImage::Format_RGB888);
    if (img.isNull())
        return {};

    for (int oy = 0; oy < outH; ++oy) {
        const int sy = decimate ? static_cast<int>(static_cast<qint64>(oy) * h / outH) : oy;
        const float* row = frame.depthData.data() + static_cast<size_t>(sy) * w;
        uint8_t* out = img.scanLine(oy);
        for (int ox = 0; ox < outW; ++ox) {
            const int sx = decimate ? static_cast<int>(static_cast<qint64>(ox) * w / outW) : ox;
            float d = row[sx];
            QRgb c;
            if (d <= 0.0f) {
                c = qRgb(18, 20, 26);
            } else {
                float t = std::clamp((d - depthMin) / range, 0.0f, 1.0f);
                c = GradientColor(t);
            }
            out[ox * 3 + 0] = static_cast<uint8_t>(c >> 16 & 0xFF);
            out[ox * 3 + 1] = static_cast<uint8_t>(c >> 8 & 0xFF);
            out[ox * 3 + 2] = static_cast<uint8_t>(c & 0xFF);
        }
    }
    return img;
}

QImage DepthToQImage16(const CameraFrame& frame)
{
    // 尺寸口径与 `DepthToQImage` 逐字一致（含 0 回落与 `size()` 守卫）——两函数必须
    // 永远对「同一帧该出多大图」给出一致答案，否则存图与预览会错位。
    const int w = (frame.depthWidth  > 0) ? frame.depthWidth  : frame.width;
    const int h = (frame.depthHeight > 0) ? frame.depthHeight : frame.height;
    if (frame.depthData.empty() || w <= 0 || h <= 0)
        return {};
    if (frame.depthData.size() < static_cast<size_t>(w) * static_cast<size_t>(h))
        return {};

    QImage img(w, h, QImage::Format_Grayscale16);
    if (img.isNull())
        return {};

    for (int y = 0; y < h; ++y) {
        const float* row = frame.depthData.data() + static_cast<size_t>(y) * w;
        // `scanLine` 按 4 字节对齐（Grayscale16 下每行至少 `w * 2` 字节，奇数宽会被补齐），
        // 故只写前 `w` 个 `quint16`，不整行 `memset`。
        quint16* out = reinterpret_cast<quint16*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const float d = row[x];
            // 无效深度（`<= 0`：无回波 / 超量程 / 未对齐留白）写 0 —— 与参考实现同语义。
            // 有效值四舍五入到整毫米并夹到 `uint16` 上限：`depthData` 已是 mm
            // （`OrbbecCamera.cpp:732` 的 `src[i] * scale`），此处**不再**做任何缩放。
            out[x] = (d > 0.0f)
                         ? static_cast<quint16>(std::min(d + 0.5f, 65535.0f))
                         : static_cast<quint16>(0);
        }
    }
    return img;
}

void DrawOverlays(QImage& image, const QVector<PuffResult>& results, double confidenceThreshold)
{
    if (image.isNull()) return;

    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 字号随图宽自适应（TR-132）：存图是**全分辨率**（真机 1920×1080），固定 9pt 在 1920 宽
    // 的图上仅 ~12 px 高，读 u/v 数字困难，缩进预览格更糊。夹到 [9, 20] pt 以兼顾小图
    // （仿真 640×480 ⇒ 取下限 9pt，外观与改造前一致）。
    QFont font = p.font();
    font.setPointSizeF(std::clamp(image.width() / 120.0, 9.0, 20.0));
    font.setBold(true);
    p.setFont(font);
    const QFontMetrics fm(font);

    for (int i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        if (r.confidence < confidenceThreshold)
            continue;

        // 包围框（TR-132）：用 `bboxX`/`bboxY` + `width`/`height` **精确**绘制。
        // ⚠ 不能再用「以 `pixelU`/`pixelV` 为中心」——该点现在语义为**球顶**，与包围框中心
        //   在真机帧上实测相差 ~47 px（≈23 mm）⇒ 照旧画会把框整体偏移、罩不住目标。
        const QRect box(r.bboxX, r.bboxY, r.width, r.height);
        p.setPen(QPen(QColor(0, 255, 120), 2));
        p.setBrush(Qt::NoBrush);
        if (box.width() > 0 && box.height() > 0)
            p.drawRect(box);

        // 目标中心 = **球顶**（青色十字）：与 `z` 同源——`SequenceWorker` 正是拿这一点喂
        // `PixelToRobot(u, v, z)`，所以十字与深度必须来自同一个物理点。
        const QPoint center(r.pixelU, r.pixelV);
        const int arm = std::max(6, fm.height() / 2);
        p.setPen(QPen(QColor(0, 255, 255), 2));
        p.drawLine(center.x() - arm, center.y(), center.x() + arm, center.y());
        p.drawLine(center.x(), center.y() - arm, center.x(), center.y() + arm);

        // ⚠ 原文本打的是 `x`/`y`（机器人坐标）——契约 §十 裁定 2 已弃用、**恒为 0**，属无效
        //   信息（算法不做坐标转换，基座坐标由下游 `PixelToRobot` 算）。改为打像素量：
        //   u/v = 球顶中心，w/h = 包围框尺寸，序号与结果表「序号」列一致。
        const QString line1 = QStringLiteral("#%1  u=%2 v=%3  w=%4 h=%5")
                                  .arg(i + 1)
                                  .arg(r.pixelU).arg(r.pixelV)
                                  .arg(r.width).arg(r.height);
        const QString line2 = QStringLiteral("z=%1mm  conf=%2%")
                                  .arg(QString::number(r.z, 'f', 1),
                                       QString::number(r.confidence * 100.0, 'f', 0));

        const int textW = std::max(fm.horizontalAdvance(line1), fm.horizontalAdvance(line2)) + 14;
        const int textH = fm.height() * 2 + 8;
        const int anchorX = (box.width() > 0) ? box.left() : center.x() - textW / 2;
        const int anchorY = ((box.height() > 0) ? box.top() : center.y()) - textH - 6;
        const QRect textRect(anchorX, std::max(0, anchorY), textW, textH);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 175));
        p.drawRect(textRect);
        p.setPen(Qt::white);
        p.setBrush(Qt::NoBrush);
        p.drawText(textRect.adjusted(7, 2, -5, -2),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   line1 + QLatin1Char('\n') + line2);
    }

    // 左上角 HUD（2026-10-16，用户需求「overlay 左上角打上排序后第一个目标的检测值」）：
    // 在图像左上角 (0,0) 内缩 6 px 处，固定打**排序后第一个（= 最高）目标**的检测值。
    // 动机：目标框落在画面任意位置、框上文字随框走（框贴顶时还会被 `max(0, …)` 上推），
    // 缩放 / 裁图后不便定位；角上 HUD 位置固定、内容与框上信息条**同款**，一眼可读。
    // ⚠ 取「**通过阈值过滤的第一个**」而非无条件 `results.front()`：阈值与检测同源后二者
    //   等价（TR-133），但这样写保证 HUD 与画面上的 `#1` 框**严格对应**，不会出现
    //   「HUD 有值、画面上却没这个框」的错配。
    // ⚠ 画在最后（最上层）：HUD 的意义就是「始终可读」，即使与某个框的标签重叠也以它为准
    //   （重叠时两者内容相同，不丢信息）。
    {
        int hudIdx = -1;
        for (int i = 0; i < results.size(); ++i) {
            if (results[i].confidence >= confidenceThreshold) { hudIdx = i; break; }
        }
        if (hudIdx >= 0) {
            const auto& r = results[hudIdx];
            const QString h1 = QStringLiteral("#%1  u=%2 v=%3  w=%4 h=%5")
                                   .arg(hudIdx + 1)
                                   .arg(r.pixelU).arg(r.pixelV)
                                   .arg(r.width).arg(r.height);
            const QString h2 = QStringLiteral("z=%1mm  conf=%2%")
                                   .arg(QString::number(r.z, 'f', 1),
                                        QString::number(r.confidence * 100.0, 'f', 0));
            const int hw = std::max(fm.horizontalAdvance(h1), fm.horizontalAdvance(h2)) + 14;
            const int hh = fm.height() * 2 + 8;
            const QRect hudRect(6, 6, hw, hh);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 190));   // 比目标信息条略实，HUD 更醒目
            p.drawRect(hudRect);
            p.setPen(Qt::white);
            p.setBrush(Qt::NoBrush);
            p.drawText(hudRect.adjusted(7, 2, -5, -2),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       h1 + QLatin1Char('\n') + h2);
        }
    }
    p.end();
}

void DrawRoiAnnotation(QImage& image, const RoiRect& roi, bool targetFound)
{
    if (image.isNull()) return;

    const QRect imgRect(0, 0, image.width(), image.height());

    // 有效检测区域：ROI 与图像求交。`w`/`h` ≤ 0 = 未配置 ROI（全图），或求交后为空
    // （ROI 完全在图外，属配置错误）→ 回落为整幅图内缩一圈，保证边框与标签可见
    // （pen 宽 2，内缩 1 px 恰好让描边完整落在图内）。
    QRect box;
    if (roi.w > 0 && roi.h > 0)
        box = QRect(roi.x, roi.y, roi.w, roi.h).intersected(imgRect);
    if (box.isEmpty())
        box = imgRect.adjusted(1, 1, -1, -1);

    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);

    // ROI 框用**黄色**，与 DrawOverlays 的目标绿框区分；线宽 2 对齐目标框。
    p.setPen(QPen(QColor(255, 200, 0), 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(box);

    if (!targetFound) {
        QFont font = p.font();
        font.setPointSize(9);
        font.setBold(true);
        p.setFont(font);

        const QString text  = QStringLiteral("未找到目标");
        const QFontMetrics fm(font);
        // 标签锚在框左上角：框顶贴图缘时压在框内侧（y 截到 0），横向越界时贴右边内缘。
        QRect textRect(std::max(0, std::min(box.left(), image.width() - fm.horizontalAdvance(text) - 10)),
                       std::max(0, box.top() - fm.height() - 6),
                       fm.horizontalAdvance(text) + 10, fm.height() + 4);
        textRect = textRect.intersected(imgRect);

        // 标签样式对齐 DrawOverlays 的信息条：半透明底 + 白字；底色带红以示「失败」。
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(170, 30, 30, 180));
        p.drawRect(textRect);
        p.setPen(Qt::white);
        p.setBrush(Qt::NoBrush);
        p.drawText(textRect, Qt::AlignCenter, text);
    }
    p.end();
}

}
