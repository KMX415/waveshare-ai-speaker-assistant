#include "runtime_gate.h"
#include <stdatomic.h>
static atomic_int current;
bool runtime_acquire(runtime_owner owner) {
    if(owner==RUNTIME_IDLE)return false;
    int expected=RUNTIME_IDLE;
    return atomic_compare_exchange_strong(&current,&expected,owner);
}
void runtime_release(runtime_owner owner) {
    int expected=owner;
    atomic_compare_exchange_strong(&current,&expected,RUNTIME_IDLE);
}
bool runtime_updating(void){return atomic_load(&current)==RUNTIME_UPDATE;}
