// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS);
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "android/cpu/cpu_brand.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
// IWYU pragma: end_keep
// clang-format on
#include <windows.h>
#include <winioctl.h>
// IWYU pragma: end_keep
// clang-format on
#include <Wbemidl.h>
#include <comdef.h>

#include "android/base/scoped_com_initializer.h"

#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <cstring>
#ifndef _MSC_VER
#include <unistd.h>
#endif
#endif

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif  // __APPLE__

#include <cstdio>
#include <string>

#include "absl/log/log.h"
#include "absl/strings/str_format.h"

#include "x86_cpuid.h"

namespace android::cpu {

namespace {
void trimWhitespace(char* name) {
    char *p, *q, *s;

    p = q = s = name;

    while (*p == ' ') p++;

    while (*p) {
        if (*p != ' ') s = q;

        *q++ = *p++;
    }

    *(s + 1) = '\0';
}

#ifdef _WIN32
int getCpuBrandNameAndCoreCountWMI(char* name, uint32_t* core_count, uint32_t* lp_count) {
    HRESULT hres;
    IWbemLocator* pLoc = NULL;
    IWbemServices* pSvc = NULL;
    IEnumWbemClassObject* pEnumerator = NULL;
    IWbemClassObject* pclsObj = NULL;
    ULONG uReturn = 0;
    HRESULT hr;
    VARIANT vtProp;
    // uint32_t numCores = 0;
    // uint32_t numLogicalProcessors = 0;
    char* cpuName;

    const android::base::ScopedComInitializer com_init;
    if (!com_init) {
        LOG(ERROR) << absl::StrFormat(
                "Error in %s: Failed to initialize COM library."
                " Error code = 0x%x",
                __func__, com_init.hr());
        return 1;
    }

    hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                            (LPVOID*)&pLoc);
    if (FAILED(hres)) {
        LOG(ERROR) << absl::StrFormat(
                "Error in %s: Failed to create IWbemLocator object."
                " Error code = 0x%x",
                __func__, hres);
        return 1;
    }

    hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
    if (FAILED(hres)) {
        LOG(ERROR) << absl::StrFormat(
                "Error in %s: Could not connect."
                " Error code = 0x%x",
                __func__, hres);
        pLoc->Release();
        return 1;
    }

    hres = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                             RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hres)) {
        LOG(ERROR) << absl::StrFormat(
                "Error in %s: Could not set proxy blanket."
                " Error code = 0x%x",
                __func__, hres);
        pSvc->Release();
        pLoc->Release();
        return 1;
    }

    hres = pSvc->ExecQuery(bstr_t("WQL"), bstr_t("SELECT * FROM Win32_Processor"),
                           WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL,
                           &pEnumerator);
    if (FAILED(hres)) {
        LOG(ERROR) << absl::StrFormat(
                "Error in %s: Query for Win32_Processor failed."
                " Error code = 0x%x",
                __func__, hres);
        pSvc->Release();
        pLoc->Release();
        return 1;
    }

    while (pEnumerator) {
        (void)pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
        if (!uReturn) break;

        VariantInit(&vtProp);
        /*(void)pclsObj->Get(L"NumberOfCores", 0, &vtProp, 0, 0);
        numCores += vtProp.lVal;
        VariantClear(&vtProp);

        (void)pclsObj->Get(L"NumberOfLogicalProcessors", 0, &vtProp, 0, 0);
        numLogicalProcessors += vtProp.lVal;
        VariantClear(&vtProp);*/

        (void)pclsObj->Get(L"Name", 0, &vtProp, 0, 0);
        cpuName = _com_util::ConvertBSTRToString(vtProp.bstrVal);
        trimWhitespace(cpuName);
        strcpy(name, (char*)cpuName);
        free(cpuName);
        VariantClear(&vtProp);

        pclsObj->Release();
    }

    pSvc->Release();
    pLoc->Release();
    pEnumerator->Release();

    return 0;
}
#endif

#if defined(__APPLE__) && defined(__MACH__)
int getCpuBrandNameSysctl(char* name) {
    size_t buf_len;

    if (sysctlbyname("machdep.cpu.brand_string", NULL, &buf_len, NULL, 0) != 0) {
        LOG(ERROR) << absl::StrFormat("Error in %s: sysctlbyname failed. ", __func__);
        return 1;
    }

    std::string value(buf_len - 1, '\0');
    if (sysctlbyname("machdep.cpu.brand_string", &value[0], &buf_len, nullptr, 0) != 0) {
        LOG(ERROR) << absl::StrFormat("Error in %s: sysctlbyname failed. ", __func__);
        return 1;
    }

    strcpy(name, value.c_str());
    trimWhitespace(name);
    return 0;
}
#endif

