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

    QFont font = p.font();
    font.setPointSize(9);
    font.setBold(true);
    p.setFont(font);

    for (const auto& r : results) {
        if (r.confidence < confidenceThreshold)
            continue;

        QRect box(r.pixelU - r.width / 2, r.pixelV - r.height / 2, r.width, r.height);
        QPen pen(QColor(0, 255, 120), 2);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(box);

        QString text = QStringLiteral("x=%1 y=%2 z=%3 conf=%4%")
                           .arg(QString::number(r.x, 'f', 1),
                                QString::number(r.y, 'f', 1),
                                QString::number(r.z, 'f', 1),
                                QString::number(r.confidence * 100.0, 'f', 0));

        QFontMetrics fm(font);
        QRect textRect(box.left(), std::max(0, box.top() - fm.height() - 6),
                       fm.horizontalAdvance(text) + 10, fm.height() + 4);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 160));
        p.drawRect(textRect);
        p.setPen(Qt::white);
        p.setBrush(Qt::NoBrush);
        p.drawText(textRect, Qt::AlignCenter, text);
    }
    p.end();
}

}
