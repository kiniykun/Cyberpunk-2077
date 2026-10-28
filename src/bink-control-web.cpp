#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/bink2/include/bink.h"
#include <cstddef>
// Native header layout verified against the original Win64 SDK routine.
static_assert(offsetof(BINK,frameoffsets)==0x1c8,"Bink frame-offset ABI changed");
static_assert(offsetof(BINK,OpenFlags)==0x20,"Bink flag ABI changed");
bool CpWebBinkSyncLoop(HBINK movie,S32 enabled);
void RADEXPLINK BinkSetWillLoop(HBINK movie,S32 enabled) {
    if(!movie)return;
    if(CpWebBinkSyncLoop(movie,enabled))return;
    if(enabled)movie->OpenFlags|=0x80u;
    else movie->OpenFlags&=~0x80u;
}
U32 RADEXPLINK BinkGetKeyFrame(HBINK movie,U32 frame,S32 flags) {
    if(!movie||!movie->frameoffsets||!movie->Frames)return 0;
    if(!frame)frame=1;
    if(frame>movie->Frames)return 0;
    const auto key=[movie](U32 number){return (movie->frameoffsets[number-1]&1)!=0;};
    if(!(flags&BINKGETKEYNOTEQUAL)&&key(frame))return frame;
    switch(flags&127) {
        case BINKGETKEYPREVIOUS: {
            if(frame==1)return 0;
            U32 previous=frame-1;
            while(previous>1&&!key(previous))--previous;
            return previous;
        }
        case BINKGETKEYNEXT:
            for(U32 next=frame;next<movie->Frames;) {++next;if(key(next))return next;}
            return 0;
        case BINKGETKEYCLOSEST:
            // The original checks the preceding candidate first, so ties go
            // backward. Current-frame exclusion was handled above.
            for(U32 distance=1;distance<movie->Frames;++distance) {
                if(distance<frame&&key(frame-distance))return frame-distance;
                if(distance<=movie->Frames-frame&&key(frame+distance))return frame+distance;
            }
            return 0;
        default:return 0;
    }
}
