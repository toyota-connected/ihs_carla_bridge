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

using UnrealBuildTool;

public class IhsVkExport : ModuleRules
{
	public IhsVkExport(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"RHI",
			"RenderCore",
		});

		// The consumer reaches UE's Vulkan device through IVulkanDynamicRHI and
		// imports external memory/semaphores with raw Vulkan calls, so it needs
		// the VulkanRHI module and the Vulkan SDK headers. Linux-only (the plugin
		// is gated to Linux in the .uplugin); guard here too so a non-Vulkan
		// target never tries to pull these in.
		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			PrivateDependencyModuleNames.Add("VulkanRHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "Vulkan");
		}
	}
}
