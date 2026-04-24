#ifndef __C2_AGENT_H__
#define __C2_AGENT_H__

#include <Arduino.h>

void startC2AgentTask();
bool isC2AgentRunning();
String c2AgentDeviceId();
String c2AgentStatus();
String c2AgentLastResult();
uint32_t c2AgentLastSeenMs();

#endif
