#include "XRServo.h"

#include <spdlog/spdlog.h>
#include <Windows.h>

// 在全局作用域自动注册到工厂
REGISTER_AXIS_SERVO("XRServo", XRServo)

// ============================================================
// XR 串口协议实现（移植自 bopai\puff\src\core\XRServo.cpp）
// 帧格式: 0xF9 0xFF <id> <len> <cmd> [data...] <checksum>
// ============================================================
class XRServo::Impl
{
public:
    HANDLE hSerial = INVALID_HANDLE_VALUE;
    bool ownsHandle = false;
    uint8_t servoId = 1;
    bool online = false;
    bool torque = false;
    double angle = 90.0;
    double speed = 50.0;
    int lastErrorCode = 0;
    std::string lastError;

    static constexpr float SERVO_ACTUAL_RANGE = 270.0f;
    static constexpr float STANDARD_360_RANGE = 360.0f;
    static constexpr float ZERO_OFFSET = 0.0f;

    static constexpr uint8_t FRAME_HEADER1 = 0xF9;
    static constexpr uint8_t FRAME_HEADER2_TX = 0xFF;
    static constexpr uint8_t CMD_READ = 0x02;
    static constexpr uint8_t CMD_WRITE = 0x03;
    static constexpr uint8_t ADDR_SERVO_ID = 0x0F;
    static constexpr uint8_t ADDR_CUR_POS = 0x46;
    static constexpr uint8_t ADDR_CUR_VOLT = 0x4B;
    static constexpr uint8_t ADDR_CUR_TEMP = 0x4A;
    static constexpr uint8_t ADDR_CUR_CURRENT = 0x48;
    static constexpr uint8_t ADDR_TORQUE_SW = 0x64;
    static constexpr uint8_t ADDR_MOVE_TIME = 0x65;
    static constexpr uint8_t ADDR_MOVE_SPEED = 0x66;

    bool IsOpen() const { return hSerial != INVALID_HANDLE_VALUE; }

    uint8_t CheckSum(uint8_t* buf, int len) const
    {
        uint8_t sum = 0;
        for (int i = 0; i < len; ++i) sum += buf[i];
        return ~sum;
    }

    bool SerialSend(uint8_t* buf, int len)
    {
        if (!IsOpen()) return false;
        DWORD written = 0;
        return WriteFile(hSerial, buf, len, &written, NULL) == TRUE;
    }

    int SerialRecv(uint8_t* buf, int maxLen)
    {
        if (!IsOpen()) return 0;
        DWORD readLen = 0;
        ReadFile(hSerial, buf, maxLen, &readLen, NULL);
        return static_cast<int>(readLen);
    }

    void SendReadCmd(uint8_t addr)
    {
        uint8_t buf[16] = { 0 };
        int idx = 0;
        buf[idx++] = FRAME_HEADER1;
        buf[idx++] = FRAME_HEADER2_TX;
        buf[idx++] = servoId;
        buf[idx++] = 3;
        buf[idx++] = CMD_READ;
        buf[idx++] = addr;
        buf[idx++] = CheckSum(&buf[2], 4);
        SerialSend(buf, idx);
        Sleep(20);
    }

    bool SendWrite8Cmd(uint8_t addr, uint8_t val)
    {
        uint8_t buf[16] = { 0 };
        int idx = 0;
        buf[idx++] = FRAME_HEADER1;
        buf[idx++] = FRAME_HEADER2_TX;
        buf[idx++] = servoId;
        buf[idx++] = 4;
        buf[idx++] = CMD_WRITE;
        buf[idx++] = addr;
        buf[idx++] = val;
        buf[idx++] = CheckSum(&buf[2], 5);
        if (!SerialSend(buf, idx)) {
            lastErrorCode = -1;
            lastError = "串口发送失败";
            return false;
        }
        Sleep(20);
        return true;
    }

    bool SendWrite16Cmd(uint8_t addr, uint16_t val)
    {
        uint8_t buf[16] = { 0 };
        int idx = 0;
        buf[idx++] = FRAME_HEADER1;
        buf[idx++] = FRAME_HEADER2_TX;
        buf[idx++] = servoId;
        buf[idx++] = 5;
        buf[idx++] = CMD_WRITE;
        buf[idx++] = addr;
        buf[idx++] = val & 0xFF;
        buf[idx++] = (val >> 8) & 0xFF;
        buf[idx++] = CheckSum(&buf[2], 6);
        if (!SerialSend(buf, idx)) {
            lastErrorCode = -1;
            lastError = "串口发送失败";
            return false;
        }
        Sleep(20);
        return true;
    }

    int16_t MapAngleToServo(float standardAngle)
    {
        if (standardAngle < 0) standardAngle = 0;
        if (standardAngle > STANDARD_360_RANGE) standardAngle = STANDARD_360_RANGE;
        float servoAngle = standardAngle * (SERVO_ACTUAL_RANGE / STANDARD_360_RANGE) + ZERO_OFFSET;
        return static_cast<int16_t>(servoAngle * 10);
    }

    bool MoveSpeedRaw(int16_t angle10, uint16_t speed)
    {
        uint8_t buf[20] = { 0 };
        int idx = 0;
        buf[idx++] = FRAME_HEADER1;
        buf[idx++] = FRAME_HEADER2_TX;
        buf[idx++] = servoId;
        buf[idx++] = 7;
        buf[idx++] = CMD_WRITE;
        buf[idx++] = ADDR_MOVE_SPEED;
        buf[idx++] = angle10 & 0xFF;
        buf[idx++] = (angle10 >> 8) & 0xFF;
        buf[idx++] = speed & 0xFF;
        buf[idx++] = (speed >> 8) & 0xFF;
        buf[idx++] = CheckSum(&buf[2], 8);
        if (!SerialSend(buf, idx)) {
            lastErrorCode = -1;
            lastError = "舵机速度移动指令发送失败";
            return false;
        }
        Sleep(50);
        uint8_t recvBuf[32] = { 0 };
        SerialRecv(recvBuf, 32);
        return true;
    }
};