#if defined(__linux__)
int getCpuBrandNameProcfs(char* name) {
    char line[200];
    char* model_name = NULL;
    int ret = 1;

    FILE* const cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo == NULL) {
        LOG(ERROR) << absl::StrFormat("Error in %s: could not open /proc/cpuinfo.", __func__);
        return 1;
    }

#if defined(__x86_64__)
    while (fgets(line, sizeof(line), cpuinfo)) {
        if (!strncmp(line, "model name", 9)) {
            if ((model_name = strstr(line, ": "))) {
                model_name += 2;
                *strstr(model_name, "\n") = 0;
                ret = 0;
                break;
            }
        }
    }
    if (!model_name) {
        LOG(ERROR) << absl::StrFormat(
                "Error in %s: could not find \"model name\" "
                "in /proc/cpuinfo.",
                __func__);
        return 1;
    }

    trimWhitespace(model_name);
    strcpy(name, model_name);
#elif defined(__aarch64__)
    struct id_part {
        int id;
        const char* name;
    };

    static const struct id_part arm_part[] = {
        {0x00f, "ARM11MPCore"},  {0xd03, "Cortex-A53"},
        {0xd04, "Cortex-A35"},   {0xd05, "Cortex-A55"},
        {0xd07, "Cortex-A57"},   {0xd08, "Cortex-A72"},
        {0xd09, "Cortex-A73"},   {0xd0a, "Cortex-A75"},
        {0xd0b, "Cortex-A76"},   {0xd0c, "Neoverse-N1"},
        {0xd0d, "Cortex-A77"},   {0xd0e, "Cortex-A76AE"},
        {0xd40, "Neoverse-V1"},  {0xd41, "Cortex-A78"},
        {0xd42, "Cortex-A78AE"}, {0xd44, "Cortex-X1"},
        {0xd46, "Cortex-A510"},  {0xd47, "Cortex-A710"},
        {0xd48, "Cortex-X2"},    {0xd49, "Neoverse-N2"},
        {0xd4b, "Cortex-A78C"},  {0xd4c, "Cortex-X1C"},
        {0xd4d, "Cortex-A715"},  {0xd4e, "Cortex-X3"},
        {0xd4f, "Neoverse-V2"},  {-1, NULL},
    };

    static const struct id_part qcom_part[] = {
        {0x800, "Falkor-V1/Kryo"},
        {0x801, "Kryo-V2"},
        {0x802, "Kryo-3XX-Gold"},
        {0x803, "Kryo-3XX-Silver"},
        {0x804, "Kryo-4XX-Gold"},
        {0x805, "Kryo-4XX-Silver"},
        {-1, NULL},
    };

    static const struct id_part apple_part[] = {
        {0x022, "IceStorm"}, {0x023, "FireStorm"}, {0x024, "Blizzard"}, {0x025, "Avalanche"},
        {0x028, "Sawtooth"}, {0x029, "Everest"},   {-1, NULL},
    };

    static const struct {
        int id;
        const struct id_part* parts;
        const char* name;
    } hw_implementer[] = {
        {0x41, arm_part, "ARM"},       {0x42, NULL, "Broadcom"}, {0x43, NULL, "Cavium"},
        {0x44, NULL, "DEC"},           {0x4e, NULL, "NVIDIA"},   {0x50, NULL, "APM"},
        {0x51, qcom_part, "Qualcomm"}, {0x53, NULL, "Samsung"},  {0x61, apple_part, "Apple"},
        {0xc0, NULL, "Ampere"},        {-1, NULL, NULL},
    };

    char* cpu_impl = NULL;
    char* cpu_part = NULL;
    uint32_t cpu_impl_id = 0;
    uint32_t cpu_part_id = 0;
    char* cpu_impl_name = NULL;
    char* cpu_part_name = NULL;
    int j;
    const struct id_part* parts = NULL;

    while (fgets(line, sizeof(line), cpuinfo)) {
        if (!strncmp(line, "CPU implementer", 15)) {
            if ((cpu_impl = strstr(line, ": "))) {
                cpu_impl += 2;
                *strstr(cpu_impl, "\n") = 0;
                cpu_impl_id = strtol(cpu_impl, NULL, 0);
                continue;
            }
        }
        if (!strncmp(line, "CPU part", 8)) {
            if ((cpu_part = strstr(line, ": "))) {
                cpu_part += 2;
                *strstr(cpu_part, "\n") = 0;
                cpu_part_id = strtol(cpu_part, NULL, 0);
                continue;
            }
        }
        if (cpu_impl_id && cpu_part_id) break;
    }

    if (!cpu_impl_id || !cpu_part_id) {
        if (!cpu_impl_id) {
            LOG(ERROR) << absl::StrFormat(
                    "Error in %s: could not find \"CPU implementor\" "
                    "in /proc/cpuinfo.",
                    __func__);
        }
        if (!cpu_part_id) {
            LOG(ERROR) << absl::StrFormat(
                    "Error in %s: could not find \"CPU part\" "
                    "in /proc/cpuinfo.",
                    __func__);
        }
        goto out;
    }

    /* decode vendor */
    for (j = 0; hw_implementer[j].id != -1; j++) {
        if (hw_implementer[j].id == cpu_impl_id) {
            parts = hw_implementer[j].parts;
            cpu_impl_name = strdup(hw_implementer[j].name);
            break;
        }
    }

    /* decode model */
    if (parts) {
        for (j = 0; parts[j].id != -1; j++) {
            if (parts[j].id == cpu_part_id) {
                cpu_part_name = strdup(parts[j].name);
                break;
            }
        }
    }

    if (!cpu_impl_name) {
        cpu_impl_name = (char*)malloc(17);
        if (!cpu_impl_name) {
            LOG(ERROR) << absl::StrFormat("Error in %s: memory allocation failed.", __func__);
            goto out;
        }
        memset(cpu_impl_name, 0, 17);
        snprintf(cpu_impl_name, 17, "Implementer_0x%02x", cpu_impl_id);
    }
    if (!cpu_part_name) {
        cpu_part_name = (char*)malloc(11);
        if (!cpu_part_name) {
            LOG(ERROR) << absl::StrFormat("Error in %s: memory allocation failed.", __func__);
            free(cpu_impl_name);
            goto out;
        }
        memset(cpu_part_name, 0, 11);
        snprintf(cpu_part_name, 11, "Part_0x%03x", cpu_part_id);
    }

    snprintf(name, strlen(cpu_impl_name) + strlen(cpu_part_name) + 2, "%s %s", cpu_impl_name,
             cpu_part_name);
    free(cpu_impl_name);
    free(cpu_part_name);
    ret = 0;
