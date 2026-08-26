#include "XRServo.h"

#include <spdlog/spdlog.h>
#include <Windows.h>

#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

// 在全局作用域自动注册到工厂
REGISTER_AXIS_SERVO("XRServo", XRServo)

namespace {

// 同一条串口总线上的多个舵机实例共享同一个句柄：
// 注册表按端口名管理句柄 + 引用计数，最后一个析构的实例负责关闭。
// mtx 为串口级通信互斥：同总线多实例（如 J2/R 共连 COM3）若并发发帧会交叉，
// 必须在每帧事务（发送+接收）期间持锁。
struct SharedSerial
{
    HANDLE h = INVALID_HANDLE_VALUE;
    int refs = 0;
    std::mutex mtx;
};

std::mutex g_serialMutex;
std::map<std::string, std::shared_ptr<SharedSerial>> g_serials;

} // namespace

// ============================================================
// FashionStar 总线伺服舵机 (Fashionrobo) 串口协议
// 移植自 D:\workspace\projects\ServoTest\FashionStar_UartServoProtocol.*
// （真机实测可运行：Ping/查询角度/设置角度/查询电压电流功率温度）
// 帧格式: 请求 [0x4C 0x12] <cmd> <content_size> <content...> <checksum>
//         响应 [0x1C 0x05] <cmd> <content_size> <content...> <checksum>
// 校验和 = (header_lo + header_hi + cmd + content_size + Σcontent) & 0xFF
// 小端字节序; 角度单位 0.1°, 速度单位 0.1°/s
// ============================================================
class XRServo::Impl
{
public:
    std::shared_ptr<SharedSerial> serial_;
    std::string portName;
    bool ownsHandle = false;
    uint8_t servoId = 1;
    bool online = false;
    bool torque = false;
    bool onlineWarned_ = false;   // 边沿去重：online→offline 首次失败打 WARN，持续离线不再刷屏
    double angle = 90.0;
    double speed = 50.0;
    int lastMoveTimeMs = 0;
    int lastErrorCode = 0;
    std::string lastError;

    // 真实角度 ↔ 协议原始角度 标定系数（默认 1:1，待真机标定）
    float kAngleReal2Raw = 1.0f;
    float bAngleReal2Raw = 0.0f;

    static constexpr uint16_t FRAME_REQ_HEADER = 0x4C12;
    static constexpr uint16_t FRAME_RSP_HEADER = 0x1C05;
    static constexpr uint32_t RESP_TIMEOUT_MS = 60;   // 曾 40ms：USB 转串口延迟抖动下偶发误超时

    static constexpr uint8_t CMD_PING = 1;
    static constexpr uint8_t CMD_WRITE_DATA = 4;   // 写入用户数据
    static constexpr uint8_t CMD_SET_ANGLE = 8;    // 设置角度(指定周期)
    static constexpr uint8_t CMD_DAMPING = 9;      // 阻尼模式(松力)
    static constexpr uint8_t CMD_QUERY_ANGLE = 10; // 查询角度
    static constexpr uint8_t CMD_MONITOR = 22;     // 数据监控(电压/电流/功率/温度/状态/角度)
    static constexpr uint8_t CMD_CONTROL_MODE_STOP = 24; // 控制模式停止

    // FSUS_PARAM_ACCEL_SWITCH：0x00 不启用加速度处理（匀速），0x01 梯形加减速（默认）
    // 点动高频打断时梯形曲线反复重启会造成一顿一顿，关掉后匀速更平滑
    static constexpr uint8_t FSUS_PARAM_ACCEL_SWITCH = 44;

    bool IsOpen() const { return serial_ && serial_->h != INVALID_HANDLE_VALUE; }

    float AngleReal2Raw(float real) const { return kAngleReal2Raw * real + bAngleReal2Raw; }
    float AngleRaw2Real(float raw) const { return (raw - bAngleReal2Raw) / kAngleReal2Raw; }

    // 发送前清空输入缓冲（丢弃上一条指令的残留响应字节）
    void EmptyInput()
    {
        if (!IsOpen()) return;
        DWORD r = 0;
        uint8_t tmp[256];
        ReadFile(serial_->h, tmp, sizeof(tmp), &r, NULL);
    }

