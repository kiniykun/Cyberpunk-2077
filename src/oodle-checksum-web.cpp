// Recovered from the original Oodle7 quantum-checksum/hash routine and checked
// against its CPU-emulated reference. See MODLOG.md and external analysis files.
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace {
uint32_t Rotate(uint32_t value,unsigned amount) {return (value<<amount)|(value>>(32-amount));}
void Mix(uint32_t& a,uint32_t& b,uint32_t& c) {
    a-=c;a^=Rotate(c,4);c+=b;
    b-=a;b^=Rotate(a,6);a+=c;
    c-=b;c^=Rotate(b,8);b+=a;
    a-=c;a^=Rotate(c,16);c+=b;
    b-=a;b^=Rotate(a,19);a+=c;
    c-=b;c^=Rotate(b,4);b+=a;
}
uint64_t Final(uint32_t a,uint32_t b,uint32_t c) {
    c^=b;c-=Rotate(b,14);
    a^=c;a-=Rotate(c,11);
    b^=a;b-=Rotate(a,25);
    c^=b;c-=Rotate(b,16);
    a^=c;a-=Rotate(c,4);
    b^=a;b-=Rotate(a,14);
    c^=b;c-=Rotate(b,24);
    return (uint64_t(b)<<32)|c;
}
uint32_t ReadBigEndian(const uint8_t* p) {return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
void Round(const uint8_t* data,uint32_t* a,uint32_t* b,uint32_t* c) {
    for(unsigned i=0;i<4;++i) {
        a[i]+=ReadBigEndian(data+i*4);
        b[i]+=ReadBigEndian(data+16+i*4);
        c[i]+=ReadBigEndian(data+32+i*4);
        Mix(a[i],b[i],c[i]);
    }
}
}
extern "C" uint64_t CpWebOodleHash64(const uint8_t* data,size_t length) {
    uint32_t a[4],b[4],c[4];
    for(unsigned i=0;i<4;++i) {a[i]=3735928559u+uint32_t(length);b[i]=544179635u;c[i]=1466479909u-uint32_t(length);}
    size_t remaining=length;
    while(remaining>=48) {Round(data,a,b,c);data+=48;remaining-=48;}
    if(remaining) {uint8_t tail[48]={};std::memcpy(tail,data,remaining);Round(tail,a,b,c);}
    uint32_t x=a[0],y=a[1],z=a[2];
    Mix(x,y,z);
    x+=a[3];y+=b[0];z+=b[1];Mix(x,y,z);
    x+=b[2];y+=b[3];z+=c[0];Mix(x,y,z);
    return Final(x+c[1],y+c[2],z+c[3]);
}
extern "C" uint32_t CpWebOodleQuantumChecksum(const uint8_t* data,size_t length) {return uint32_t(CpWebOodleHash64(data,length))&0xffffffu;}