out:
#endif

    fclose(cpuinfo);
    return ret;
}
#endif

#if defined(__x86_64__)
int getCpuBrandNameCpuid(char* name) {
    char cpuName[49] = {0};
    uint32_t eax = 0;

    android_get_x86_cpuid(0x80000000, 0, &eax, NULL, NULL, NULL);

    if (!(eax & 0x80000000 && eax >= 0x80000004)) {
        LOG(ERROR) << absl::StrFormat(
                "Error in %s: CPU does not support fetching "
                "brand name using CPUID.",
                __func__);
        return 1;
    }

    android_get_x86_cpuid(0x80000002, 0, (uint32_t*)&cpuName[0], (uint32_t*)&cpuName[4],
                          (uint32_t*)&cpuName[8], (uint32_t*)&cpuName[12]);
    android_get_x86_cpuid(0x80000003, 0, (uint32_t*)&cpuName[16], (uint32_t*)&cpuName[20],
                          (uint32_t*)&cpuName[24], (uint32_t*)&cpuName[28]);
    android_get_x86_cpuid(0x80000004, 0, (uint32_t*)&cpuName[32], (uint32_t*)&cpuName[36],
                          (uint32_t*)&cpuName[40], (uint32_t*)&cpuName[44]);

    trimWhitespace(cpuName);
    strcpy(name, (char*)cpuName);
    return 0;
}
#endif

}  // namespace

int GetCpuBrandName(char* name) {
#if defined(__x86_64__)
    return getCpuBrandNameCpuid(name);

#elif defined(__aarch64__) || defined(_M_ARM64)

#ifdef _WIN32
    uint32_t core_count, lp_count;
    return getCpuBrandNameAndCoreCountWMI(name, &core_count, &lp_count);
#elif defined(__APPLE__) && defined(__MACH__)
    return getCpuBrandNameSysctl(name);
#elif defined(__linux__)
    return getCpuBrandNameProcfs(name);
#else
#error "Unsupported platform!"
#endif  //_WIN32

#else
#error "Unsupported platform!"
#endif  // defined(__x86_64__)
}

}  // namespace android::cpu