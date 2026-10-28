// Browser entry into the original cpLauncher, not a replacement game.
#include "build.h"
#include "mainRed.h"
#include "../../../common/redCore/include/commandline.h"

int main( int argc, char** argv )
{
    red::CommandLine::Init( argc, argv );
    // Browser Run schedules the state machine and returns without quick_exit.
    return mainRed();
}