    // 发送帧；wantResponse=true 时等待并校验完整响应帧。
    // 不加固定 Sleep：写后轮询 ReadFile（短超时），单帧事务通常仅几 ms。
    bool Transaction(uint8_t cmd, const uint8_t* content, int nContent,
                     uint8_t* resp, int respMax, int* respLen, bool wantResponse)
    {
        if (!IsOpen()) { lastErrorCode = -2; lastError = "串口未打开"; return false; }
        std::lock_guard<std::mutex> lock(serial_->mtx);
        EmptyInput();

        uint8_t tx[256];
        int n = 0;
        tx[n++] = FRAME_REQ_HEADER & 0xFF;
        tx[n++] = (FRAME_REQ_HEADER >> 8) & 0xFF;
        tx[n++] = cmd;
        tx[n++] = static_cast<uint8_t>(nContent);
        uint16_t sum = static_cast<uint16_t>((FRAME_REQ_HEADER & 0xFF) + ((FRAME_REQ_HEADER >> 8) & 0xFF) + cmd + nContent);
        for (int i = 0; i < nContent; ++i) { tx[n++] = content[i]; sum += content[i]; }
        tx[n++] = sum & 0xFF;

        DWORD written = 0;
        if (!WriteFile(serial_->h, tx, n, &written, NULL)) {
            lastErrorCode = -3;
            lastError = "串口发送失败";
            return false;
        }
        if (!wantResponse) return true;

        uint8_t rx[256];
        int m = 0;
        DWORD start = GetTickCount();
        while (m < (int)sizeof(rx) && (GetTickCount() - start) < RESP_TIMEOUT_MS) {
            DWORD r = 0;
            if (ReadFile(serial_->h, rx + m, sizeof(rx) - m, &r, NULL) && r > 0) m += (int)r;
            // 滑动对齐：头部脏字节（迟到残留/半帧）逐字节丢弃直到找到响应帧头，
            // 曾因固定比较 rx[0..1] 且不丢字节 → 一个脏字节导致整事务超时误判离线
            while (m >= 2 && !(rx[0] == (FRAME_RSP_HEADER & 0xFF) && rx[1] == ((FRAME_RSP_HEADER >> 8) & 0xFF))) {
                memmove(rx, rx + 1, --m);
            }
            if (m < 4) continue;
            int contentLen = rx[3];
            int frameLen = 4 + contentLen + 1;
            if (m < frameLen) continue;
            uint16_t s = rx[0] + rx[1] + rx[2] + rx[3];
            for (int i = 4; i < frameLen - 1; ++i) s += rx[i];
            if ((s & 0xFF) == rx[frameLen - 1]) {
                if (resp && respMax > 0) {
                    int cpy = contentLen < respMax ? contentLen : respMax;
                    memcpy(resp, rx + 4, cpy);
                }
                if (respLen) *respLen = contentLen;
                return true;
            }
            // 校验失败：丢一字节重新对齐继续等后续数据（可能是错位/坏帧拼接），
            // 不立即放弃——曾一次坏帧即判查询失败，被上层放大成舵机离线
            memmove(rx, rx + 1, --m);
            lastErrorCode = -5;
            lastError = "响应校验和错误";
        }
        lastErrorCode = m >= sizeof(rx) ? -7 : -6;
        lastError = "响应超时";
        return false;
    }

    bool Ping()
    {
        uint8_t resp[8];
        int len = 0;
        uint8_t c[1] = { servoId };
        if (!Transaction(CMD_PING, c, 1, resp, sizeof(resp), &len, true)) {
            online = false;
            return false;
        }
        online = (len >= 1 && resp[0] == servoId);
        return online;
    }

    // cmd 8: 设置角度(指定周期). power=0 表示不限制功率
    bool SendSetAngle(float angleRaw, uint16_t intervalMs, uint16_t power)
    {
        int16_t a = static_cast<int16_t>(angleRaw * 10.0f);
        uint8_t c[7];
        c[0] = servoId;
        c[1] = a & 0xFF; c[2] = (a >> 8) & 0xFF;
        c[3] = intervalMs & 0xFF; c[4] = intervalMs >> 8;
        c[5] = power & 0xFF; c[6] = power >> 8;
        bool ok = Transaction(CMD_SET_ANGLE, c, 7, nullptr, 0, nullptr, false);
        SPDLOG_INFO("[XRServo] SendSetAngle id={} angleRaw={:.1f} angle={:.1f} interval={}ms -> {}",
                    (int)servoId, angleRaw, AngleRaw2Real(angleRaw), (int)intervalMs, ok);
        return ok;
    }

