#pragma once
#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/oodle/include/oodle2.h"
struct CpWebOodleWorkspace {
    void* data=nullptr;
    t_fp_OodleCore_Plugin_Free* release=nullptr;
    CpWebOodleWorkspace()=default;
    CpWebOodleWorkspace(const CpWebOodleWorkspace&)=delete;
    CpWebOodleWorkspace& operator=(const CpWebOodleWorkspace&)=delete;
    ~CpWebOodleWorkspace() {if(data&&release)release(data);}
};
bool CpWebOodleAllocateWorkspace(SINTa bytes,CpWebOodleWorkspace& workspace);
void CpWebOodleReportAssertion(const char* file,int line,const char* function,const char* message);
