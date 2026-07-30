#pragma once

#include <string>

enum class EndEffectorType
{
    Gripper,     // 夹爪
    Suction,     // 吸盘
    Custom       // 其他（如灌装头）
};

class IEndEffector
{
public:
    virtual ~IEndEffector() = default;

    virtual bool Connect() = 0;
    virtual void Disconnect() = 0;
    virtual bool IsConnected() const = 0;

    virtual bool Open() = 0;
    virtual bool Close() = 0;
    virtual bool SetPosition(double percent) = 0;
    virtual double GetPosition() const = 0;

    virtual bool IsOpen() const = 0;
    virtual bool IsClosed() const = 0;

    virtual EndEffectorType GetType() const = 0;

    virtual std::string GetLastError() const = 0;
};
