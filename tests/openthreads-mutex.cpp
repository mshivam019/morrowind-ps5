#include <OpenThreads/Mutex>
#include <OpenThreads/Condition>
#include <system_error>
#include <cerrno>
#include <cassert>
#include <pthread.h>
static bool fail_attr = false;
extern "C" int __real_pthread_mutexattr_init(pthread_mutexattr_t*);
extern "C" int __wrap_pthread_mutexattr_init(pthread_mutexattr_t* attr) {
    return fail_attr ? ENOMEM : __real_pthread_mutexattr_init(attr);
}
int main() {
    for (int i=0; i<100000; ++i) {
        OpenThreads::Mutex m(OpenThreads::Mutex::MUTEX_RECURSIVE);
        assert(m.lock()==0);
        assert(m.lock()==0);
        assert(m.unlock()==0);
        assert(m.unlock()==0);
        OpenThreads::Condition condition;
    }
    fail_attr=true;
    bool thrown=false;
    try { OpenThreads::Mutex m; }
    catch (const std::system_error& error) { thrown=error.code().value()==ENOMEM; }
    assert(thrown);
}
