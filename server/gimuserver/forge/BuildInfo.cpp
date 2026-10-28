// The commit forgeStatus.php reports when BF_SERVER_COMMIT is not set.
//
// Its own file, compiled without the precompiled header, because CMake gives
// it the GIMU_SERVER_COMMIT definition at configure time: a new commit then
// recompiles these few lines and nothing else.
#include "ForgeStatus.hpp"

#ifndef GIMU_SERVER_COMMIT
#define GIMU_SERVER_COMMIT "unknown"
#endif

const char* forge::buildCommit()
{
	return GIMU_SERVER_COMMIT;
}
