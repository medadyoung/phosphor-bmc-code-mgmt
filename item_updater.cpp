#include "config.h"

#include "item_updater.hpp"

#include "images.hpp"
#include "serialize.hpp"
#include "version.hpp"

#include <experimental/filesystem>
#include <fstream>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/log.hpp>
#include <queue>
#include <set>
#include <string>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Software/Image/error.hpp>

namespace phosphor
{
namespace software
{
namespace updater
{

// When you see server:: you know we're referencing our base class
namespace server = sdbusplus::xyz::openbmc_project::Software::server;
namespace control = sdbusplus::xyz::openbmc_project::Control::server;

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Software::Image::Error;
using namespace phosphor::software::image;
namespace fs = std::experimental::filesystem;
using NotAllowed = sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed;
using VersionPurpose = server::Version::VersionPurpose;

void ItemUpdater::createActivation(sdbusplus::message::message& msg)
{

    using SVersion = server::Version;
    using VersionClass = phosphor::software::manager::Version;
    namespace mesg = sdbusplus::message;
    namespace variant_ns = mesg::variant_ns;

    mesg::object_path objPath;
    auto purpose = VersionPurpose::Unknown;
    std::string version;
    std::map<std::string, std::map<std::string, mesg::variant<std::string>>>
        interfaces;
    msg.read(objPath, interfaces);
    std::string path(std::move(objPath));
    std::string filePath;

    for (const auto& intf : interfaces)
    {
        if (intf.first == VERSION_IFACE)
        {
            for (const auto& property : intf.second)
            {
                if (property.first == "Purpose")
                {
                    auto value = SVersion::convertVersionPurposeFromString(
                        variant_ns::get<std::string>(property.second));
                    if (value == VersionPurpose::BMC ||
                        value == VersionPurpose::Host)
                    {
                        purpose = value;
                    }
                }
                else if (property.first == "Version")
                {
                    version = variant_ns::get<std::string>(property.second);
                }
            }
        }
        else if (intf.first == FILEPATH_IFACE)
        {
            for (const auto& property : intf.second)
            {
                if (property.first == "Path")
                {
                    filePath = variant_ns::get<std::string>(property.second);
                }
            }
        }
    }
    if (version.empty() || filePath.empty() ||
        purpose == VersionPurpose::Unknown)
    {
        return;
    }

    // Version id is the last item in the path
    auto pos = path.rfind("/");
    if (pos == std::string::npos)
    {
        log<level::ERR>("No version id found in object path",
                        entry("OBJPATH=%s", path.c_str()));
        return;
    }

    auto versionId = path.substr(pos + 1);

    if (activations.find(versionId) == activations.end())
    {
        // Determine the Activation state by processing the given image dir.
        auto activationState = server::Activation::Activations::Invalid;
        ItemUpdater::ActivationStatus result =
            ItemUpdater::validateSquashFSImage(filePath);
        AssociationList associations = {};

        if (result == ItemUpdater::ActivationStatus::ready)
        {
            activationState = server::Activation::Activations::Ready;
            // Create an association to the BMC inventory item
            associations.emplace_back(
                std::make_tuple(ACTIVATION_FWD_ASSOCIATION,
                                ACTIVATION_REV_ASSOCIATION, bmcInventoryPath));
        }

        activations.insert(std::make_pair(
            versionId,
            std::make_unique<Activation>(bus, path, *this, versionId,
                                         activationState, associations)));

        auto versionPtr = std::make_unique<VersionClass>(
            bus, path, version, purpose, filePath,
            std::bind(&ItemUpdater::erase, this, std::placeholders::_1));
        versionPtr->deleteObject =
            std::make_unique<phosphor::software::manager::Delete>(bus, path,
                                                                  *versionPtr);
        versions.insert(std::make_pair(versionId, std::move(versionPtr)));
    }
    return;
}

void ItemUpdater::processHostImage()
{
    using VersionClass = phosphor::software::manager::Version;

    auto activationState = server::Activation::Activations::Active;
    auto purpose = server::Version::VersionPurpose::Host;
    auto biosRelease = fs::path(BIOS_FW_FILE);

    if (!fs::is_regular_file(biosRelease))
    {
        log<level::ERR>("Failed to read biosRelease",
                        entry("FILENAME=%s", biosRelease.string().c_str()));
        return;
    }

    // Read bios-release from /usr/share/phosphor-bmc-code-mgmt to get the initial BIOS version
    // The version may be chenaged by ipmi command, set system info.
    auto initialVersion = VersionClass::getBMCVersion(biosRelease);
    auto id = VersionClass::getId(initialVersion);
    auto path = fs::path(SOFTWARE_OBJPATH) / id;
    createFunctionalAssociation(path);

    AssociationList associations = {};

    // Create an association to the BMC inventory item
    associations.emplace_back(std::make_tuple(ACTIVATION_FWD_ASSOCIATION,
                                              ACTIVATION_REV_ASSOCIATION,
                                              hostInventoryPath));

    // Create an active association since this image is active
    createActiveAssociation(path);

    // Create Version instance for this version.
    auto versionPtr = std::make_unique<VersionClass>(
        bus, path, initialVersion, purpose, "",
        std::bind(&ItemUpdater::erase, this, std::placeholders::_1));

    versions.insert(std::make_pair(id, std::move(versionPtr)));

    // Create Activation instance for this version.
    activations.insert(std::make_pair(
        id, std::make_unique<Activation>(bus, path, *this, id, activationState,
                                         associations)));

    uint8_t priority = std::numeric_limits<uint8_t>::max();
    if (!restoreFromFile(id, priority))
        priority = 0;

    activations.find(id)->second->redundancyPriority =
        std::make_unique<RedundancyPriority>(
            bus, path, *(activations.find(id)->second), priority, false);

    return;
}

void ItemUpdater::processBMCImage()
{
    using VersionClass = phosphor::software::manager::Version;

    // Check MEDIA_DIR and create if it does not exist
    try
    {
        if (!fs::is_directory(MEDIA_DIR))
        {
            fs::create_directory(MEDIA_DIR);
        }
    }
    catch (const fs::filesystem_error& e)
    {
        log<level::ERR>("Failed to prepare dir", entry("ERR=%s", e.what()));
        return;
    }

    // Read os-release from /etc/ to get the functional BMC version
    auto functionalVersion = VersionClass::getBMCVersion(OS_RELEASE_FILE);

    // Read os-release from folders under /media/ to get
    // BMC Software Versions.
    for (const auto& iter : fs::directory_iterator(MEDIA_DIR))
    {
        auto activationState = server::Activation::Activations::Active;
        static const auto BMC_RO_PREFIX_LEN = strlen(BMC_ROFS_PREFIX);

        // Check if the BMC_RO_PREFIXis the prefix of the iter.path
        if (0 ==
            iter.path().native().compare(0, BMC_RO_PREFIX_LEN, BMC_ROFS_PREFIX))
        {
            // The versionId is extracted from the path
            // for example /media/ro-2a1022fe.
            auto id = iter.path().native().substr(BMC_RO_PREFIX_LEN);
            auto osRelease = iter.path() / OS_RELEASE_FILE;
            if (!fs::is_regular_file(osRelease))
            {
                log<level::ERR>(
                    "Failed to read osRelease",
                    entry("FILENAME=%s", osRelease.string().c_str()));
                ItemUpdater::erase(id);
                continue;
            }
            auto version = VersionClass::getBMCVersion(osRelease);
            if (version.empty())
            {
                log<level::ERR>(
                    "Failed to read version from osRelease",
                    entry("FILENAME=%s", osRelease.string().c_str()));
                activationState = server::Activation::Activations::Invalid;
            }

            auto purpose = server::Version::VersionPurpose::BMC;
            auto path = fs::path(SOFTWARE_OBJPATH) / id;

            // Create functional association if this is the functional
            // version
            if (version.compare(functionalVersion) == 0)
            {
                createFunctionalAssociation(path);
            }

            AssociationList associations = {};

            if (activationState == server::Activation::Activations::Active)
            {
                // Create an association to the BMC inventory item
                associations.emplace_back(std::make_tuple(
                    ACTIVATION_FWD_ASSOCIATION, ACTIVATION_REV_ASSOCIATION,
                    bmcInventoryPath));
                // Create an active association since this image is active
                createActiveAssociation(path);
            }

            // Create Version instance for this version.
            auto versionPtr = std::make_unique<VersionClass>(
                bus, path, version, purpose, "",
                std::bind(&ItemUpdater::erase, this, std::placeholders::_1));
            auto isVersionFunctional = versionPtr->isFunctional();
            if (!isVersionFunctional)
            {
                versionPtr->deleteObject =
                    std::make_unique<phosphor::software::manager::Delete>(
                        bus, path, *versionPtr);
            }
            versions.insert(std::make_pair(id, std::move(versionPtr)));

            // Create Activation instance for this version.
            activations.insert(std::make_pair(
                id, std::make_unique<Activation>(
                        bus, path, *this, id, activationState, associations)));

            // If Active, create RedundancyPriority instance for this
            // version.
            if (activationState == server::Activation::Activations::Active)
            {
                uint8_t priority = std::numeric_limits<uint8_t>::max();
                if (!restoreFromFile(id, priority))
                {
                    if (isVersionFunctional)
                    {
                        priority = 0;
                    }
                    else
                    {
                        log<level::ERR>("Unable to restore priority from file.",
                                        entry("VERSIONID=%s", id.c_str()));
                    }
                }
                activations.find(id)->second->redundancyPriority =
                    std::make_unique<RedundancyPriority>(
                        bus, path, *(activations.find(id)->second), priority,
                        false);
            }
        }
    }

    // If there is no ubi volume for bmc version then read the /etc/os-release
    // and create rofs-<versionId> under /media
    if (activations.size() < 2)
    {
        auto version = VersionClass::getBMCVersion(OS_RELEASE_FILE);
        auto id = phosphor::software::manager::Version::getId(version);
        auto versionFileDir = BMC_ROFS_PREFIX + id + "/etc/";
        try
        {
            if (!fs::is_directory(versionFileDir))
            {
                fs::create_directories(versionFileDir);
            }
            auto versionFilePath = BMC_ROFS_PREFIX + id + OS_RELEASE_FILE;
            fs::create_directory_symlink(OS_RELEASE_FILE, versionFilePath);
            ItemUpdater::processBMCImage();
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(e.what());
        }
    }

    mirrorUbootToAlt();
    return;
}

void ItemUpdater::erase(std::string entryId)
{
    // Find entry in versions map
    auto it = versions.find(entryId);
    if (it != versions.end())
    {
        if (it->second->isFunctional() && ACTIVE_BMC_MAX_ALLOWED > 1)
        {
            log<level::ERR>("Error: Version is currently running on the BMC. "
                            "Unable to remove.",
                            entry("VERSIONID=%s", entryId.c_str()));
            return;
        }

        // Delete ReadOnly partitions if it's not active
        removeReadOnlyPartition(entryId);
        removeFile(entryId);

        // Removing entry in versions map
        this->versions.erase(entryId);
    }
    else
    {
        // Delete ReadOnly partitions even if we can't find the version
        removeReadOnlyPartition(entryId);
        removeFile(entryId);

        log<level::ERR>("Error: Failed to find version in item updater "
                        "versions map. Unable to remove.",
                        entry("VERSIONID=%s", entryId.c_str()));
    }

    helper.clearEntry(entryId);

    // Removing entry in activations map
    auto ita = activations.find(entryId);
    if (ita == activations.end())
    {
        log<level::ERR>("Error: Failed to find version in item updater "
                        "activations map. Unable to remove.",
                        entry("VERSIONID=%s", entryId.c_str()));
    }
    else
    {
        removeAssociations(ita->second->path);
        this->activations.erase(entryId);
    }
    ItemUpdater::resetUbootEnvVars();
    return;
}

void ItemUpdater::deleteAll()
{
    std::vector<std::string> deletableVersions;

    for (const auto& versionIt : versions)
    {
        if (!versionIt.second->isFunctional())
        {
            deletableVersions.push_back(versionIt.first);
        }
    }

    for (const auto& deletableIt : deletableVersions)
    {
        ItemUpdater::erase(deletableIt);
    }

    helper.cleanup();
}

ItemUpdater::ActivationStatus
    ItemUpdater::validateSquashFSImage(const std::string& filePath)
{
    bool invalid = true;

    for (auto& bmcImage : bmcImages)
    {
        fs::path file(filePath);
        file /= bmcImage;
        std::ifstream efile(file.c_str());
        if (efile.good())
        {
            invalid = false;
        }
    }

    if (invalid)
    {
        return ItemUpdater::ActivationStatus::invalid;
    }

    return ItemUpdater::ActivationStatus::ready;
}

void ItemUpdater::savePriority(const std::string& versionId, uint8_t value)
{
    storeToFile(versionId, value);
    helper.setEntry(versionId, value);
}

void ItemUpdater::freePriority(uint8_t value, const std::string& versionId)
{
    std::map<std::string, uint8_t> priorityMap;

    // Insert the requested version and priority, it may not exist yet.
    priorityMap.insert(std::make_pair(versionId, value));

    for (const auto& intf : activations)
    {
        if (intf.second->redundancyPriority)
        {
            priorityMap.insert(std::make_pair(
                intf.first, intf.second->redundancyPriority.get()->priority()));
        }
    }

    // Lambda function to compare 2 priority values, use <= to allow duplicates
    typedef std::function<bool(std::pair<std::string, uint8_t>,
                               std::pair<std::string, uint8_t>)>
        cmpPriority;
    cmpPriority cmpPriorityFunc =
        [](std::pair<std::string, uint8_t> priority1,
           std::pair<std::string, uint8_t> priority2) {
            return priority1.second <= priority2.second;
        };

    // Sort versions by ascending priority
    std::set<std::pair<std::string, uint8_t>, cmpPriority> prioritySet(
        priorityMap.begin(), priorityMap.end(), cmpPriorityFunc);

    auto freePriorityValue = value;
    for (auto& element : prioritySet)
    {
        if (element.first == versionId)
        {
            continue;
        }
        if (element.second == freePriorityValue)
        {
            ++freePriorityValue;
            auto it = activations.find(element.first);
            it->second->redundancyPriority.get()->sdbusPriority(
                freePriorityValue);
        }
    }

    auto lowestVersion = prioritySet.begin()->first;
    if (value == prioritySet.begin()->second)
    {
        lowestVersion = versionId;
    }
    updateUbootEnvVars(lowestVersion);
}

void ItemUpdater::reset()
{
    helper.factoryReset();

    log<level::INFO>("BMC factory reset will take effect upon reboot.");
}

void ItemUpdater::removeReadOnlyPartition(std::string versionId)
{
    helper.removeVersion(versionId);
}

bool ItemUpdater::fieldModeEnabled(bool value)
{
    // enabling field mode is intended to be one way: false -> true
    if (value && !control::FieldMode::fieldModeEnabled())
    {
        control::FieldMode::fieldModeEnabled(value);

        helper.enableFieldMode();
    }
    else if (!value && control::FieldMode::fieldModeEnabled())
    {
        elog<NotAllowed>(xyz::openbmc_project::Common::NotAllowed::REASON(
            "FieldMode is not allowed to be cleared"));
    }

    return control::FieldMode::fieldModeEnabled();
}

void ItemUpdater::restoreFieldModeStatus()
{
    std::system("fw_printenv > /tmp/env");
    std::ifstream input("/tmp/env");

    for (std::string envVar; getline(input, envVar); )
    {
        if (envVar.find("fieldmode=true") != std::string::npos)
        {
            ItemUpdater::fieldModeEnabled(true);
        }
    }
    std::system("rm /tmp/env");
}

void ItemUpdater::setHostInventoryPath()
{
    auto depth = 0;
    auto mapperCall = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                          MAPPER_INTERFACE, "GetSubTreePaths");