// ============================================================
XRServo::XRServo()
    : impl_(std::make_unique<Impl>())
{
}

XRServo::~XRServo()
{
    Disconnect();
}

bool XRServo::Connect(const std::string& port, int baudRate)
{
    char portName[32];
    sprintf_s(portName, "\\\\.\\%s", port.c_str());

    impl_->hSerial = CreateFileA(portName, GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (impl_->hSerial == INVALID_HANDLE_VALUE) {
        impl_->lastErrorCode = -2;
        impl_->lastError = "打开串口失败: " + port;
        SPDLOG_WARN("[XRServo] Open port {} failed", port);
        return false;
    }
    impl_->ownsHandle = true;

    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(DCB);
    GetCommState(impl_->hSerial, &dcb);
    dcb.BaudRate = (baudRate > 0) ? baudRate : 115200;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    SetCommState(impl_->hSerial, &dcb);

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutConstant = 50;
    SetCommTimeouts(impl_->hSerial, &timeouts);

    SPDLOG_INFO("[XRServo] Connected to {} @ {} baud", port, dcb.BaudRate);
    return true;
}

void XRServo::Disconnect()
{
    if (impl_->ownsHandle && impl_->hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->hSerial);
        impl_->hSerial = INVALID_HANDLE_VALUE;
        impl_->ownsHandle = false;
    }
    impl_->torque = false;
    impl_->online = false;
}

bool XRServo::IsConnected() const
{
    return impl_->IsOpen();
}

bool XRServo::SetServoId(uint8_t id)
{
    impl_->servoId = id;
    SPDLOG_INFO("[XRServo] Servo id set to {}", id);
    return true;
}

uint8_t XRServo::GetServoId() const
{
    return impl_->servoId;
}

bool XRServo::TorqueOn()
{
    impl_->torque = impl_->SendWrite8Cmd(Impl::ADDR_TORQUE_SW, 2);
    return impl_->torque;
}

bool XRServo::TorqueOff()
{
    bool ok = impl_->SendWrite8Cmd(Impl::ADDR_TORQUE_SW, 0);
    impl_->torque = false;
    return ok;
}

bool XRServo::MoveToAngle(double angleDeg, int timeMs)
{
    double a = angleDeg;
    if (a < 0) a = 0;
    if (a > 360) a = 360;
    int16_t sendVal = impl_->MapAngleToServo(static_cast<float>(a));
    impl_->angle = a;

    uint8_t buf[20] = { 0 };
    int idx = 0;
    buf[idx++] = Impl::FRAME_HEADER1;
    buf[idx++] = Impl::FRAME_HEADER2_TX;
    buf[idx++] = impl_->servoId;
    buf[idx++] = 7;
    buf[idx++] = Impl::CMD_WRITE;
    buf[idx++] = Impl::ADDR_MOVE_TIME;
    buf[idx++] = sendVal & 0xFF;
    buf[idx++] = (sendVal >> 8) & 0xFF;
    uint16_t t = static_cast<uint16_t>(timeMs > 0 ? timeMs : 1000);
    buf[idx++] = t & 0xFF;
    buf[idx++] = (t >> 8) & 0xFF;
    buf[idx++] = impl_->CheckSum(&buf[2], 8);
    if (!impl_->SerialSend(buf, idx)) {
        impl_->lastErrorCode = -1;
        impl_->lastError = "舵机移动指令发送失败";
        return false;
    }
    Sleep(50);
    uint8_t recvBuf[32] = { 0 };
    impl_->SerialRecv(recvBuf, 32);
    return true;
}

bool XRServo::MoveAtSpeed(double angleDeg, double speedDps)
{
    double a = angleDeg;
    if (a < 0) a = 0;
    if (a > 360) a = 360;
    int16_t sendVal = impl_->MapAngleToServo(static_cast<float>(a));
    impl_->angle = a;
    if (speedDps > 0) impl_->speed = speedDps;
    uint16_t spd = static_cast<uint16_t>(impl_->speed > 0 ? impl_->speed : 50.0);
    return impl_->MoveSpeedRaw(sendVal, spd);
}

bool XRServo::Stop()
{
    // 停止 = 发"移动到当前目标角"指令让舵机停在原地并保持扭矩，
    // 不能发扭矩关，否则 J2/R 会失去力矩直接下垂
    return MoveToAngle(impl_->angle, 1000);
}

bool XRServo::SetSpeed(double speedDps)
{
    impl_->speed = speedDps;
    return true;
}

double XRServo::ReadAngle()
{
    return impl_->angle;
}

ServoTelemetry XRServo::ReadTelemetry()
{
    ServoTelemetry t;
    t.angleDeg = impl_->angle;
    t.online = impl_->IsOpen();

    // 尝试读电压/温度/电流
    impl_->SendReadCmd(Impl::ADDR_CUR_VOLT);
    uint8_t buf[32] = { 0 };
    int len = impl_->SerialRecv(buf, 32);
    if (len >= 8) {
        int16_t v = static_cast<int16_t>(buf[6] | (buf[7] << 8));
        t.voltage = v / 1000.0f;
    }
    return t;
}

bool XRServo::IsOnline() const
{
    return impl_->IsOpen();
}

bool XRServo::ClearAlarm()
{
    impl_->lastErrorCode = 0;
    impl_->lastError.clear();
    return true;
}

std::string XRServo::GetLastError() const
{
    return impl_->lastError;
}
