#include "utf16-runtime.h"
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <limits>
#include <emscripten/heap.h>

namespace cpweb {
// Preserve Windows narrow-string and integer length modifiers at the libc ABI.
int vsnprintf_ansi(char* destination,size_t capacity,const char* format,va_list args) {
    if(!format){errno=EINVAL;return -1;}
    const size_t length=std::strlen(format);
    if(length>1024*1024){errno=EOVERFLOW;return -1;}
    char local[512];char* translated=length<sizeof(local)?local:static_cast<char*>(emscripten_builtin_malloc(length+1));
    if(!translated){errno=ENOMEM;return -1;}
    const char* p=format;char* out=translated;
    while(*p){
        *out++=*p++;if(out[-1]!='%')continue;
        if(*p=='%'){*out++=*p++;continue;}
        while(*p && (std::strchr("-+ #0'.*$",*p)||(*p>='0'&&*p<='9'))) *out++=*p++;
        const char* modifier=p;
        if(*p=='h'||*p=='l'){char c=*p++;if(*p==c)++p;}
        else if(*p=='I'){++p;if((p[0]=='6'&&p[1]=='4')||(p[0]=='3'&&p[1]=='2'))p+=2;}
        else if(*p=='j'||*p=='z'||*p=='t'||*p=='L')++p;
        const size_t n=size_t(p-modifier);
        if(n==1 && modifier[0]=='h' && (*p=='s'||*p=='c')){}
        else if(n==3 && modifier[0]=='I' && modifier[1]=='6'){*out++='l';*out++='l';}
        else if(n==3 && modifier[0]=='I' && modifier[1]=='3'){}
        else if(n==1 && modifier[0]=='I')*out++='z';
        else {std::memcpy(out,modifier,n);out+=n;}
        if(*p)*out++=*p++;
    }
    *out=0;
    int result=std::vsnprintf(destination,capacity,translated,args);
    if(translated!=local)emscripten_builtin_free(translated);
    return result;
}
size_t wcstombs(char* destination, const wchar_t* source, size_t capacity) {
    return wcsrtombs(destination, &source, capacity, nullptr);
}
size_t mbstowcs(wchar_t* destination, const char* source, size_t capacity) {
    return mbsrtowcs(destination, &source, capacity, nullptr);
}
size_t wcslen(const wchar_t* s) { const auto* p=s; while(*p) ++p; return p-s; }
size_t wcsnlen(const wchar_t* s,size_t n) { size_t i=0; while(i<n && s[i]) ++i; return i; }
wchar_t* wcsncpy(wchar_t* d,const wchar_t* s,size_t n) { size_t i=0; for(;i<n && s[i];++i) d[i]=s[i]; for(;i<n;++i)d[i]=0; return d; }
wchar_t* wcsncat(wchar_t* d,const wchar_t* s,size_t n) { size_t at=wcslen(d),i=0; for(;i<n && s[i];++i)d[at+i]=s[i]; d[at+i]=0; return d; }
int wcscmp(const wchar_t* a,const wchar_t* b) { while(*a && *a==*b){++a;++b;} return (*a>*b)-(*a<*b); }
int wcsncmp(const wchar_t* a,const wchar_t* b,size_t n) { for(size_t i=0;i<n;++i){if(a[i]!=b[i])return (a[i]>b[i])-(a[i]<b[i]); if(!a[i])break;} return 0; }
const wchar_t* wcschr(const wchar_t* s,wchar_t c) { do{if(*s==c)return s;}while(*s++); return nullptr; }
wchar_t* wcschr(wchar_t* s,wchar_t c) { return const_cast<wchar_t*>(wcschr(static_cast<const wchar_t*>(s),c)); }
const wchar_t* wcsrchr(const wchar_t* s,wchar_t c) { const wchar_t* result=nullptr; do{if(*s==c)result=s;}while(*s++);return result; }
wchar_t* wcsrchr(wchar_t* s,wchar_t c) { return const_cast<wchar_t*>(wcsrchr(static_cast<const wchar_t*>(s),c)); }
const wchar_t* wcsstr(const wchar_t* s,const wchar_t* part) { size_t n=wcslen(part);if(!n)return s;for(;*s;++s)if(!wcsncmp(s,part,n))return s;return nullptr; }
wchar_t* wcsstr(wchar_t* s,const wchar_t* part) { return const_cast<wchar_t*>(wcsstr(static_cast<const wchar_t*>(s),part)); }

// Integer parsing keeps input/end pointers in UTF-16 code units. Numeric syntax
// is ASCII; a non-ASCII code unit terminates that syntax instead of being lost.
static std::string numeric(const wchar_t* s) { std::string out;for(;*s && *s<128;++s)out.push_back(static_cast<char>(*s));return out; }
long wcstol(const wchar_t* s,wchar_t** end,int base) { auto text=numeric(s);char* e=nullptr;auto v=std::strtol(text.c_str(),&e,base);if(end)*end=const_cast<wchar_t*>(s)+(e-text.c_str());return v; }
unsigned long wcstoul(const wchar_t* s,wchar_t** end,int base) { auto text=numeric(s);char* e=nullptr;auto v=std::strtoul(text.c_str(),&e,base);if(end)*end=const_cast<wchar_t*>(s)+(e-text.c_str());return v; }

static bool decode8(const char* s,uint32_t& value,size_t& bytes) {
    const auto* p=reinterpret_cast<const unsigned char*>(s); unsigned char lead=*p;
    if(lead<128){value=lead;bytes=1;return true;}
    uint32_t minimum;
    if(lead>=0xc2 && lead<=0xdf){bytes=2;value=lead&31;minimum=0x80;}
    else if(lead>=0xe0 && lead<=0xef){bytes=3;value=lead&15;minimum=0x800;}
    else if(lead>=0xf0 && lead<=0xf4){bytes=4;value=lead&7;minimum=0x10000;}
    else return false;
    for(size_t i=1;i<bytes;++i){if((p[i]&0xc0)!=0x80)return false;value=(value<<6)|(p[i]&63);}
    return value>=minimum && value<=0x10ffff && !(value>=0xd800 && value<=0xdfff);
}
static bool decode16(const wchar_t* s,uint32_t& value,size_t& units) {
    value=*s;units=1;
    if(value>=0xd800 && value<=0xdbff){uint32_t low=s[1];if(low<0xdc00 || low>0xdfff)return false;value=0x10000+((value-0xd800)<<10)+(low-0xdc00);units=2;}
    else if(value>=0xdc00 && value<=0xdfff)return false;
    return true;
}
static size_t encode8(uint32_t c,char* out) {
    if(c<128){out[0]=char(c);return 1;}
    if(c<0x800){out[0]=char(0xc0|(c>>6));out[1]=char(0x80|(c&63));return 2;}
    if(c<0x10000){out[0]=char(0xe0|(c>>12));out[1]=char(0x80|((c>>6)&63));out[2]=char(0x80|(c&63));return 3;}
    out[0]=char(0xf0|(c>>18));out[1]=char(0x80|((c>>12)&63));out[2]=char(0x80|((c>>6)&63));out[3]=char(0x80|(c&63));return 4;
}
size_t mbsrtowcs(wchar_t* dest,const char** input,size_t capacity,mbstate_t*) {
    const char* s=*input;size_t written=0;
    while(true){uint32_t c;size_t bytes;if(!decode8(s,c,bytes)){errno=EILSEQ;if(dest)*input=s;return size_t(-1);}size_t units=c>0xffff?2:1;
        if(dest && units>capacity-written){*input=s;return written;}
        if(!c){if(dest){dest[written]=0;*input=nullptr;}return written;}
        if(dest){if(units==1)dest[written]=wchar_t(c);else{c-=0x10000;dest[written]=wchar_t(0xd800+(c>>10));dest[written+1]=wchar_t(0xdc00+(c&1023));}}
        written+=units;s+=bytes;
    }
}
size_t wcsrtombs(char* dest,const wchar_t** input,size_t capacity,mbstate_t*) {
    const wchar_t* s=*input;size_t written=0;
    while(true){uint32_t c;size_t units;if(!decode16(s,c,units)){errno=EILSEQ;if(dest)*input=s;return size_t(-1);}char encoded[4];size_t bytes=encode8(c,encoded);
        if(dest && bytes>capacity-written){*input=s;return written;}
        if(!c){if(dest){dest[written]=0;*input=nullptr;}return written;}
        if(dest)std::memcpy(dest+written,encoded,bytes);written+=bytes;s+=units;
    }
}

static bool append16(std::string& out,const wchar_t* s) { if(!s){out+="(null)";return true;}while(*s){uint32_t c;size_t n;if(!decode16(s,c,n)){errno=EILSEQ;return false;}char b[4];size_t bytes=encode8(c,b);out.append(b,bytes);s+=n;}return true; }
template<class T> static bool formatValue(std::string& out,const std::string& spec,T value) {
    int n=std::snprintf(nullptr,0,spec.c_str(),value);if(n<0)return false;
    std::vector<char> buffer(size_t(n)+1);std::snprintf(buffer.data(),buffer.size(),spec.c_str(),value);out.append(buffer.data(),size_t(n));return true;
}

// Format individual values through the narrow libc ABI, never send a 16-bit
// wchar_t buffer to the 32-bit wide libc. Unsupported formats fail explicitly.
int vswprintf(wchar_t* dest,size_t capacity,const wchar_t* fmt,va_list incoming) {
    std::string output;va_list args;va_copy(args,incoming);bool ok=true;
    for(const wchar_t* p=fmt;*p && ok;){
        if(*p!='%'){uint32_t c;size_t units;ok=decode16(p,c,units);if(!ok)break;char encoded[4];output.append(encoded,encode8(c,encoded));p+=units;continue;}
        ++p;if(*p=='%'){output+='%';++p;continue;}
        std::string flags;while(*p && std::strchr("-+ #0",char(*p)))flags+=char(*p++);
        int width=0;bool hasWidth=false;
        if(*p=='*'){width=va_arg(args,int);hasWidth=true;++p;if(width==std::numeric_limits<int>::min()){ok=false;break;}if(width<0){flags+='-';width=-width;}}
        else while(*p>='0' && *p<='9'){hasWidth=true;width=width*10+(*p++-'0');if(width>1000000){ok=false;break;}}
        int precision=-1;if(*p=='.'){++p;precision=0;if(*p=='*'){precision=va_arg(args,int);++p;}else while(*p>='0' && *p<='9'){precision=precision*10+(*p++-'0');if(precision>1000000){ok=false;break;}}}
        if (!ok || width>1000000 || precision>1000000) {ok=false;break;}
        std::string length;if(*p=='h' || *p=='l'){length+=char(*p++);if(*p==length[0])length+=char(*p++);}else if(*p=='j'||*p=='z'||*p=='t'||*p=='L')length+=char(*p++);
        else if(*p=='I' && p[1]=='6' && p[2]=='4'){length="ll";p+=3;}
        char kind=char(*p);if(!kind){ok=false;break;}++p;
        std::string spec="%"+flags+(hasWidth?std::to_string(width):"")+(precision>=0?"."+std::to_string(precision):"");
        if(kind=='s' || kind=='S'){
            std::string text;
            if(length=="h" || (kind=='S' && length!="l")){const char* s=va_arg(args,const char*);text=s?s:"(null)";}
            else {
                const wchar_t* value=va_arg(args,const wchar_t*);
                if(!value)value=L"(null)";
                size_t units=precision<0?wcslen(value):wcsnlen(value,size_t(precision));
                // Do not split a UTF-16 surrogate pair at a precision boundary.
                if(units && value[units-1]>=0xd800 && value[units-1]<=0xdbff)--units;
                std::vector<wchar_t> bounded(value,value+units);bounded.push_back(0);
                ok=append16(text,bounded.data());
            }
            if(ok){
                const char* cursor=text.c_str();size_t units=mbsrtowcs(nullptr,&cursor,0,nullptr);
                if(units==size_t(-1)){ok=false;break;}
                if((length=="h" || (kind=='S' && length!="l")) && precision>=0){
                    std::vector<wchar_t> wide(units+1);mbsrtowcs(wide.data(),&cursor,wide.size(),nullptr);
                    size_t cut=units<size_t(precision)?units:size_t(precision);
                    if(cut && wide[cut-1]>=0xd800 && wide[cut-1]<=0xdbff)--cut;
                    wide[cut]=0;text.clear();ok=append16(text,wide.data());units=cut;
                }
                size_t padding=width>0 && size_t(width)>units?size_t(width)-units:0;
                bool left=flags.find('-')!=std::string::npos;
                if(!left)output.append(padding,' ');output+=text;if(left)output.append(padding,' ');
            }
        }else if(kind=='c' || kind=='C'){
            unsigned c=unsigned(va_arg(args,int));if(c>0x10ffff || (c>=0xd800 && c<=0xdfff)){errno=EILSEQ;ok=false;break;}std::string text;char bytes[4];text.append(bytes,encode8(c,bytes));ok=formatValue(output,spec+"s",text.c_str());
        }else if(kind=='d' || kind=='i'){
            if(length=="ll"||length=="j")ok=formatValue(output,spec+"ll"+kind,va_arg(args,long long));
            else if(length=="l")ok=formatValue(output,spec+"l"+kind,va_arg(args,long));
            else if(length=="z"||length=="t")ok=formatValue(output,spec+"t"+kind,va_arg(args,ptrdiff_t));
            else ok=formatValue(output,spec+length+kind,va_arg(args,int));
        }else if(std::strchr("uoxX",kind)){
            if(length=="ll"||length=="j")ok=formatValue(output,spec+"ll"+kind,va_arg(args,unsigned long long));
            else if(length=="l")ok=formatValue(output,spec+"l"+kind,va_arg(args,unsigned long));
            else if(length=="z"||length=="t")ok=formatValue(output,spec+"z"+kind,va_arg(args,size_t));
            else ok=formatValue(output,spec+length+kind,va_arg(args,unsigned));
        }else if(std::strchr("fFeEgGaA",kind)){
            if(length=="L")ok=formatValue(output,spec+"L"+kind,va_arg(args,long double));else ok=formatValue(output,spec+kind,va_arg(args,double));
        }else if(kind=='p')ok=formatValue(output,spec+"p",va_arg(args,void*));
        else {errno=EINVAL;ok=false;}
    }
    va_end(args);if(!ok){if(dest && capacity)dest[0]=0;return -1;}
    const char* text=output.c_str();size_t required=mbsrtowcs(nullptr,&text,0,nullptr);
    if(required==size_t(-1) || !dest || !capacity){errno=EINVAL;return -1;}
    size_t written=mbsrtowcs(dest,&text,capacity-1,nullptr);dest[written==size_t(-1)?0:written]=0;
    if(required>=capacity){errno=ERANGE;return -1;}return int(required);
}
int fwprintf(FILE* stream,const wchar_t* format,...) {
    va_list arguments;va_start(arguments,format);
    for(size_t capacity=256;capacity<=1048576;capacity*=2){
        std::vector<wchar_t> buffer(capacity);
        int count=vswprintf(buffer.data(),capacity,format,arguments);
        if(count>=0){
            const wchar_t* cursor=buffer.data();size_t bytes=wcsrtombs(nullptr,&cursor,0,nullptr);
            if(bytes==size_t(-1)){va_end(arguments);return -1;}
            std::vector<char> encoded(bytes+1);wcsrtombs(encoded.data(),&cursor,encoded.size(),nullptr);
            va_end(arguments);return std::fwrite(encoded.data(),1,bytes,stream)==bytes?count:-1;
        }
        if(errno!=ERANGE){va_end(arguments);return -1;}
    }
    va_end(arguments);errno=EOVERFLOW;return -1;
}
}