    mapperCall.append(INVENTORY_PATH);
    mapperCall.append(depth);
    std::vector<std::string> filter = {"xyz.openbmc_project.Inventory.Item.System"};
    mapperCall.append(filter);

    try
    {
        auto response = bus.call(mapperCall);

        using ObjectPaths = std::vector<std::string>;
        ObjectPaths result;
        response.read(result);

        if (!result.empty())
        {
            hostInventoryPath = result.front();
        }
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        log<level::ERR>("Error in mapper GetSubTreePath");
        return;
    }

    return;
}



void ItemUpdater::setBMCInventoryPath()
{
    auto depth = 0;
    auto mapperCall = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                          MAPPER_INTERFACE, "GetSubTreePaths");

    mapperCall.append(INVENTORY_PATH);
    mapperCall.append(depth);
    std::vector<std::string> filter = {BMC_INVENTORY_INTERFACE};
    mapperCall.append(filter);

    try
    {
        auto response = bus.call(mapperCall);

        using ObjectPaths = std::vector<std::string>;
        ObjectPaths result;
        response.read(result);

        if (!result.empty())
        {
            bmcInventoryPath = result.front();
        }
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        log<level::ERR>("Error in mapper GetSubTreePath");
        return;
    }

    return;
}

void ItemUpdater::createActiveAssociation(const std::string& path)
{
    assocs.emplace_back(
        std::make_tuple(ACTIVE_FWD_ASSOCIATION, ACTIVE_REV_ASSOCIATION, path));
    associations(assocs);
}

void ItemUpdater::createFunctionalAssociation(const std::string& path)
{
    assocs.emplace_back(std::make_tuple(FUNCTIONAL_FWD_ASSOCIATION,
                                        FUNCTIONAL_REV_ASSOCIATION, path));
    associations(assocs);
}

void ItemUpdater::removeAssociations(const std::string& path)
{
    for (auto iter = assocs.begin(); iter != assocs.end();)
    {
        if ((std::get<2>(*iter)).compare(path) == 0)
        {
            iter = assocs.erase(iter);
            associations(assocs);
        }
        else
        {
            ++iter;
        }
    }
}

bool ItemUpdater::isLowestPriority(uint8_t value)
{
    for (const auto& intf : activations)
    {
        if (intf.second->redundancyPriority)
        {
            if (intf.second->redundancyPriority.get()->priority() < value)
            {
                return false;
            }
        }
    }
    return true;
}

void ItemUpdater::updateUbootEnvVars(const std::string& versionId)
{
    helper.updateUbootVersionId(versionId);
}

void ItemUpdater::resetUbootEnvVars()
{
    decltype(activations.begin()->second->redundancyPriority.get()->priority())
        lowestPriority = std::numeric_limits<uint8_t>::max();
    decltype(activations.begin()->second->versionId) lowestPriorityVersion;
    for (const auto& intf : activations)
    {
        if (!intf.second->redundancyPriority.get())
        {
            // Skip this version if the redundancyPriority is not initialized.
            continue;
        }

        if (intf.second->redundancyPriority.get()->priority() <= lowestPriority)
        {
            lowestPriority = intf.second->redundancyPriority.get()->priority();
            lowestPriorityVersion = intf.second->versionId;
        }
    }

    // Update the U-boot environment variable to point to the lowest priority
    updateUbootEnvVars(lowestPriorityVersion);
}

void ItemUpdater::freeSpace(Activation& caller)
{
    //  Versions with the highest priority in front
    std::priority_queue<std::pair<int, std::string>,
                        std::vector<std::pair<int, std::string>>,
                        std::less<std::pair<int, std::string>>>
        versionsPQ;

    std::size_t count = 0;
    for (const auto& iter : activations)
    {
        if ((iter.second.get()->activation() ==
             server::Activation::Activations::Active) ||
            (iter.second.get()->activation() ==
             server::Activation::Activations::Failed))
        {
            count++;
            // Don't put the functional version on the queue since we can't
            // remove the "running" BMC version.
            // If ACTIVE_BMC_MAX_ALLOWED <= 1, there is only one active BMC,
            // so remove functional version as well.
            // Don't delete the the Activation object that called this function.
            if ((versions.find(iter.second->versionId)
                     ->second->isFunctional() &&
                 ACTIVE_BMC_MAX_ALLOWED > 1) ||
                (iter.second->versionId == caller.versionId) ||
                 (versions.find(iter.second->versionId)
                     ->second->purpose() == server::Version::VersionPurpose::BMC &&
                 versions.find(iter.second->versionId)
                     ->second->isFunctional()))
            {
                continue;
            }

            // Failed activations don't have priority, assign them a large value
            // for sorting purposes.
            auto priority = 999;
            if (iter.second.get()->activation() ==
                server::Activation::Activations::Active)
            {
                priority = iter.second->redundancyPriority.get()->priority();
            }

            versionsPQ.push(std::make_pair(priority, iter.second->versionId));
        }
    }

    // If the number of BMC versions is over ACTIVE_BMC_MAX_ALLOWED -1,
    // remove the highest priority one(s).
    while ((count >= ACTIVE_BMC_MAX_ALLOWED) && (!versionsPQ.empty()))
    {
        erase(versionsPQ.top().second);
        versionsPQ.pop();
        count--;
    }
}

void ItemUpdater::mirrorUbootToAlt()
{
    helper.mirrorAlt();
}

VersionPurpose ItemUpdater::getVersionPurpose(const std::string& versionId)
{
    auto iter = versions.find(versionId);
    if (iter != versions.end())
    {
        auto _purpose = iter->second->purpose();
        std::string _purpose_str = server::convertForMessage(_purpose);
        log<level::DEBUG>(("version purpose:" + _purpose_str).c_str(),
                            entry("VERSIONID=%s", versionId.c_str()));
        return _purpose;
    }
    else
    {
        log<level::ERR>("Error in get version purpose, no such id",
                        entry("VERSIONID=%s", versionId.c_str()));
    }
    return VersionPurpose::Unknown;
}

} // namespace updater
} // namespace software
} // namespace phosphor
