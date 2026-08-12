#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MotorStatus
{
    int     axisId        = 0;
    double  position      = 0.0;   // 脉冲
    double  velocity      = 0.0;   // 脉冲/s
    double  current       = 0.0;
    bool    enabled       = false;
    bool    alarm         = false;
    bool    homeDone      = false;
    bool    running       = false;
    bool    limitPositive = false;
    bool    limitNegative = false;
};

struct AxisConfig
{
    int     axisId        = 0;
    double  maxSpeed      = 100.0;
    double  maxAccel      = 200.0;
    double  maxDecel      = 200.0;
    double  jogSpeed      = 100.0;
    double  homePos       = 0.0;
    double  limitMin      = -180.0;
    double  limitMax      = 180.0;
    bool    inverted      = false;
    double  pulsesPerUnit = 100.0;

    // 单位换算参数（由 HardwareManager 从 config 喂入）
    int     hardwareType  = 0;     // 0=运动控制卡(脉冲), 1=串口总线舵机
    int     axisType      = 0;     // 0=旋转(角度), 1=直线(mm)，仅卡轴参与换算
    double  pulsesPerRev  = 131072;
    int     microSteps    = 512;
    double  lead          = 20.0;  // 直线轴导程 (mm/rev)
    double  gearRatio     = 1.0;   // 电机每转时输出端转数 (从动/主动), 直线轴参与换算
    int     homeDir       = 1;     // 回零搜索方向：1=正方向, 0=反方向（仅卡轴使用）
    int     homeSns       = -1;    // HOME 信号极性：-1=不改(沿用卡默认), 0/1=设置有效电平（仅卡轴使用）
    double  homeRapidVel  = 5.0;   // 回零快速段速度 (Pulse/ms，SDK 单位)，搜索段
    double  homeLocatVel  = 1.0;   // 回零定位段速度 (Pulse/ms，SDK 单位)，碰信号后精定位
};

class IMotionCard
{
public:
    virtual ~IMotionCard() = default;

    virtual bool Connect(const std::string& ip, int port) = 0;
    virtual void Disconnect() = 0;
    virtual bool IsConnected() const = 0;

    // 网口卡的本地(PC)地址注入。默认空实现，仅网口卡按需 override。
    virtual bool SetHost(const std::string& pcIp, int pcPort)
    {
        (void)pcIp; (void)pcPort;
        return true;
    }

    virtual bool EnableAxis(int axisId) = 0;
    virtual bool DisableAxis(int axisId) = 0;
    virtual bool HomeAxis(int axisId) = 0;
    virtual bool StopAxis(int axisId) = 0;
    virtual bool StopAll() = 0;
    virtual bool EmergencyStop() = 0;

    virtual bool MoveAbs(int axisId, double position, double speed = -1.0) = 0;
    virtual bool MoveRel(int axisId, double distance, double speed = -1.0) = 0;
    virtual bool MoveLinear(const std::vector<double>& targetPositions, double speed = -1.0) = 0;

    virtual bool MoveJog(int axisId, double speedPulsesPerSec, double accel = -1.0, int direction = 1) = 0;
    virtual bool StopJog(int axisId) = 0;

    virtual bool SetSpeed(int axisId, double speed) = 0;
    virtual bool SetAccel(int axisId, double accel, double decel = -1.0) = 0;
    virtual bool SetAxisConfig(int axisId, const AxisConfig& cfg) = 0;

    virtual double GetPosition(int axisId) = 0;
    virtual MotorStatus GetAxisStatus(int axisId) = 0;
    virtual std::vector<MotorStatus> GetAllStatus() = 0;

    virtual bool SetDO(int channel, bool state) = 0;
    virtual bool GetDI(int channel) = 0;

    virtual std::string GetLastError() const = 0;
};
