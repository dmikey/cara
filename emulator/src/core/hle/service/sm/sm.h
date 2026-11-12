// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <boost/serialization/weak_ptr.hpp>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/split_member.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/unordered_map.hpp>
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/object.h"
#include "core/hle/kernel/server_port.h"
#include "core/hle/result.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class ClientSession;
class SessionRequestHandler;
} // namespace Kernel

namespace Service::SM {

class SRV;

constexpr ResultCode ERR_SERVICE_NOT_REGISTERED(1, ErrorModule::SRV, ErrorSummary::WouldBlock,
                                                ErrorLevel::Temporary); // 0xD0406401
constexpr ResultCode ERR_MAX_CONNECTIONS_REACHED(2, ErrorModule::SRV, ErrorSummary::WouldBlock,
                                                 ErrorLevel::Temporary); // 0xD0406402
constexpr ResultCode ERR_INVALID_NAME_SIZE(5, ErrorModule::SRV, ErrorSummary::WrongArgument,
                                           ErrorLevel::Permanent); // 0xD9006405
constexpr ResultCode ERR_ACCESS_DENIED(6, ErrorModule::SRV, ErrorSummary::InvalidArgument,
                                       ErrorLevel::Permanent); // 0xD8E06406
constexpr ResultCode ERR_NAME_CONTAINS_NUL(7, ErrorModule::SRV, ErrorSummary::WrongArgument,
                                           ErrorLevel::Permanent); // 0xD9006407
constexpr ResultCode ERR_ALREADY_REGISTERED(ErrorDescription::AlreadyExists, ErrorModule::OS,
                                            ErrorSummary::WrongArgument,
                                            ErrorLevel::Permanent); // 0xD9001BFC

class ServiceManager {
public:
    static void InstallInterfaces(Core::System& system);

    explicit ServiceManager(Core::System& system);

    ResultVal<std::shared_ptr<Kernel::ServerPort>> RegisterService(std::string name,
                                                                   unsigned int max_sessions);
    ResultVal<std::shared_ptr<Kernel::ClientPort>> GetServicePort(const std::string& name);
    ResultVal<std::shared_ptr<Kernel::ClientSession>> ConnectToService(const std::string& name);
    // For IPC Recorder
    std::string GetServiceNameByPortId(u32 port) const;

    template <typename T>
    std::shared_ptr<T> GetService(const std::string& service_name) const {
        static_assert(std::is_base_of_v<Kernel::SessionRequestHandler, T>,
                      "Not a base of ServiceFrameworkBase");
        auto service = registered_services.find(service_name);
        if (service == registered_services.end()) {
            LOG_DEBUG(Service, "Can't find service: {}", service_name);
            return nullptr;
        }
        auto port = service->second->GetServerPort();
        if (port == nullptr) {
            return nullptr;
        }
        return std::static_pointer_cast<T>(port->hle_handler);
    }

private:
    Core::System& system;
    std::weak_ptr<SRV> srv_interface;

    /// Map of registered services, retrieved using GetServicePort or ConnectToService.
    std::unordered_map<std::string, std::shared_ptr<Kernel::ClientPort>> registered_services;

    // For IPC Recorder
    /// client port Object id -> service name
    std::unordered_map<u32, std::string> registered_services_inverse;

    template <class Archive>
    void save(Archive& ar, const unsigned int file_version) const {
        const u32 count = static_cast<u32>(registered_services.size());
        ar << count;
        for (const auto& pair : registered_services) {
            if (std::getenv("CARA_STATE_TRACE")) std::fprintf(stderr, "service: %s\n", pair.first.c_str());
            try { ar << pair.first; ar << pair.second; }
            catch (const std::exception& error) { throw std::runtime_error("Save-state service " + pair.first + ": " + error.what()); }
        }
        ar << srv_interface;
    }

    template <class Archive>
    void load(Archive& ar, const unsigned int file_version) {
        u32 count = 0; ar >> count;
        if (count > 4096) throw std::runtime_error("Invalid saved service count");
        registered_services.clear();
        for (u32 i = 0; i < count; ++i) {
            std::string name; std::shared_ptr<Kernel::ClientPort> port;
            ar >> name; ar >> port;
            if (name.empty() || name.size() > 8 || !port || !registered_services.emplace(name, port).second)
                throw std::runtime_error("Invalid saved service entry");
        }
        ar >> srv_interface;
        registered_services_inverse.clear();
        for (const auto& pair : registered_services) {
            registered_services_inverse.emplace(pair.second->GetObjectId(), pair.first);
        }
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()

    friend class boost::serialization::access;
};

} // namespace Service::SM