    // cmd 10: 查询角度 → 真实角度(度)
    bool QueryAngle(double& outDeg)
    {
        uint8_t resp[8];
        int len = 0;
        uint8_t c[1] = { servoId };
        if (!Transaction(CMD_QUERY_ANGLE, c, 1, resp, sizeof(resp), &len, true) || len < 3)
            return false;
        int16_t raw = static_cast<int16_t>(resp[1] | (resp[2] << 8));
        outDeg = AngleRaw2Real(static_cast<float>(raw) * 0.1f);
        return true;
    }

    // cmd 22: 数据监控 → 电压/电流/功率/温度/状态/角度 一次取全
    bool QueryMonitor(ServoTelemetry& t)
    {
        uint8_t resp[64];
        int len = 0;
        uint8_t c[1] = { servoId };
        if (!Transaction(CMD_MONITOR, c, 1, resp, sizeof(resp), &len, true) || len < 14)
            return false;
        t.voltage = static_cast<int16_t>((resp[2] << 8) | resp[1]);
        t.current = static_cast<int16_t>((resp[4] << 8) | resp[3]);
        int16_t tempAdc = static_cast<int16_t>((resp[8] << 8) | resp[7]);
        if (tempAdc > 0 && tempAdc < 4096) {
            double tLog = std::log(static_cast<double>(tempAdc) / (4096.0 - tempAdc));
            t.temperature = static_cast<int16_t>(
                1.0 / (tLog / 3435.0 + 1.0 / (273.15 + 25.0)) - 273.15);
        } else {
            t.temperature = -999;
        }
        int32_t rawAngle = (resp[13] << 24) | (resp[12] << 16) | (resp[11] << 8) | resp[10];
        t.angleDeg = AngleRaw2Real(static_cast<float>(rawAngle) * 0.1f);
        return true;
    }

    // cmd 9: 阻尼模式（松力）
    bool SendDamping(uint16_t power)
    {
        uint8_t c[3];
        c[0] = servoId;
        c[1] = power & 0xFF;
        c[2] = power >> 8;
        return Transaction(CMD_DAMPING, c, 3, nullptr, 0, nullptr, false);
    }

    // cmd 24: 控制模式停止。mode: 0=停止后卸力(失锁), 1=停止后保持锁力, 2=停止后阻尼
    bool SendControlModeStop(uint8_t mode, uint16_t power)
    {
        uint8_t c[4];
        c[0] = servoId;
        c[1] = static_cast<uint8_t>(mode | 0x10);
        c[2] = power & 0xFF;
        c[3] = power >> 8;
        return Transaction(CMD_CONTROL_MODE_STOP, c, 4, nullptr, 0, nullptr, false);
    }

