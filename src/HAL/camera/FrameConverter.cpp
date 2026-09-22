#include "FrameConverter.h"

#include <algorithm>

#include <QPainter>

namespace FrameConverter {

QImage ColorToQImage(const CameraFrame& frame)
{
    if (frame.colorData.empty() || frame.width <= 0 || frame.height <= 0)
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
    if (frame.depthData.empty() || frame.width <= 0 || frame.height <= 0)
        return {};

    const int w = frame.width;
    const int h = frame.height;
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
