#include <stdlib.h>
#include <string.h>
#include "uv_callback.h"

// not covered now: closing a uv_callback handle does not release all the resources
// automatically.
// for this libuv should support calling a callback when our handle is being closed.
// for now we must use the uv_callback_stop or .._stop_all before closing the event
// loop and then call uv_callback_release on the callback from uv_close.

/*****************************************************************************/
/* RECEIVER / CALLED THREAD **************************************************/
/* 接收者   / 调用者  线程       **********************************************/
/*****************************************************************************/

#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

void uv_callback_idle_cb(uv_idle_t *handle);

/* Master Callback ***********************************************************/

/**
 * 检查给定的句柄是否是回调句柄。
 *
 * @param handle 要检查的句柄。
 * @return 如果句柄是回调句柄，则返回非零值，否则返回零。
 */
int uv_is_callback(uv_handle_t *handle)
{
   return (handle->type == UV_ASYNC && handle->data == handle);
}

/**
 * 遍历循环中的句柄时调用的回调函数。
 *
 * @param handle 正在遍历的句柄。
 * @param arg 传递给 uv_walk 的参数。
 */
void master_on_walk(uv_handle_t *handle, void *arg)
{
   if (handle->type == UV_ASYNC && ((uv_callback_t *)handle)->usequeue)
   {
      *(uv_callback_t **)arg = (uv_callback_t *)handle;
   }
}

/**
 * 从给定的循环中检索主回调句柄。
 *
 * @param loop 要搜索主回调句柄的循环。
 * @return 如果找到主回调句柄，则返回该句柄，否则返回 NULL。
 */
uv_callback_t *get_master_callback(uv_loop_t *loop)
{
   uv_callback_t *callback = 0;
   uv_walk(loop, master_on_walk, &callback);
   return callback;
}

/* 回调释放 **********************************************************/

/**
 * 释放回调对象。
 *
 * @param callback 要释放的回调对象。
 */
void uv_callback_release(uv_callback_t *callback)
{
   if (callback)
   {
      callback->refcount--;
      if (callback->refcount == 0 && callback->free_cb)
      {
         /* remove the object from the list */
         uv_callback_t *cb = callback->master;
         while (cb)
         {
            if (cb->next == callback)
            {
               cb->next = callback->next;
               break;
            }
            cb = cb->next;
         }
         /* stop the idle handle */
         if (callback->idle_active)
         {
            uv_idle_stop(&callback->idle);
         }
         /* release the object */
         callback->free_cb(callback);
      }
   }
}

/* Dequeue *******************************************************************/

/**
 * 从回调对象的队列中出列一个调用。
 *
 * @param callback 回调对象。
 * @return 出列的调用对象，如果队列为空，则返回 NULL。
 */
void *dequeue_call(uv_callback_t *callback)
{
   uv_call_t *current, *prev = NULL;

   uv_mutex_lock(&callback->mutex);

   current = callback->queue;
   while (current && current->next)
   {
      prev = current;
      current = current->next;
   }

   if (prev)
      prev->next = NULL;
   else
      callback->queue = NULL;

   uv_mutex_unlock(&callback->mutex);

   return current;
}

/**
 * 从主回调对象的队列中移除所有属于指定回调对象的调用。
 *
 * @param master 主回调对象。
 * @param callback 要移除其调用的回调对象。
 */
void dequeue_all_from_callback(uv_callback_t *master, uv_callback_t *callback)
{
   uv_call_t *call, *prev = NULL;

   if (!master)
      master = callback;

   uv_mutex_lock(&master->mutex);

   call = master->queue;
   while (call)
   {
      uv_call_t *next = call->next;
      if (call->callback == callback)
      {
         /* remove it from the queue */
         if (prev)
            prev->next = next;
         else
            master->queue = next;
         /* discard this call */
         if (call->data && call->free_data)
         {
            call->free_data(call->data);
         }
         free(call);
      }
      else
      {
         prev = call;
      }
      /* move to the next call */
      call = next;
   }

   callback->queue = NULL;

   uv_mutex_unlock(&master->mutex);
}

