#include <pthread.h>
#include <stdlib.h>

// ---- C++ thread_local destructors (libc++abi) ------------------------------
// Runs registered destructors when the thread exits, in reverse order.
typedef struct ps5_tls_dtor {
    void (*dtor)(void*);
    void* obj;
    struct ps5_tls_dtor* next;
} ps5_tls_dtor;

static pthread_key_t ps5_tls_dtor_key;
static pthread_once_t ps5_tls_dtor_once = PTHREAD_ONCE_INIT;

static void ps5_tls_dtor_run(void* head) {
    ps5_tls_dtor* node = (ps5_tls_dtor*)head;
    while (node != NULL) {
        ps5_tls_dtor* next = node->next;
        node->dtor(node->obj);
        free(node);
        node = next;
    }
}

static void ps5_tls_dtor_init(void) {
    pthread_key_create(&ps5_tls_dtor_key, ps5_tls_dtor_run);
}

int __cxa_thread_atexit_impl(void (*dtor)(void*), void* obj, void* dso) {
    pthread_once(&ps5_tls_dtor_once, ps5_tls_dtor_init);
    ps5_tls_dtor* node = (ps5_tls_dtor*)malloc(sizeof(*node));
    if (node == NULL) {
        return -1;
    }
    node->dtor = dtor;
    node->obj = obj;
    node->next = (ps5_tls_dtor*)pthread_getspecific(ps5_tls_dtor_key);
    pthread_setspecific(ps5_tls_dtor_key, node);
    return 0;
}

