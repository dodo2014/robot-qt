#pragma once

#include <QImage>
#include <QVector>

#include "ICamera.h"
#include "IPuffAlgorithm.h"

namespace FrameConverter {

QImage ColorToQImage(const CameraFrame& frame);
QImage DepthToQImage(const CameraFrame& frame, float depthMin = 0.0f, float depthMax = 500.0f);
void   DrawOverlays(QImage& image, const QVector<PuffResult>& results, double confidenceThreshold = 0.0);

}
