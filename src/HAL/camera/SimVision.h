#pragma once

#include <cmath>

namespace SimVision {

struct TargetSpec
{
    int   r, g, b;
    int   radius;
    float depthMm;
    float phase;
    float ampX, ampY;
    float speed;
};

constexpr int kWidth  = 640;
constexpr int kHeight = 480;
constexpr int kTargetCount = 2;

inline const TargetSpec kTargets[kTargetCount] = {
    { 255,  60, 130, 30,  55.0f, 0.0f, 120.0f,  70.0f, 0.32f },
    {  50, 140, 255, 20, 105.0f, 1.3f, 100.0f,  60.0f, 0.55f },
};

inline float MotionPhase(float tSec, float speed)
{
    return 2.0f * 3.14159265358979f * tSec * speed;
}

}
