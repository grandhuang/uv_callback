#ifndef UV_CALLBACK_H
#define UV_CALLBACK_H
#ifdef __cplusplus
extern "C" {
#endif

#include <uv.h>


/* Typedefs */

typedef struct uv_callback_s   uv_callback_t;
typedef struct uv_call_s       uv_call_t;


/* Callback Functions */

typedef void* (*uv_callback_func)(uv_callback_t* handle, void *data, int size);


/* Functions */

int uv_callback_init(uv_loop_t* loop, uv_callback_t* callback, uv_callback_func function, int callback_type);

int uv_callback_init_ex(
   uv_loop_t* loop,
   uv_callback_t* callback,
   uv_callback_func function,
   int callback_type,
   void (*free_cb)(void*),
   void (*free_result)(void*)
);

int uv_callback_fire(uv_callback_t* callback, void *data, uv_callback_t* notify);

int uv_callback_fire_ex(uv_callback_t* callback, void *data, int size, void (*free_data)(void*), uv_callback_t* notify);

int uv_callback_fire_sync(uv_callback_t* callback, void *data, void** presult, int timeout);

void uv_callback_stop(uv_callback_t* callback);
void uv_callback_stop_all(uv_loop_t* loop);

int uv_is_callback(uv_handle_t *handle);
void uv_callback_release(uv_callback_t *callback);


/* Constants */

#define UV_DEFAULT      0
#define UV_COALESCE     1


/* Structures */

/* 异步回调事件管理器 */
struct uv_callback_s 
{
   uv_async_t async;          /* 用于线程信号的基础异步句柄 */
   void *data;                /* 附加数据指针，与句柄中的数据不同 */
   int usequeue;              /* 如果此回调使用调用队列 */
   uv_call_t *queue;          /* 对此回调的调用队列 */
   uv_mutex_t mutex;          /* 用于访问队列的互斥锁 */
   uv_callback_func function; /* 要调用的函数 */
   void *arg;                 /* 合并调用时的数据参数（不使用队列时） */
   uv_idle_t idle;            /* 空闲句柄，用于在处理旧的异步请求时排空队列 */
   int idle_active;           /* 空闲句柄是否激活的标志 */
   uv_callback_t *master;     /* 主回调句柄，具有有效 uv_async 句柄的那个 */
   uv_callback_t *next;       /* 此 uv_async 句柄的下一个回调 */
   int inactive;              /* 此回调不再有效，被调用的线程不应触发响应回调 */
   int refcount;              /* 引用计数器 */
   void (*free_cb)(void*);    /* 释放此对象的函数 */
   void (*free_result)(void*);/* 释放未使用的调用结果的函数 */
};

/* 异步回调事件队列管理器 */
struct uv_call_s 
{
   uv_call_t *next;           /* 指向下一个队列中的调用的指针 */
   uv_callback_t *callback;   /* 与此调用关联的回调 */
   void *data;                /* 此调用的数据参数 */
   int   size;                /* 此调用的大小参数 */
   void (*free_data)(void*);  /* 如果调用未触发，释放数据的函数 */
   uv_callback_t *notify;     /* 将与此调用的结果一起触发的回调 */
};



#ifdef __cplusplus
}
#endif
#endif  // UV_CALLBACK_H
