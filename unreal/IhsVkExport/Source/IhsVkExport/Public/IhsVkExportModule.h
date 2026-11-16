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

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

// Runtime module for the ihs-vk-export consumer. It loads at PostConfigInit so
// StartupModule can request the Vulkan external-memory/semaphore device
// extensions before UE creates its Vulkan device, then (once the RHI is up)
// probes and logs the device UUID and extension status. Later revisions add the
// wire-protocol client and the zero-copy texture import.
class FIhsVkExportModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
#if PLATFORM_LINUX
	// Runs after RHI init (FCoreDelegates::OnPostEngineInit): confirms the RHI is
	// Vulkan, logs the device UUID, and reports which import extensions came up.
	void ProbeDevice();

	FDelegateHandle PostEngineInitHandle;
#endif
};
