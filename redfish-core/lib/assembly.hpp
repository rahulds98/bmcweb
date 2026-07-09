// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

#include "app.hpp"
#include "assembly_battery_cm.hpp"
#include "async_resp.hpp"
#include "concurrent_maintenance_task.hpp"
#include "dbus_singleton.hpp"
#include "dbus_utility.hpp"
#include "error_messages.hpp"
#include "generated/enums/resource.hpp"
#include "http_request.hpp"
#include "http_response.hpp"
#include "led.hpp"
#include "logging.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"
#include "utils/assembly_utils.hpp"
#include "utils/asset_utils.hpp"
#include "utils/json_utils.hpp"
#include "utils/name_utils.hpp"

#include <asm-generic/errno.h>

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/error_code.hpp>
#include <boost/url/format.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/property.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace redfish
{

/**
 * @brief Get Location code for the given assembly.
 * @param[in] asyncResp - Shared pointer for asynchronous calls.
 * @param[in] serviceName - Service in which the assembly is hosted.
 * @param[in] assembly - Assembly object.
 * @param[in] assemblyJsonPtr - json-keyname on the assembly list output.
 * @return None.
 */
inline void getAssemblyLocationCode(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& serviceName, const std::string& assembly,
    const nlohmann::json::json_pointer& assemblyJsonPtr)
{
    sdbusplus::asio::getProperty<std::string>(
        *crow::connections::systemBus, serviceName, assembly,
        "xyz.openbmc_project.Inventory.Decorator.LocationCode", "LocationCode",
        [asyncResp, assembly, assemblyJsonPtr](
            const boost::system::error_code& ec, const std::string& value) {
            if (ec)
            {
                if (ec.value() != EBADR)
                {
                    BMCWEB_LOG_ERROR("DBUS response error: {} for assembly {}",
                                     ec.value(), assembly);
                    messages::internalError(asyncResp->res);
                }
                return;
            }

            asyncResp->res.jsonValue[assemblyJsonPtr]["Location"]
                                    ["PartLocation"]["ServiceLabel"] = value;
        });
}

inline void getAssemblyState(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const auto& serviceName, const auto& assembly,
    const nlohmann::json::json_pointer& assemblyJsonPtr)
{
    asyncResp->res.jsonValue[assemblyJsonPtr]["Status"]["State"] =
        resource::State::Enabled;

    dbus::utility::getProperty<bool>(
        serviceName, assembly, "xyz.openbmc_project.Inventory.Item", "Present",
        [asyncResp, assemblyJsonPtr,
         assembly](const boost::system::error_code& ec, const bool value) {
            if (ec)
            {
                if (ec.value() != EBADR)
                {
                    BMCWEB_LOG_ERROR("DBUS response error: {}", ec.value());
                    messages::internalError(asyncResp->res);
                }
                return;
            }

            if (!value)
            {
                asyncResp->res.jsonValue[assemblyJsonPtr]["Status"]["State"] =
                    resource::State::Absent;
            }

            std::string fru =
                sdbusplus::message::object_path(assembly).filename();
            // Special handling for LCD and base panel CM.
            if (fru == "panel0" || fru == "panel1")
            {
                asyncResp->res.jsonValue[assemblyJsonPtr]["Oem"]["OpenBMC"]
                                        ["@odata.type"] =
                    "#OpenBMCAssembly.v1_0_0.OpenBMC";

                // if panel is not present, implies it is already removed or
                // can be placed.
                asyncResp->res.jsonValue[assemblyJsonPtr]["Oem"]["OpenBMC"]
                                        ["ReadyToRemove"] = !value;
            }
        });
}

void getAssemblyHealth(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const auto& serviceName, const auto& assembly,
                       const nlohmann::json::json_pointer& assemblyJsonPtr)
{
    asyncResp->res.jsonValue[assemblyJsonPtr]["Status"]["Health"] =
        resource::Health::OK;

    dbus::utility::getProperty<bool>(
        serviceName, assembly,
        "xyz.openbmc_project.State.Decorator.OperationalStatus", "Functional",
        [asyncResp, assemblyJsonPtr](const boost::system::error_code& ec,
                                     bool functional) {
            if (ec)
            {
                if (ec.value() != EBADR)
                {
                    BMCWEB_LOG_ERROR("DBUS response error {}", ec.value());
                    messages::internalError(asyncResp->res);
                }
                return;
            }

            if (!functional)
            {
                asyncResp->res.jsonValue[assemblyJsonPtr]["Status"]["Health"] =
                    resource::Health::Critical;
            }
        });
}

/**
 * @brief Populate Oem/OpenBMC/ReadyToRemove for assemblies that implement
 *        xyz.openbmc_project.State.ReadyToRemove.
 */
inline void getAssemblyReadyToRemove(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& serviceName, const std::string& assembly,
    const nlohmann::json::json_pointer& assemblyJsonPtr)
{
    dbus::utility::getProperty<bool>(
        serviceName, assembly, std::string(readyToRemoveIface), "ReadyToRemove",
        [asyncResp, assemblyJsonPtr](const boost::system::error_code& ec,
                                     bool value) {
            if (ec)
            {
                if (ec.value() != EBADR)
                {
                    BMCWEB_LOG_ERROR("DBUS response error: {}", ec.value());
                    messages::internalError(asyncResp->res);
                }
                return;
            }
            asyncResp->res.jsonValue[assemblyJsonPtr]["Oem"]["OpenBMC"]
                                    ["@odata.type"] =
                "#OpenBMCAssembly.v1_0_0.OpenBMC";
            asyncResp->res.jsonValue[assemblyJsonPtr]["Oem"]["OpenBMC"]
                                    ["ReadyToRemove"] = value;
        });
}

inline void afterGetDbusObject(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& assembly,
    const nlohmann::json::json_pointer& assemblyJsonPtr,
    const boost::system::error_code& ec,
    const dbus::utility::MapperGetObject& object)
{
    if (ec)
    {
        BMCWEB_LOG_ERROR("DBUS response error : {} for assembly {}", ec.value(),
                         assembly);
        messages::internalError(asyncResp->res);
        return;
    }

    nlohmann::json::json_pointer ptr = assemblyJsonPtr;
    ptr /= "Name";
    name_util::getPrettyName(asyncResp, assembly, object, ptr);

    for (const auto& [serviceName, interfaceList] : object)
    {
        for (const auto& interface : interfaceList)
        {
            if (interface == "xyz.openbmc_project.Inventory.Decorator.Asset")
            {
                asset_utils::getAssetInfo(asyncResp, serviceName, assembly,
                                          assemblyJsonPtr, true, false);
            }
            else if (interface ==
                     "xyz.openbmc_project.Inventory.Decorator.LocationCode")
            {
                getAssemblyLocationCode(asyncResp, serviceName, assembly,
                                        assemblyJsonPtr);
            }
            else if (interface == "xyz.openbmc_project.Inventory.Item")
            {
                getAssemblyState(asyncResp, serviceName, assembly,
                                 assemblyJsonPtr);
            }
            else if (interface ==
                     "xyz.openbmc_project.State.Decorator.OperationalStatus")
            {
                getAssemblyHealth(asyncResp, serviceName, assembly,
                                  assemblyJsonPtr);
            }
            else if (interface == readyToRemoveIface)
            {
                getAssemblyReadyToRemove(asyncResp, serviceName, assembly,
                                         assemblyJsonPtr);
            }
        }
    }
}

/**
 * @brief Get properties for the assemblies associated to given chassis
 * @param[in] asyncResp - Shared pointer for asynchronous calls.
 * @param[in] chassisId - Chassis the assemblies are associated with.
 * @param[in] assemblies - list of all the assemblies associated with the
 * chassis.
 * @return None.
 */
inline void getAssemblyProperties(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId, const std::vector<std::string>& assemblies)
{
    BMCWEB_LOG_DEBUG("Get properties for assembly associated");

    std::size_t assemblyIndex = 0;
    for (const std::string& assembly : assemblies)
    {
        nlohmann::json::object_t item;
        item["@odata.type"] = "#Assembly.v1_5_1.AssemblyData";
        item["@odata.id"] = boost::urls::format(
            "/redfish/v1/Chassis/{}/Assembly#/Assemblies/{}", chassisId,
            std::to_string(assemblyIndex));
        item["MemberId"] = std::to_string(assemblyIndex);
        item["Name"] = sdbusplus::message::object_path(assembly).filename();

        asyncResp->res.jsonValue["Assemblies"].emplace_back(item);

        nlohmann::json::json_pointer assemblyJsonPtr(
            "/Assemblies/" + std::to_string(assemblyIndex));

        // Handle special case for tod_battery assembly OEM ReadyToRemove
        // property NOTE: The following method for the special case of the
        // tod_battery ReadyToRemove property only works when there is only ONE
        // adcsensor handled by the adcsensor application.
        if (sdbusplus::message::object_path(assembly).filename() ==
            "tod_battery")
        {
            getReadyToRemoveOfTodBattery(asyncResp, assemblyIndex);
        }

        dbus::utility::getDbusObject(
            assembly, assemblyInterfaces,
            std::bind_front(afterGetDbusObject, asyncResp, assembly,
                            assemblyJsonPtr));

        getLocationIndicatorActive(
            asyncResp, assembly, [asyncResp, assemblyJsonPtr](bool asserted) {
                asyncResp->res
                    .jsonValue[assemblyJsonPtr]["LocationIndicatorActive"] =
                    asserted;
            });

        nlohmann::json& assemblyArray = asyncResp->res.jsonValue["Assemblies"];
        asyncResp->res.jsonValue["Assemblies@odata.count"] =
            assemblyArray.size();

        assemblyIndex++;
    }
}

inline void afterHandleChassisAssemblyGet(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisID, const boost::system::error_code& ec,
    const std::vector<std::string>& assemblyList)
{
    if (ec)
    {
        BMCWEB_LOG_WARNING("Chassis {} not found", chassisID);
        messages::resourceNotFound(asyncResp->res, "Chassis", chassisID);
        return;
    }

    asyncResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/Assembly/Assembly.json>; rel=describedby");

    asyncResp->res.jsonValue["@odata.type"] = "#Assembly.v1_5_1.Assembly";
    asyncResp->res.jsonValue["@odata.id"] =
        boost::urls::format("/redfish/v1/Chassis/{}/Assembly", chassisID);
    asyncResp->res.jsonValue["Name"] = "Assembly Collection";
    asyncResp->res.jsonValue["Id"] = "Assembly";

    asyncResp->res.jsonValue["Assemblies"] = nlohmann::json::array();
    asyncResp->res.jsonValue["Assemblies@odata.count"] = 0;

    if (!assemblyList.empty())
    {
        getAssemblyProperties(asyncResp, chassisID, assemblyList);
    }
}

/**
 * @param[in] asyncResp - Shared pointer for asynchronous calls.
 * @param[in] chassisID - Chassis to which the assemblies are
 * associated.
 *
 * @return None.
 */
inline void handleChassisAssemblyGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisID)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    BMCWEB_LOG_DEBUG("Get chassis Assembly");
    assembly_utils::getChassisAssembly(
        asyncResp, chassisID,
        std::bind_front(afterHandleChassisAssemblyGet, asyncResp, chassisID));
}