/* Callback Function Call ****************************************************/

/**
 * 异步回调的处理函数。
 *
 * @param handle 异步回调的句柄。
 */
void uv_callback_async_cb(uv_async_t *handle)
{
   uv_callback_t *callback = (uv_callback_t *)handle;

   if (callback->usequeue)
   {
      uv_call_t *call = dequeue_call(callback);//取出下一个队列调用
      if (call)
      {
         void *result = call->callback->function(call->callback, call->data, call->size);
         /* check if the result notification callback is still active */
         if (call->notify && !call->notify->inactive)
         {
            uv_callback_fire(call->notify, result, NULL);
         }
         else if (result && call->callback->free_result)
         {
            call->callback->free_result(result);
         }
         if (call->notify)
         {
            uv_callback_release(call->notify);
         }
         if (call->data && call->free_data)
         {
            call->free_data(call->data);
         }
         free(call);
         /* don't check for new calls now to prevent the loop from blocking
         for i/o events. start an idle handle to call this function again */
         if (!callback->idle_active)
         {
            uv_idle_start(&callback->idle, uv_callback_idle_cb);
            callback->idle_active = 1;
         }
      }
      else
      {
         /* no more calls in the queue. stop the idle handle */
         uv_idle_stop(&callback->idle);
         callback->idle_active = 0;
      }
   }
   else
   {
      callback->function(callback, callback->arg, 0);
   }
}

/**
 * 空闲回调的处理函数。
 *
 * @param handle 空闲回调的句柄。
 */
void uv_callback_idle_cb(uv_idle_t *handle)
{
   uv_callback_t *callback = container_of(handle, uv_callback_t, idle);
   uv_callback_async_cb((uv_async_t *)callback);
}

/* 初始化 ************************************************************/

/**
 * 初始化回调对象。
 *
 * @param loop 事件循环。
 * @param callback 回调对象。
 * @param function 回调函数。
 * @param callback_type 回调类型。
 * @param free_cb 释放回调对象时调用的函数。
 * @param free_result 释放回调结果时调用的函数。
 * @return 返回初始化结果。
 */
int uv_callback_init_ex(
    uv_loop_t *loop,
    uv_callback_t *callback,
    uv_callback_func function,
    int callback_type,
    void (*free_cb)(void *),
    void (*free_result)(void *))
{
   int rc;

   if (!loop || !callback || !function)
      return UV_EINVAL;

   memset(callback, 0, sizeof(uv_callback_t));
   callback->async.data = callback; /* mark as a uv_callback handle */

   callback->function = function;

   callback->refcount = 1;
   callback->free_cb = free_cb;

   switch (callback_type)
   {
   case UV_DEFAULT:
      callback->usequeue = 1;
      callback->free_result = free_result;
      callback->master = get_master_callback(loop);
      if (callback->master)
      {
         /* add this callback to the list */
         uv_callback_t *base = callback->master;
         while (base->next)
         {
            base = base->next;
         }
         base->next = callback;
         return 0; /* the uv_async handle is already initialized */
      }
      else
      {
         uv_mutex_init(&callback->mutex);
         rc = uv_idle_init(loop, &callback->idle);
         if (rc)
            return rc;
      }
      /* fallthrough */
   case UV_COALESCE:
      break;
   default:
      return UV_EINVAL;
   }

   return uv_async_init(loop, (uv_async_t *)callback, uv_callback_async_cb);
}

/**
 * 初始化回调对象。
 *
 * @param loop 事件循环。
 * @param callback 回调对象。
 * @param function 回调函数。
 * @param callback_type 回调类型。
 * @return 返回初始化结果。
 */
