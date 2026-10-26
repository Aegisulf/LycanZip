#include "pch.h"

// XamlTypeInfo.g.cpp contains exports for the VS designer that refer to these two functions. Normally module.g.cpp provides them,
// but module.g.cpp asks for the activation factories of App / XamlMetaDataProvider (meaningless for an exe, and not compatible with the projected types).
// This is an executable and offers no activation factories, so it reports "no classes".
int32_t __stdcall WINRT_CanUnloadNow() noexcept
{
    return 1;  // S_FALSE
}

int32_t __stdcall WINRT_GetActivationFactory(void*, void** factory) noexcept
{
    *factory = nullptr;
    return static_cast<int32_t>(0x80040111);  // CLASS_E_CLASSNOTAVAILABLE
}
