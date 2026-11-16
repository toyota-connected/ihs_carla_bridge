/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "IhsVkExportModule.h"

#include "Misc/CoreDelegates.h"

#if PLATFORM_LINUX
#include "IVulkanDynamicRHI.h"
#include "RHI.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogIhsVkExport, Log, All);

void FIhsVkExportModule::StartupModule()
{
#if PLATFORM_LINUX
	// Enable the external-memory / semaphore device extensions the zero-copy
	// import needs. This is a static hook that records the request; UE applies it
	// when it creates the Vulkan device during RHI init. It must therefore run
	// BEFORE that -- the module loads at PostConfigInit (see IhsVkExport.uplugin).
	static const ANSICHAR* const DeviceExtensions[] =
	{
		"VK_KHR_external_memory",
		"VK_KHR_external_memory_fd",
		"VK_KHR_external_semaphore",
		"VK_KHR_external_semaphore_fd",
		"VK_EXT_external_memory_dma_buf",
		"VK_EXT_image_drm_format_modifier",
		"VK_EXT_queue_family_foreign",
	};
	IVulkanDynamicRHI::AddEnabledDeviceExtensionsAndLayers(
		MakeArrayView(DeviceExtensions),
		TArrayView<const ANSICHAR* const>());

	UE_LOG(LogIhsVkExport, Log,
		TEXT("[ihs-vk-export] requested %d Vulkan device extensions for zero-copy import"),
		(int32)UE_ARRAY_COUNT(DeviceExtensions));

	PostEngineInitHandle =
		FCoreDelegates::OnPostEngineInit.AddRaw(this, &FIhsVkExportModule::ProbeDevice);
#endif
}

void FIhsVkExportModule::ShutdownModule()
{
#if PLATFORM_LINUX
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
#endif
}

#if PLATFORM_LINUX
void FIhsVkExportModule::ProbeDevice()
{
	if (GDynamicRHI == nullptr ||
		GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::Vulkan)
	{
		UE_LOG(LogIhsVkExport, Warning,
			TEXT("[ihs-vk-export] active RHI is not Vulkan; zero-copy import unavailable"));
		return;
	}

	IVulkanDynamicRHI* RHI = GetIVulkanDynamicRHI();

	// The device UUID must match the ivi-homescreen shell's exporting GPU for a
	// same-device dma-buf/OPAQUE_FD import; log it so the shell can be pinned to
	// it via IVI_VK_DEVICE_UUID.
	const uint8* Uuid = RHI->RHIGetVulkanDeviceUUID();
	FString UuidHex;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		UuidHex += FString::Printf(TEXT("%02x"), Uuid[Index]);
	}
	UE_LOG(LogIhsVkExport, Log, TEXT("[ihs-vk-export] Vulkan device UUID: %s"), *UuidHex);
	UE_LOG(LogIhsVkExport, Log,
		TEXT("[ihs-vk-export] pin the shell to this GPU with IVI_VK_DEVICE_UUID=%s"),
		*UuidHex);

	// Confirm the import extensions actually came up on the device (UE filters
	// requested extensions against what the physical device supports).
	static const ANSICHAR* const Required[] =
	{
		"VK_KHR_external_memory_fd",
		"VK_KHR_external_semaphore_fd",
		"VK_EXT_external_memory_dma_buf",
		"VK_EXT_image_drm_format_modifier",
		"VK_EXT_queue_family_foreign",
	};
	const TArray<VkExtensionProperties> Available =
		RHI->RHIGetAllDeviceExtensions(RHI->RHIGetVkPhysicalDevice());
	for (const ANSICHAR* Name : Required)
	{
		bool bFound = false;
		for (const VkExtensionProperties& Ext : Available)
		{
			if (FCStringAnsi::Strcmp(Ext.extensionName, Name) == 0)
			{
				bFound = true;
				break;
			}
		}
		UE_LOG(LogIhsVkExport, Log, TEXT("[ihs-vk-export]   extension %s: %s"),
			ANSI_TO_TCHAR(Name), bFound ? TEXT("ENABLED") : TEXT("MISSING"));
	}
}
#endif  // PLATFORM_LINUX

IMPLEMENT_MODULE(FIhsVkExportModule, IhsVkExport)
