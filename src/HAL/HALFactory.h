#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <functional>

#include "IMotionCard.h"
#include "IAxisServo.h"
#include "IEndEffector.h"
#include "ICamera.h"
#include "IPuffAlgorithm.h"

// ============================================================
// 硬件工厂 — 通过 JSON 配置文件中的类型名自动创建硬件实例
// 每种硬件类型维护一个静态注册表，支持运行时注册新实现
// ============================================================

template<typename TInterface>
class HardwareFactory
{
    using CreatorFunc = std::function<std::unique_ptr<TInterface>()>;
    using Registry = std::unordered_map<std::string, CreatorFunc>;

public:
    static HardwareFactory& Instance()
    {
        static HardwareFactory instance;
        return instance;
    }

    void Register(const std::string& typeName, CreatorFunc creator)
    {
        registry_[typeName] = std::move(creator);
    }

    std::unique_ptr<TInterface> Create(const std::string& typeName)
    {
        auto it = registry_.find(typeName);
        if (it != registry_.end())
            return it->second();
        return nullptr;
    }

    std::vector<std::string> AvailableTypes() const
    {
        std::vector<std::string> types;
        for (const auto& [name, _] : registry_)
            types.push_back(name);
        return types;
    }

private:
    Registry registry_;
};

// 快捷类型别名
using MotionCardFactory   = HardwareFactory<IMotionCard>;
using AxisServoFactory    = HardwareFactory<IAxisServo>;
using EndEffectorFactory  = HardwareFactory<IEndEffector>;
using CameraFactory       = HardwareFactory<ICamera>;
using PuffAlgorithmFactory = HardwareFactory<IPuffAlgorithm>;

// 便捷宏：自动注册硬件实现
#define REGISTER_MOTION_CARD(TypeName, ClassName) \
    namespace { \
        struct ClassName##_Registrar { \
            ClassName##_Registrar() { \
                MotionCardFactory::Instance().Register(TypeName, \
                    []() -> std::unique_ptr<IMotionCard> { \
                        return std::make_unique<ClassName>(); \
                    }); \
            } \
        }; \
        static ClassName##_Registrar s_##ClassName##_Reg; \
    }

#define REGISTER_AXIS_SERVO(TypeName, ClassName) \
    namespace { \
        struct ClassName##_Registrar { \
            ClassName##_Registrar() { \
                AxisServoFactory::Instance().Register(TypeName, \
                    []() -> std::unique_ptr<IAxisServo> { \
                        return std::make_unique<ClassName>(); \
                    }); \
            } \
        }; \
        static ClassName##_Registrar s_##ClassName##_Reg; \
    }

#define REGISTER_END_EFFECTOR(TypeName, ClassName) \
    namespace { \
        struct ClassName##_Registrar { \
            ClassName##_Registrar() { \
                EndEffectorFactory::Instance().Register(TypeName, \
                    []() -> std::unique_ptr<IEndEffector> { \
                        return std::make_unique<ClassName>(); \
                    }); \
            } \
        }; \
        static ClassName##_Registrar s_##ClassName##_Reg; \
    }

#define REGISTER_CAMERA(TypeName, ClassName) \
    namespace { \
        struct ClassName##_Registrar { \
            ClassName##_Registrar() { \
                CameraFactory::Instance().Register(TypeName, \
                    []() -> std::unique_ptr<ICamera> { \
                        return std::make_unique<ClassName>(); \
                    }); \
            } \
        }; \
        static ClassName##_Registrar s_##ClassName##_Reg; \
    }

#define REGISTER_PUFF_ALGORITHM(TypeName, ClassName) \
    namespace { \
        struct ClassName##_Registrar { \
            ClassName##_Registrar() { \
                PuffAlgorithmFactory::Instance().Register(TypeName, \
                    []() -> std::unique_ptr<IPuffAlgorithm> { \
                        return std::make_unique<ClassName>(); \
                    }); \
            } \
        }; \
        static ClassName##_Registrar s_##ClassName##_Reg; \
    }
