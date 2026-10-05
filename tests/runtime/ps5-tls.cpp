// Exercise the PS5 cleanup shim with real emulated C++ thread-local storage.
#include <atomic>
#include <cassert>
#include <pthread.h>
#include <string>

static std::atomic<int> destroyed{0};
thread_local int destructionOrder = 0;
struct Local {
    int order;
    std::string text = std::string(1024, 'x');
    ~Local() {
        assert(text.size() == 1024 && text.front() == 'x' && text.back() == 'x');
        assert(++destructionOrder == order);
        ++destroyed;
    }
};
thread_local Local first{2};
thread_local Local second{1};
static void* worker(void*) {
    assert(first.text.size() == 1024);
    assert(second.text.size() == 1024);
    return nullptr;
}
int main() {
    pthread_t threads[16];
    for (auto& thread : threads) assert(pthread_create(&thread, nullptr, worker, nullptr) == 0);
    for (auto& thread : threads) assert(pthread_join(thread, nullptr) == 0);
    assert(destroyed == 32);
}
