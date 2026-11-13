#pragma once

#ifdef FERNWEH_DEPLOYMENT_CONFIGURED
#include <fernwehdeploymentconfig.h>
#else
// Standalone component tests and unconfigured builds have no operator endpoint.
namespace FernwehDeployment {
inline constexpr char BrokerHost[] = "";
inline constexpr unsigned short BrokerPort = 29000;
inline constexpr char BrokerPins[] = "";
inline constexpr char PasskeyRpId[] = "";
inline constexpr bool UpdatesEnabled = false;
}
#endif