int uv_callback_init(uv_loop_t *loop, uv_callback_t *callback, uv_callback_func function, int callback_type)
{
   return uv_callback_init_ex(loop, callback, function, callback_type, NULL, NULL);
}

/**
 * 释放回调对象。
 * param callback 回调对象。
 * @return 返回释放结果。
 */
void uv_callback_stop(uv_callback_t *callback)
{

   if (!callback)
      return;

   callback->inactive = 1;

   if (callback->usequeue)
   {
      dequeue_all_from_callback(callback->master, callback);
   }
}

/**
 * 停止所有回调。
 * param loop 事件循环。
 * @return 返回释放结果。
 */
void stop_all_on_walk(uv_handle_t *handle, void *arg)
{
   if (uv_is_callback(handle))
   {
      uv_callback_t *callback = (uv_callback_t *)handle;
      while (callback)
      {
         uv_callback_t *next = callback->next;
         uv_callback_stop(callback);
         callback = next;
      }
   }
}

/**
 * 停止所有回调。
 * param loop 事件循环。
 * @return 返回释放结果。
 */
void uv_callback_stop_all(uv_loop_t *loop)
{
   uv_walk(loop, stop_all_on_walk, NULL);
}

/*****************************************************************************/
/* SENDER / CALLER THREAD ****************************************************/
/* 发送者 / 调用者  线程       ************************************************/
/*****************************************************************************/

/* 异步回调触发 **********************************************/

/**
 * 异步触发回调函数。
 *
 * @param callback 回调对象。
 * @param data 回调数据。
 * @param size 数据大小。
 * @param free_data 释放数据的函数。
 * @param notify 通知回调对象。
 * @return 返回触发结果。
 */
int uv_callback_fire_ex(uv_callback_t *callback, void *data, int size, void (*free_data)(void *), uv_callback_t *notify)
{

   if (!callback)
      return UV_EINVAL;
   if (callback->inactive)
      return UV_EPERM;

   /* 如果设置了通知回调，则调用必须使用队列 */
   if (notify && !callback->usequeue)
      return UV_EINVAL;

   if (callback->usequeue)
   {
      /* 分配一个新的调用信息 */
      uv_call_t *call = malloc(sizeof(uv_call_t));
      if (!call)
         return UV_ENOMEM;
      /* 保存调用信息 */
      call->data = data;
      call->size = size;
      call->notify = notify;
      call->callback = callback;
      call->free_data = free_data;
      /* 如果有主回调，则使用它 */
      if (callback->master)
         callback = callback->master;
      /* 将调用添加到队列中 */
      uv_mutex_lock(&callback->mutex);
      call->next = callback->queue;
      callback->queue = call;
      uv_mutex_unlock(&callback->mutex);
      /* 增加引用计数器 */
      if (notify)
         notify->refcount++;
   }
   else
   {
      callback->arg = data;
   }

   /* 调用 uv_async_send */
   return uv_async_send((uv_async_t *)callback);
}

/**
 * 触发回调函数。
 * @param callback 回调对象。
 * @param data 回调数据。
 * @param notify 通知回调对象。
 * @return 返回触发结果。
 */
int uv_callback_fire(uv_callback_t *callback, void *data, uv_callback_t *notify)
{
   return uv_callback_fire_ex(callback, data, 0, NULL, notify);
}

/* 同步回调触发 ***********************************************/

struct call_result
{
   int timed_out;
   int called;
   void *data;
   int size;
};

/**
 * 当回调句柄关闭时调用的回调函数。
 *
 * @param handle 关闭的句柄。
 */
void callback_on_close(uv_handle_t *handle)
{
   if (uv_is_callback(handle))
   {
      uv_callback_release((uv_callback_t *)handle);
   }
}

/**
 * 当回调句柄关闭时调用的回调函数。
 *
 * @param handle 关闭的句柄。
 * @param arg 回调函数的参数。
 */
void callback_on_walk(uv_handle_t *handle, void *arg)
{
   uv_close(handle, callback_on_close);
}