inline void handleChassisAssemblyHead(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisID)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    assembly_utils::getChassisAssembly(
        asyncResp, chassisID,
        [asyncResp,
         chassisID](const boost::system::error_code& ec,
                    const std::vector<std::string>& /*assemblyList*/) {
            if (ec)
            {
                BMCWEB_LOG_WARNING("Chassis {} not found", chassisID);
                messages::resourceNotFound(asyncResp->res, "Chassis",
                                           chassisID);
                return;
            }
            asyncResp->res.addHeader(
                boost::beast::http::field::link,
                "</redfish/v1/JsonSchemas/Assembly.json>; rel=describedby");
        });
}

inline void afterHandleChassisAssemblyPatch(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisID,
    std::vector<nlohmann::json::object_t>& assemblyData,
    task::Payload payload,
    const boost::system::error_code& ec,
    const std::vector<std::string>& assemblyList)
{
    if (ec)
    {
        BMCWEB_LOG_WARNING("Chassis {} not found", chassisID);
        messages::resourceNotFound(asyncResp->res, "Chassis", chassisID);
        return;
    }

    std::map<std::string, bool> locationIndicatorActiveMap;
    std::map<std::string, nlohmann::json> oemIndicatorMap;

    for (nlohmann::json::object_t& item : assemblyData)
    {
        std::optional<std::string> memberId;
        std::optional<bool> locationIndicatorActive;
        std::optional<nlohmann::json> oem;
        if (!json_util::readJsonObject(item, asyncResp->res, "MemberId",
                                       memberId, "LocationIndicatorActive",
                                       locationIndicatorActive, "Oem", oem))
        {
            return;
        }
        if (locationIndicatorActive)
        {
            if (memberId)
            {
                locationIndicatorActiveMap[*memberId] =
                    *locationIndicatorActive;
            }
            else
            {
                BMCWEB_LOG_WARNING(
                    "Property Missing - MemberId must be included with LocationIndicatorActive ");
                messages::propertyMissing(asyncResp->res, "MemberId");
                return;
            }
        }
        if (oem)
        {
            if (memberId)
            {
                oemIndicatorMap[*memberId] = *oem;
            }
            else
            {
                BMCWEB_LOG_WARNING(
                    "Property Missing - MemberId must be included with Oem property");
                messages::propertyMissing(asyncResp->res, "MemberId");
                return;
            }
        }
    }

    std::size_t assemblyIndex = 0;
    for (const auto& assembly : assemblyList)
    {
        auto iter =
            locationIndicatorActiveMap.find(std::to_string(assemblyIndex));

        if (iter != locationIndicatorActiveMap.end())
        {
            setLocationIndicatorActive(asyncResp, assembly, iter->second);
        }

        auto iter2 = oemIndicatorMap.find(std::to_string(assemblyIndex));

        if (iter2 != oemIndicatorMap.end())
        {
            std::optional<bool> readytoremove;
            if (!json_util::readJson(iter2->second, asyncResp->res,
                                     "OpenBMC/ReadyToRemove", readytoremove))
            {
                BMCWEB_LOG_WARNING("Property Value Format Error ");
                messages::propertyValueFormatError(
                    asyncResp->res, iter2->second, "OpenBMC/ReadyToRemove");
                return;
            }

            if (!readytoremove)
            {
                BMCWEB_LOG_WARNING("Property Missing ");
                messages::propertyMissing(asyncResp->res,
                                          "OpenBMC/ReadyToRemove");
                return;
            }

            // Handle special case for tod_battery assembly OEM ReadyToRemove
            // property. NOTE: The following method for the special case of the
            // tod_battery ReadyToRemove property only works when there is only
            // ONE adcsensor handled by the adcsensor application.
            if (sdbusplus::message::object_path(assembly).filename() ==
                "tod_battery")
            {
                doBatteryCM(asyncResp, assembly, readytoremove.value());
            }

            // Special handling for LCD and base panel. This is required to
            // support concurrent maintenance for base and LCD panel.
            else if (sdbusplus::message::object_path(assembly).filename() ==
                         "panel0" ||
                     sdbusplus::message::object_path(assembly).filename() ==
                         "panel1")
            {
                // Based on the status of readytoremove flag, inventory data
                // like CCIN and present property needs to be updated for this
                // FRU.
                // readytoremove as true implies FRU has been prepared for
                // removal. Set action as "deleteFRUVPD". This is the api
                // exposed by vpd-manager to clear CCIN and set present
                // property as false for the FRU.
                // readytoremove as false implies FRU has been replaced. Set
                // action as "CollectFRUVPD". This is the api exposed by
                // vpd-manager to recollect vpd for a given FRU.
                std::string action = "CollectFRUVPD";
                if (readytoremove.value())
                {
                    action = "deleteFRUVPD";
                }

                dbus::utility::async_method_call(
                    [asyncResp, action](const boost::system::error_code& ec1) {
                        if (ec1)
                        {
                            BMCWEB_LOG_ERROR(
                                "Call to Manager failed for action: {} with error: {}",
                                action, ec1.value());
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    },
                    "com.ibm.VPD.Manager", "/com/ibm/VPD/Manager",
                    "com.ibm.VPD.Manager", action,
                    sdbusplus::message::object_path(assembly));
            }
            else
            {
                // CM-backed assembly: start async task tracking the CM daemon
                startCmTask(asyncResp, task::Payload(payload), assembly,
                            readytoremove.value());
            }
        }
        assemblyIndex++;
    }
}

inline void handleChassisAssemblyPatch(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisID)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    std::vector<nlohmann::json::object_t> assemblyData;
    if (!redfish::json_util::readJsonPatch(req, asyncResp->res, "Assemblies",
                                           assemblyData))
    {
        return;
    }

    assembly_utils::getChassisAssembly(
        asyncResp, chassisID,
        [asyncResp, chassisID, assemblyData = std::move(assemblyData),
         payload = task::Payload(req)](
            const boost::system::error_code& ec,
            const std::vector<std::string>& assemblyList) mutable {
            afterHandleChassisAssemblyPatch(asyncResp, chassisID, assemblyData,
                                            std::move(payload), ec,
                                            assemblyList);
        });
}

/**
 * Systems derived class for delivering Assembly Schema.
 */
inline void requestRoutesAssembly(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/Assembly/")
        .privileges(redfish::privileges::headAssembly)
        .methods(boost::beast::http::verb::head)(
            std::bind_front(handleChassisAssemblyHead, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/Assembly/")
        .privileges(redfish::privileges::getAssembly)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleChassisAssemblyGet, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/Assembly/")
        .privileges(redfish::privileges::patchAssembly)
        .methods(boost::beast::http::verb::patch)(
            std::bind_front(handleChassisAssemblyPatch, std::ref(app)));
}

} // namespace redfish
