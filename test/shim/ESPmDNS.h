#pragma once
#include "Arduino.h"
struct FakeMDNS { bool begin(const char*){return true;} void addService(const char*,const char*,int){} };
extern FakeMDNS MDNS;