/**
 * 处理回调结果的函数。
 *
 * @param callback 回调对象。
 * @param data 回调数据。
 * @param size 数据大小。
 * @return 返回 NULL。
 */
void *on_call_result(uv_callback_t *callback, void *data, int size)
{
   uv_loop_t *loop = ((uv_handle_t *)callback)->loop;
   struct call_result *result = loop->data;
   result->called = 1;
   result->data = data;
   result->size = size;
   uv_stop(loop);
   return NULL;
}

/**
 * 处理定时器超时事件的回调函数。
 *
 * @param timer 超时的定时器。
 */
void on_timer(uv_timer_t *timer)
{
   uv_loop_t *loop = timer->loop;
   struct call_result *result = loop->data;
   result->timed_out = 1;
   uv_stop(loop);
}

/**
 * 同步触发回调函数。
 *
 * @param callback 回调对象。
 * @param data 回调数据。
 * @param presult 存储回调结果的指针。
 * @param timeout 超时时间（毫秒）。
 * @return 返回触发结果。
 */
int uv_callback_fire_sync(uv_callback_t *callback, void *data, void **presult, int timeout)
{
   struct call_result result = {0};
   uv_loop_t loop;
   uv_timer_t timer;
   uv_callback_t *notify; /* must be allocated because it is shared with the called thread */
   int rc = 0;

   if (!callback || callback->usequeue == 0)
      return UV_EINVAL;

   notify = malloc(sizeof(uv_callback_t));
   if (!notify)
      return UV_ENOMEM;

   /* set the call result */
   uv_loop_init(&loop);
   uv_callback_init_ex(&loop, notify, on_call_result, UV_DEFAULT, free, NULL);
   loop.data = &result;

   /* fire the callback on the other thread */
   rc = uv_callback_fire(callback, data, notify);
   if (rc)
   {
      uv_close((uv_handle_t *)notify, callback_on_close);
      goto loc_exit;
   }

   /* if a timeout is supplied, set a timer */
   if (timeout > 0)
   {
      uv_timer_init(&loop, &timer);
      uv_timer_start(&timer, on_timer, timeout, 0);
   }

   /* run the event loop */
   uv_run(&loop, UV_RUN_DEFAULT);

   /* exited the event loop */
   /* before closing the loop handles */
   // uv_callback_stop(notify);
   uv_callback_stop_all(&loop);
   uv_walk(&loop, callback_on_walk, NULL);
   uv_run(&loop, UV_RUN_DEFAULT);
loc_exit:
   uv_loop_close(&loop);

   /* store the result */
   if (presult)
      *presult = result.data;
   if (rc == 0 && result.timed_out)
      rc = UV_ETIMEDOUT;
   if (rc == 0 && result.called == 0)
      rc = UV_UNKNOWN;
   return rc;
}

int main()
{
   printf("Hello World\n");

   uv_loop_t loop;
   uv_loop_init(&loop);
   printf("uv_loop_init success\n");

   // 测试 uv_callback_init_ex

   uv_callback_t callback;
   int ex_result = uv_callback_init_ex(&loop, &callback, NULL, UV_DEFAULT, NULL, NULL);
   printf("uv_callback_init_ex result: %d\n", ex_result);

   // 测试 uv_callback_fire
   int fire_result = uv_callback_fire(&callback, NULL, NULL);
   printf("uv_callback_fire result: %d\n", fire_result);

   // 测试 uv_callback_fire_sync
   void *sync_result = NULL;
   int sync_timeout = 1000; // 1 second
   int fire_sync_result = uv_callback_fire_sync(&callback, NULL, &sync_result, sync_timeout);
   printf("uv_callback_fire_sync result: %d\n", fire_sync_result);

   // 测试 uv_callback_stop_all
   uv_callback_stop_all(&loop);

   uv_loop_close(&loop);

   return 0;
}