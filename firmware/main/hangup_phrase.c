#include "hangup_phrase.h"
#include <string.h>
void hangup_phrase_reset(hangup_phrase_t *p){memset(p,0,sizeof(*p));}
void hangup_phrase_feed(hangup_phrase_t *p,const char *delta,uint32_t now){
    if(!delta || !*delta)return;
    if((uint32_t)(now-p->last_ms)>3000)hangup_phrase_reset(p);
    p->last_ms=now;
    while(*delta){
        unsigned char c=(unsigned char)*delta++;
        if(c>='A' && c<='Z')c+='a'-'A';
        if(!((c>='a' && c<='z') || (c>='0' && c<='9')))c=' ';
        if(c==' ' && (!p->used || p->text[p->used-1]==' '))continue;
        if(p->used==sizeof(p->text)-1){
            // Drop a whole word so a truncated suffix cannot become a command.
            char *space=strchr(p->text,' ');
            if(space){unsigned n=(unsigned)(space-p->text)+1;memmove(p->text,p->text+n,p->used-n);p->used-=n;}
            else {p->used=1;p->text[0]='x';}
        }
        p->text[p->used++]=(char)c;p->text[p->used]=0;
    }
}
bool hangup_phrase_ready(const hangup_phrase_t *p,uint32_t now){
    // Live has no transcript-done event. Allow the final streamed word to settle.
    if((uint32_t)(now-p->last_ms)<600)return false;
    const char *phrases[]={"goodbye jarvis","good bye jarvis"};
    for(unsigned i=0;i<2;i++){
        const char *s=p->text;
        while((s=strstr(s,phrases[i]))){
            size_t n=strlen(phrases[i]);
            if((s==p->text || s[-1]==' ') && (s[n]==0 || s[n]==' '))return true;
            s++;
        }
    }
    return false;
}
bool hangup_phrase_selftest(void){
    hangup_phrase_t p={0};
    hangup_phrase_feed(&p,"GOOD",100);hangup_phrase_feed(&p,"bye, Jar",200);hangup_phrase_feed(&p,"vis!",300);
    if(hangup_phrase_ready(&p,500)||!hangup_phrase_ready(&p,900))return false;
    hangup_phrase_reset(&p);hangup_phrase_feed(&p,"goodbye Jarvison",100);
    if(hangup_phrase_ready(&p,900))return false;
    hangup_phrase_reset(&p);hangup_phrase_feed(&p,"goodbye",100);hangup_phrase_feed(&p," Jarvis",4000);
    if(hangup_phrase_ready(&p,5000))return false;
    hangup_phrase_reset(&p);hangup_phrase_feed(&p,"Good-bye, JARVIS.",100);
    if(!hangup_phrase_ready(&p,900))return false;
    hangup_phrase_reset(&p);hangup_phrase_feed(&p,"hello Jarvis goodbye",100);
    return !hangup_phrase_ready(&p,900);
}
