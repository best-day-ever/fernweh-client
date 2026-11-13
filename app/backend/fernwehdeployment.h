#pragma once

#ifdef FERNWEH_DEPLOYMENT_CONFIGURED
#include <fernwehdeploymentconfig.h>
#else
// Standalone component tests and unconfigured builds have no operator endpoint.
#ifndef FERNWEH_BROKER_ENABLED
#define FERNWEH_BROKER_ENABLED 0
#endif
namespace FernwehDeployment {
inline constexpr bool BrokerEnabled = FERNWEH_BROKER_ENABLED != 0;
inline constexpr char BrokerHost[] = "";
inline constexpr unsigned short BrokerPort = 29000;
inline constexpr char BrokerPins[] = "";
inline constexpr char PasskeyRpId[] = "";
inline constexpr bool UpdatesEnabled = false;
}
#endif
