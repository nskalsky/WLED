#pragma once
#include <Arduino.h>
#include <vector>

class AsyncWebServer;

typedef void (*SAF_CollectIcaosFn)(void* ctx, std::vector<String>& outUniqueUpper);
typedef void (*SAF_ApplyCategoryFn)(void* ctx, const String& icaoUpper, const String& catStr, uint32_t ts);

// Call from SkyAwareUsermod::setup()
void MetarFetcher_begin(AsyncWebServer& server,
                        void* ctx,
                        SAF_CollectIcaosFn collectCb,
                        SAF_ApplyCategoryFn applyCb);

// Call from SkyAwareUsermod::loop()
void MetarFetcher_tick(void* ctx);
