// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

#include "async_resp.hpp"
#include "dbus_singleton.hpp"
#include "dbus_utility.hpp"
#include "error_messages.hpp"
#include "logging.hpp"
#include "task.hpp"
#include "task_messages.hpp"

#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace redfish
{

static constexpr std::string_view cmParentPath =
    "/com/ibm/concurrent_maintenance";

// Fixed CM object paths
static constexpr std::string_view cmRemovePath =
    "/com/ibm/concurrent_maintenance/remove";
static constexpr std::string_view cmAddPath =
    "/com/ibm/concurrent_maintenance/add";

// Progress interface
static constexpr std::string_view cmProgressIface =
    "xyz.openbmc_project.Common.Progress";

// Terminal Status enum values
static constexpr std::string_view cmStatusCompleted =
    "xyz.openbmc_project.Common.Progress.OperationStatus.Completed";
static constexpr std::string_view cmStatusFailed =
    "xyz.openbmc_project.Common.Progress.OperationStatus.Failed";
static constexpr std::string_view cmStatusAborted =
    "xyz.openbmc_project.Common.Progress.OperationStatus.Aborted";

// Inventory interface that holds ReadyToRemove — owned by inventory service
static constexpr std::string_view readyToRemoveIface =
    "xyz.openbmc_project.State.ReadyToRemove";

/**
 * @brief Map a Common.Progress Status value to a Redfish task terminal state.
 *
 * Sets taskData->state and taskData->status and appends a task message when
 * the status is terminal.
 *
 * @return true  if the status is terminal (caller should return task::completed)
 * @return false if still in progress (caller should return !task::completed)
 */
inline bool mapCmStatus(const std::string& status,
                        const std::shared_ptr<task::TaskData>& taskData)
{
    if (status == cmStatusCompleted)
    {
        taskData->messages.emplace_back(
            messages::taskCompletedOK(std::to_string(taskData->index)));
        taskData->state = "Completed";
        taskData->status = "OK";
        return true;
    }
    if (status == cmStatusFailed || status == cmStatusAborted)
    {
        taskData->messages.emplace_back(
            messages::taskAborted(std::to_string(taskData->index)));
        taskData->state = "Exception";
        taskData->status = "Warning";
        return true;
    }
    // InProgress or any other non-terminal value — keep waiting
    return false;
}

/**
 * @brief Start an async Redfish task tracking a Concurrent Maintenance
 *        operation triggered by a ReadyToRemove property change.
 *
 * Flow:
 *   1. Install an InterfacesAdded match on @ref cmParentPath so the task is
 *      notified when the CM daemon creates the /remove or /add object.
 *   2. Create the Redfish task, arm its 30-minute timer, write 202 Accepted.
 *   3. Resolve the inventory service for @p inventoryPath (the service owning
 *      xyz.openbmc_project.State.ReadyToRemove).
 *   4. Write ReadyToRemove to the inventory object — this triggers the CM
 *      daemon to create the CM object and emit InterfacesAdded.
 *
 * The task callback handles two stages automatically:
 *   Stage 1 (InterfacesAdded): confirms the CM object appeared, reads the
 *     initial Progress.Status from the signal body.  If already terminal,
 *     completes immediately.  Otherwise replaces task->match with a
 *     PropertiesChanged match on the known CM object path.
 *   Stage 2 (PropertiesChanged): reads Status updates until terminal.
 *
 * @param asyncResp      Shared response — populateResp() writes 202 here.
 * @param payload        Task payload built from the original PATCH request.
 * @param inventoryPath  D-Bus object path of the inventory FRU.
 * @param readyToRemove  true → remove flow (/remove); false → add flow (/add).
 */
inline void startCmTask(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                        task::Payload&& payload,
                        const std::string& inventoryPath, bool readyToRemove)
{
    // The CM object path is fixed and known statically
    const std::string cmObjPath(readyToRemove ? cmRemovePath : cmAddPath);

    // Stage-1 match string: InterfacesAdded on the CM parent ObjectManager.
    // Built before startTimer() so the match is live before setProperty fires.
    const std::string stage1MatchStr =
        "type='signal',"
        "interface='org.freedesktop.DBus.ObjectManager',"
        "member='InterfacesAdded',"
        "path='" +
        std::string(cmParentPath) + "'";

    std::shared_ptr<task::TaskData> task = task::TaskData::createTask(
        [cmObjPath, stageOneDone = false](
            const boost::system::error_code& ec, sdbusplus::message_t& msg,
            const std::shared_ptr<task::TaskData>& taskData) mutable -> bool {
            // Timer expiry: ec is set, msg is a default-constructed message
            if (ec)
            {
                BMCWEB_LOG_ERROR("CM task timed out for {}", cmObjPath);
                taskData->messages.emplace_back(messages::internalError());
                taskData->state = "Exception";
                taskData->status = "Warning";
                return task::completed;
            }

            if (!stageOneDone)
            {
                // ── Stage 1: InterfacesAdded ──────────────────────────────
                sdbusplus::message::object_path addedPath;
                dbus::utility::DBusInterfacesMap ifaceMap;
                msg.read(addedPath, ifaceMap);

                if (addedPath.str != cmObjPath)
                {
                    // A different object appeared under the parent — not ours
                    return !task::completed;
                }

                // Read the initial Status from the signal body so we can
                // detect an already-terminal state without waiting for a
                // PropertiesChanged signal
                for (const auto& [iface, props] : ifaceMap)
                {
                    if (iface != cmProgressIface)
                    {
                        continue;
                    }
                    for (const auto& [propName, propVal] : props)
                    {
                        if (propName != "Status")
                        {
                            continue;
                        }
                        const std::string* status =
                            std::get_if<std::string>(&propVal);
                        if (status != nullptr &&
                            mapCmStatus(*status, taskData))
                        {
                            return task::completed;
                        }
                        break;
                    }
                    break;
                }

                // CM object exists and is still in progress.
                // Replace the InterfacesAdded match with a PropertiesChanged
                // match on the now-known fixed CM object path.
                stageOneDone = true;
                taskData->match = std::make_unique<sdbusplus::bus::match_t>(
                    static_cast<sdbusplus::bus_t&>(
                        *crow::connections::systemBus),
                    "type='signal',"
                    "interface='org.freedesktop.DBus.Properties',"
                    "member='PropertiesChanged',"
                    "path='" +
                        cmObjPath + "'",
                    [taskData](sdbusplus::message_t& innerMsg) {
                        boost::system::error_code noEc;
                        if (taskData->callback(noEc, innerMsg, taskData) ==
                            task::completed)
                        {
                            taskData->timer.cancel();
                            taskData->finishTask();
                            task::TaskData::sendTaskEvent(taskData->state,
                                                         taskData->index);
                            boost::asio::post(
                                crow::connections::systemBus->get_io_context(),
                                [taskData] { taskData->match.reset(); });
                        }
                    });
                return !task::completed;
            }

            // ── Stage 2: PropertiesChanged ────────────────────────────────
            std::string iface;
            dbus::utility::DBusPropertiesMap values;
            msg.read(iface, values);

            if (iface != cmProgressIface)
            {
                return !task::completed;
            }

            for (const auto& [propName, propVal] : values)
            {
                if (propName != "Status")
                {
                    continue;
                }
                const std::string* status =
                    std::get_if<std::string>(&propVal);
                if (status != nullptr && mapCmStatus(*status, taskData))
                {
                    return task::completed;
                }
            }
            return !task::completed;
        },
        stage1MatchStr);

    task->state = "Running";
    // startTimer() installs the sdbusplus::bus::match_t — must happen before
    // setProperty so the match is live before the CM daemon can react
    task->startTimer(std::chrono::minutes(30));
    task->payload.emplace(std::move(payload));
    task->populateResp(asyncResp->res);

    // Resolve the inventory service that owns xyz.openbmc_project.State.ReadyToRemove
    // on this object, then write the property.  The write is the trigger that
    // causes the CM daemon to create the CM object and emit InterfacesAdded.
    // Any error here is logged only — the task timer will expire if CM never fires.
    constexpr std::array<std::string_view, 1> rtrIface = {readyToRemoveIface};
    dbus::utility::getDbusObject(
        inventoryPath, rtrIface,
        [inventoryPath, readyToRemove](
            const boost::system::error_code& ec,
            const dbus::utility::MapperGetObject& object) {
            if (ec || object.empty())
            {
                BMCWEB_LOG_ERROR(
                    "getDbusObject failed for ReadyToRemove on {}: {}",
                    inventoryPath, ec.message());
                return;
            }
            const std::string& service = object.begin()->first;
            sdbusplus::asio::setProperty(
                *crow::connections::systemBus, service, inventoryPath,
                std::string(readyToRemoveIface), "ReadyToRemove",
                readyToRemove,
                [inventoryPath](const boost::system::error_code& ec2) {
                    if (ec2)
                    {
                        BMCWEB_LOG_ERROR(
                            "Failed to set ReadyToRemove on {}: {}",
                            inventoryPath, ec2.message());
                    }
                });
        });
}

} // namespace redfish