    // cmd 4: 写入用户数据（地址见 FSUS_PARAM_*，见协议头）
    bool WriteUserData(uint8_t address, const uint8_t* data, uint8_t n)
    {
        uint8_t c[2 + 16];
        c[0] = servoId;
        c[1] = address;
        for (uint8_t i = 0; i < n && i < 16; ++i) c[2 + i] = data[i];
        uint8_t resp[8];
        int len = 0;
        bool ok = Transaction(CMD_WRITE_DATA, c, 2 + n, resp, sizeof(resp), &len, true);
        SPDLOG_INFO("[XRServo] WriteUserData id={} addr={} n={} -> {}",
                    (int)servoId, (int)address, (int)n, ok);
        return ok;
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
    std::lock_guard<std::mutex> lock(g_serialMutex);

    // 同一串口已被其他实例打开：复用共享句柄，仅递增引用计数
    auto it = g_serials.find(port);
    if (it != g_serials.end()) {
        impl_->serial_ = it->second;
        ++it->second->refs;
        impl_->portName = port;
        impl_->ownsHandle = false;
        SPDLOG_INFO("[XRServo] Reusing shared serial handle for {}", port);
    } else {
        char portName[32];
        sprintf_s(portName, "\\\\.\\%s", port.c_str());

        HANDLE h = CreateFileA(portName, GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            impl_->lastErrorCode = -2;
            impl_->lastError = "打开串口失败: " + port;
            SPDLOG_WARN("[XRServo] Open port {} failed", port);
            return false;
        }

        DCB dcb = { 0 };
        dcb.DCBlength = sizeof(DCB);
        GetCommState(h, &dcb);
        dcb.BaudRate = (baudRate > 0) ? baudRate : 115200;
        dcb.ByteSize = 8;
        dcb.StopBits = ONESTOPBIT;
        dcb.Parity = NOPARITY;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        SetCommState(h, &dcb);

        COMMTIMEOUTS timeouts = { 0 };
        timeouts.ReadIntervalTimeout = 10;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 20;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 20;
        SetCommTimeouts(h, &timeouts);

        auto shared = std::make_shared<SharedSerial>();
        shared->h = h;
        shared->refs = 1;
        g_serials[port] = shared;

        impl_->serial_ = shared;
        impl_->portName = port;
        impl_->ownsHandle = true;

        SPDLOG_INFO("[XRServo] Connected to {} @ {} baud", port, dcb.BaudRate);
    }

    // 探测舵机在线并初始化角度缓存
    if (impl_->Ping()) {
        double a = 0.0;
        if (impl_->QueryAngle(a)) impl_->angle = a;
        // 关闭梯形加减速：点动高频打断时梯形曲线反复重启 → 一顿一顿；
        // 匀速模式(SET_ANGLE 带周期)下舵机按周期恒速到达，打断时继续匀速
        uint8_t accelOff = 0x00;
        impl_->WriteUserData(impl_->FSUS_PARAM_ACCEL_SWITCH, &accelOff, 1);
        SPDLOG_INFO("[XRServo] Servo id {} online, angle={:.1f}deg", (int)impl_->servoId, impl_->angle);
    } else {
        impl_->lastError = "舵机未响应 (id=" + std::to_string((int)impl_->servoId) + ")";
        SPDLOG_WARN("[XRServo] Servo id {} not responding", (int)impl_->servoId);
    }
    return true;
}

void XRServo::Disconnect()
{
    if (impl_->serial_) {
        std::lock_guard<std::mutex> lock(g_serialMutex);
        auto it = g_serials.find(impl_->portName);
        if (it != g_serials.end()) {
            if (--it->second->refs <= 0) {
                if (it->second->h != INVALID_HANDLE_VALUE)
                    CloseHandle(it->second->h);
                g_serials.erase(it);
            }
        }
        impl_->serial_.reset();
    }
    impl_->ownsHandle = false;
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
    if (!impl_->Ping()) {
        impl_->torque = false;
        return false;
    }
    double a = impl_->angle;
    if (impl_->QueryAngle(a)) impl_->angle = a;
    // 锁定到当前位置：发"到当前角(1000ms)"使舵机保持扭矩
    bool ok = impl_->SendSetAngle(impl_->AngleReal2Raw(static_cast<float>(impl_->angle)), 1000, 0);
    impl_->torque = ok;
    return ok;
}

bool XRServo::TorqueOff()
{
    bool ok = impl_->SendDamping(500);
    impl_->torque = false;
    return ok;
}

bool XRServo::MoveToAngle(double angleDeg, int timeMs)
{
    double a = angleDeg;
    if (a > 180.0) a = 180.0;
    if (a < -180.0) a = -180.0;

    uint16_t interval = 1000;
    if (timeMs > 0) {
        interval = static_cast<uint16_t>(timeMs);
    } else {
        // 先真实查询当前角度再算到达周期，不能用缓存 impl_->angle：
        // 缓存会被遥测刷新成真实滞后位置，也可能仍是上次目标（dAngle≈0 →
        // interval 被 clamp 到 100ms → 多次点 Go 舵机突然全速猛冲）。
        double cur = impl_->angle;
        double real = cur;
        if (impl_->QueryAngle(real)) { impl_->angle = real; cur = real; }
        double dAngle = std::fabs(a - cur);
        double spd = impl_->speed > 0 ? impl_->speed : 50.0;
        uint16_t est = static_cast<uint16_t>((dAngle / spd) * 1000.0);
        interval = est > 0 ? est : 50;   // 下限 50ms，禁止 0 周期猛冲
    }
    impl_->lastMoveTimeMs = interval;
    bool ok = impl_->SendSetAngle(impl_->AngleReal2Raw(static_cast<float>(a)), interval, 0);
    SPDLOG_INFO("[XRServo] MoveToAngle id={} target={:.1f} cached={:.1f} interval={}ms -> {}",
                (int)impl_->servoId, a, impl_->angle, (int)interval, ok);
    impl_->angle = a;
    return ok;
}

int XRServo::GetLastMoveTimeMs() const
{
    return impl_->lastMoveTimeMs;
}

bool XRServo::MoveAtSpeed(double angleDeg, double speedDps)
{
    double a = angleDeg;
    if (a > 180.0) a = 180.0;
    if (a < -180.0) a = -180.0;

    double spd = speedDps > 0 ? speedDps : impl_->speed;
    if (spd <= 0) spd = 1.0;
    // 点动与 Go 走同一条已验证指令 cmd 8 (SET_ANGLE)：把速度换算成到达周期。
    // 真机实测 cmd 12 (SET_ANGLE_BY_VELOCITY) 点动无响应，弃用。
    double dAngle = std::fabs(a - impl_->angle);
    uint16_t interval = static_cast<uint16_t>((dAngle / spd) * 1000.0);
    if (interval < 50) interval = 50;
    if (interval > 30000) interval = 30000;
    impl_->lastMoveTimeMs = interval;
    bool ok = impl_->SendSetAngle(impl_->AngleReal2Raw(static_cast<float>(a)), interval, 0);
    SPDLOG_INFO("[XRServo] MoveAtSpeed id={} target={:.1f} cached={:.1f} speed={:.1f} interval={}ms -> {}",
                (int)impl_->servoId, a, impl_->angle, spd, (int)interval, ok);
    impl_->angle = a;
    return ok;
}

bool XRServo::Stop()
{
    // 用协议标准"控制模式停止"（mode=1 停止后保持锁力）让舵机在当前位置立即停下。
    // 不能再用 MoveToAngle(impl_->angle, 1000)：缓存角度来自 250ms 前的遥测，
    // 滞后于实时位置，Go 途中按停止会先反向转到缓存角再停（"往回转一点"）。
    // 也不能发扭矩关(Damping)，否则 J2/R 会失去力矩直接下垂。
    bool ok = impl_->SendControlModeStop(1, 500);
    impl_->lastMoveTimeMs = 50;   // 停止≈立即到位
    SPDLOG_INFO("[XRServo] Stop id={} -> {}", (int)impl_->servoId, ok);
    return ok;
}

bool XRServo::SetSpeed(double speedDps)
{
    impl_->speed = speedDps;
    return true;
}

double XRServo::ReadAngle()
{
    double a = 0.0;
    bool ok = impl_->QueryAngle(a);
    if (ok) {
        impl_->angle = a;
        impl_->online = true;
    } else {
        impl_->online = false;
    }
    SPDLOG_INFO("[XRServo] ReadAngle id={} -> {:.1f} (ok={})", (int)impl_->servoId, impl_->angle, ok);
    return impl_->angle;
}

ServoTelemetry XRServo::ReadTelemetry()
{
    ServoTelemetry t;
    t.angleDeg = impl_->angle;
    t.online = false;

    // online 必须以查询结果为准：串口句柄在拔线/断连后数值仍有效，
    // 不能只看 IsOpen()（曾导致 COM 断开后状态灯不变）。查询失败即离线。
    // 失败自动重试 1 次（共 2 次尝试）：瞬时坏帧/超时不应直接判离线，
    // 否则连续 4 个遥测周期失败会误触发全量重连（硬件并未掉线）
    for (int attempt = 0; attempt < 2 && !t.online; ++attempt) {
        if (impl_->QueryMonitor(t)) {
            impl_->angle = t.angleDeg;
            impl_->online = true;
            impl_->onlineWarned_ = false;
            t.online = true;
        } else if (attempt == 0 && !impl_->onlineWarned_) {
            impl_->onlineWarned_ = true;
            SPDLOG_WARN("[XRServo] ReadTelemetry id={} failed ({}), retrying",
                        (int)impl_->servoId, impl_->lastError);
        }
    }
    if (!t.online) impl_->online = false;
    return t;
}

bool XRServo::IsOnline() const
{
    return impl_->online;
}

bool XRServo::ClearAlarm()
{
    impl_->lastErrorCode = 0;
    impl_->lastError.clear();
    return true;
}

bool XRServo::Ping()
{
    return impl_->Ping();
}

std::string XRServo::GetLastError() const
{
    return impl_->lastError;
}
