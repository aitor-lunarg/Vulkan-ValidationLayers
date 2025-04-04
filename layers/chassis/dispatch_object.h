/***************************************************************************
 *
 * Copyright (c) 2015-2025 The Khronos Group Inc.
 * Copyright (c) 2015-2025 Valve Corporation
 * Copyright (c) 2015-2025 LunarG, Inc.
 * Copyright (c) 2015-2024 Google Inc.
 * Copyright (c) 2023-2024 RasterGrid Kft.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

#pragma once
#include <atomic>

#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/utility/vk_safe_struct.hpp>

#include "containers/custom_containers.h"
#include "error_message/logging.h"
#include "utils/vk_layer_utils.h"
#include "layer_options.h"
#include "gpuav/core/gpuav_settings.h"
#include "sync/sync_settings.h"
#include "generated/dispatch_vector.h"
#include "generated/vk_api_version.h"
#include "generated/vk_extension_helper.h"
#include "generated/vk_layer_dispatch_table.h"

class ValidationObject;

// Layer object type identifiers
enum LayerObjectTypeId {
    LayerObjectTypeThreading,            // Instance or device threading layer object
    LayerObjectTypeParameterValidation,  // Instance or device parameter validation layer object
    LayerObjectTypeObjectTracker,        // Instance or device object tracker layer object
    LayerObjectTypeCoreValidation,       // Instance or device core validation layer object
    LayerObjectTypeBestPractices,        // Instance or device best practices layer object
    LayerObjectTypeGpuAssisted,          // Instance or device gpu assisted validation layer object
    LayerObjectTypeSyncValidation,       // Instance or device synchronization validation layer object
    LayerObjectTypeMaxEnum,              // Max enum count
};

// To avoid re-hashing unique ids on each use, we precompute the hash and store the
// hash's LSBs in the high 24 bits.
struct HashedUint64 {
    static const int HASHED_UINT64_SHIFT = 40;
    size_t operator()(const uint64_t& t) const { return t >> HASHED_UINT64_SHIFT; }

    static uint64_t hash(uint64_t id) {
        uint64_t h = (uint64_t)vvl::hash<uint64_t>()(id);
        id |= h << HASHED_UINT64_SHIFT;
        return id;
    }
};

namespace vvl {
namespace dispatch {

class Instance;
void SetData(VkInstance instance, std::unique_ptr<Instance>&&);
Instance* GetData(VkInstance);
Instance* GetData(VkPhysicalDevice);
void FreeData(void* key, VkInstance instance);

class Device;
void SetData(VkDevice dev, std::unique_ptr<Device>&&);
Device* GetData(VkDevice);
Device* GetData(VkQueue);
Device* GetData(VkCommandBuffer);
void FreeData(void* key, VkDevice device);

void FreeAllData();

struct TemplateState {
    VkDescriptorUpdateTemplate desc_update_template;
    vku::safe_VkDescriptorUpdateTemplateCreateInfo create_info;
    bool destroyed;

    TemplateState(VkDescriptorUpdateTemplate update_template, vku::safe_VkDescriptorUpdateTemplateCreateInfo* pCreateInfo)
        : desc_update_template(update_template), create_info(*pCreateInfo), destroyed(false) {}
};

struct Settings {
    GlobalSettings global_settings = {};
    GpuAVSettings gpuav_settings = {};
    SyncValSettings syncval_settings = {};

    CHECK_DISABLED disabled = {};
    CHECK_ENABLED enabled = {};
};

class HandleWrapper : public Logger {
  public:
    HandleWrapper(DebugReport* dr);
    ~HandleWrapper();

    // Unwrap a handle.
    template <typename HandleType>
    HandleType Unwrap(HandleType wrapped_handle) {
        if (wrapped_handle == (HandleType)VK_NULL_HANDLE) return wrapped_handle;
        auto iter = unique_id_mapping.find(CastToUint64(wrapped_handle));
        if (iter == unique_id_mapping.end()) return (HandleType)0;
        return (HandleType)iter->second;
    }

    // Wrap a newly created handle with a new unique ID, and return the new ID.
    template <typename HandleType>
    HandleType WrapNew(HandleType new_created_handle) {
        if (new_created_handle == (HandleType)VK_NULL_HANDLE) return new_created_handle;
        auto unique_id = global_unique_id++;
        unique_id = HashedUint64::hash(unique_id);
        assert(unique_id != 0);  // can't be 0, otherwise unwrap will apply special rule for VK_NULL_HANDLE
        unique_id_mapping.insert_or_assign(unique_id, CastToUint64(new_created_handle));
        return (HandleType)unique_id;
    }

    template <typename HandleType>
    HandleType Find(HandleType wrapped_handle) const {
        uint64_t id = CastToUint64(wrapped_handle);
        auto iter = unique_id_mapping.find(id);
        if (iter != unique_id_mapping.end()) {
            return CastFromUint64<HandleType>(iter->second);
        } else {
            return CastFromUint<HandleType>(0ULL);
        }
    }

    template <typename HandleType>
    HandleType Erase(HandleType wrapped_handle) {
        uint64_t id = CastToUint64(wrapped_handle);
        auto iter = unique_id_mapping.pop(id);
        if (iter != unique_id_mapping.end()) {
            return CastFromUint64<HandleType>(iter->second);
        } else {
            return CastFromUint<HandleType>(0ULL);
        }
    }

    void UnwrapPnextChainHandles(const void* pNext);

    static std::atomic<uint64_t> global_unique_id;
    static vvl::concurrent_unordered_map<uint64_t, uint64_t, 4, HashedUint64> unique_id_mapping;
    static bool wrap_handles;
};

class Instance : public HandleWrapper {
  public:
    Instance(const VkInstanceCreateInfo* pCreateInfo);
    ~Instance();

    void InitValidationObjects();

    // VkDisplayKHR objects are statically created in the driver at VkCreateInstance.
    // They live with the PhyiscalDevice and apps never created/destroy them.
    // Apps needs will query for them and the first time we see it we wrap it
    VkDisplayKHR MaybeWrapDisplay(VkDisplayKHR handle) {
        // See if this display is already known
        auto it = display_id_reverse_mapping.find(handle);
        if (it != display_id_reverse_mapping.end()) return (VkDisplayKHR)it->second;

        // First time see this VkDisplayKHR, so wrap
        const uint64_t unique_id = (uint64_t)WrapNew(handle);
        display_id_reverse_mapping.insert_or_assign(handle, unique_id);
        return (VkDisplayKHR)unique_id;
    }
    ValidationObject* GetValidationObject(LayerObjectTypeId object_type) const;

    Settings settings;

    APIVersion api_version;
    DeviceExtensions extensions{};

    mutable std::vector<std::unique_ptr<ValidationObject>> object_dispatch;

    VkInstance instance = VK_NULL_HANDLE;
    VkLayerInstanceDispatchTable instance_dispatch_table;
    // Reverse map display handles
    vvl::concurrent_unordered_map<VkDisplayKHR, uint64_t, 0> display_id_reverse_mapping;

#include "generated/dispatch_object_instance_methods.h"
};

class Device : public HandleWrapper {
  public:
    Device(Instance* instance, VkPhysicalDevice gpu, const VkDeviceCreateInfo* pCreateInfo);
    ~Device();

    void InitObjectDispatchVectors();
    void InitValidationObjects();
    void ReleaseValidationObject(LayerObjectTypeId type_id) const;
    ValidationObject* GetValidationObject(LayerObjectTypeId object_type) const;

    bool IsSecondary(VkCommandBuffer cb) const;

    Settings& settings;
    Instance* dispatch_instance;

    APIVersion api_version;
    DeviceExtensions extensions = {};

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkLayerDispatchTable device_dispatch_table;

    mutable std::vector<std::unique_ptr<ValidationObject>> object_dispatch;
    mutable std::vector<std::unique_ptr<ValidationObject>> aborted_object_dispatch;
    mutable std::vector<std::vector<ValidationObject*>> intercept_vectors;
    // Handle Wrapping Data
    // Wrapping Descriptor Template Update structures requires access to the template createinfo structs
    vvl::unordered_map<uint64_t, std::unique_ptr<TemplateState>> desc_template_createinfo_map;
    struct SubpassesUsageStates {
        vvl::unordered_set<uint32_t> subpasses_using_color_attachment;
        vvl::unordered_set<uint32_t> subpasses_using_depthstencil_attachment;
    };
    // Uses unwrapped handles
    vvl::unordered_map<VkRenderPass, SubpassesUsageStates> renderpasses_states;
    // Map of wrapped swapchain handles to arrays of wrapped swapchain image IDs
    // Each swapchain has an immutable list of wrapped swapchain image IDs -- always return these IDs if they exist
    vvl::unordered_map<VkSwapchainKHR, std::vector<VkImage>> swapchain_wrapped_image_handle_map;
    // Map of wrapped descriptor pools to set of wrapped descriptor sets allocated from each pool
    vvl::unordered_map<VkDescriptorPool, vvl::unordered_set<VkDescriptorSet>> pool_descriptor_sets_map;

    vvl::concurrent_unordered_map<VkDeferredOperationKHR, std::vector<std::function<void()>>, 0> deferred_operation_post_completion;
    vvl::concurrent_unordered_map<VkDeferredOperationKHR, std::vector<std::function<void(const std::vector<VkPipeline>&)>>, 0>
        deferred_operation_post_check;
    vvl::concurrent_unordered_map<VkDeferredOperationKHR, std::vector<VkPipeline>, 0> deferred_operation_pipelines;

    // State we track in order to populate HandleData for things such as ignored pointers
    vvl::unordered_map<VkCommandBuffer, VkCommandPool> secondary_cb_map{};
    mutable std::shared_mutex secondary_cb_map_mutex;

#include "generated/dispatch_object_device_methods.h"
};
}  // namespace dispatch
}  // namespace vvl
