; Reference lowering of Zell async/await onto LLVM switched-resume coroutines
; and the Zell runtime. Built and run by `make test` (needs clang).
;
; Source being modelled:
;
;   async func work(x int) int {
;       await task.Delay(5)
;       return x * 2
;   }
;
;   async func outer(x int) int {
;       var a = await work(x)        // awaiting another async func
;       return a + 1
;   }
;
;   func main() {                   // non-async main: blocking await
;       var t1 = outer(10)
;       var t2 = outer(20)
;       println(await t1)           // 21
;       println(await t2)           // 41
;   }

declare ptr  @zell_task_new(i64, ptr)
declare void @zell_task_complete(ptr, ptr)
declare zeroext i1 @zell_task_is_done(ptr)
declare zeroext i1 @zell_task_add_continuation(ptr, ptr, ptr)
declare ptr  @zell_task_result(ptr)
declare ptr  @zell_task_await(ptr)
declare ptr  @zell_task_delay(i64)
declare void @zell_coro_resume(ptr)
declare ptr  @zell_coro_frame_alloc(i64)
declare void @zell_coro_frame_free(ptr)
declare void @zell_retain(ptr)
declare void @zell_release(ptr)
declare void @zell_println_int(i64)
declare void @zell_runtime_init(i32, ptr)
declare void @zell_runtime_shutdown()

declare token @llvm.coro.id(i32, ptr, ptr, ptr)
declare i64   @llvm.coro.size.i64()
declare ptr   @llvm.coro.begin(token, ptr)
declare token @llvm.coro.save(ptr)
declare i8    @llvm.coro.suspend(token, i1)
declare ptr   @llvm.coro.free(token, ptr)
declare i1    @llvm.coro.end(ptr, i1, token)

; ---------------------------------------------------------------- work(x)
define ptr @work(i64 %x) presplitcoroutine {
entry:
  %ret.slot = alloca i64, align 8
  ; 1. result task: +1 returned to the caller, +1 owned by the frame
  %task = call ptr @zell_task_new(i64 8, ptr null)
  call void @zell_retain(ptr %task)
  ; 2. coroutine frame on the zell heap
  %id   = call token @llvm.coro.id(i32 0, ptr null, ptr null, ptr null)
  %size = call i64 @llvm.coro.size.i64()
  %mem  = call ptr @zell_coro_frame_alloc(i64 %size)
  %hdl  = call ptr @llvm.coro.begin(token %id, ptr %mem)
  ; 3. body: `await task.Delay(5)` (result type void)
  %d = call ptr @zell_task_delay(i64 5)
  %d.done = call i1 @zell_task_is_done(ptr %d)
  br i1 %d.done, label %d.ready, label %d.try_suspend

d.try_suspend:
  %d.save = call token @llvm.coro.save(ptr %hdl)
  %d.reg  = call i1 @zell_task_add_continuation(ptr %d, ptr @zell_coro_resume, ptr %hdl)
  br i1 %d.reg, label %d.suspend, label %d.ready     ; false: completed meanwhile

d.suspend:
  %d.s = call i8 @llvm.coro.suspend(token %d.save, i1 false)
  switch i8 %d.s, label %suspended [i8 0, label %d.ready
                                    i8 1, label %cleanup]

d.ready:
  call void @zell_release(ptr %d)
  ; 4. `return x * 2`: store into the task, drop the frame's task reference
  %r = mul i64 %x, 2
  store i64 %r, ptr %ret.slot
  call void @zell_task_complete(ptr %task, ptr %ret.slot)
  call void @zell_release(ptr %task)
  br label %cleanup

cleanup:                                    ; no final suspend: frame frees itself
  %mem.free = call ptr @llvm.coro.free(token %id, ptr %hdl)
  call void @zell_coro_frame_free(ptr %mem.free)
  br label %suspended

suspended:
  %unused = call i1 @llvm.coro.end(ptr %hdl, i1 false, token none)
  ret ptr %task                             ; ramp returns the task (resume clones return void)
}

; ---------------------------------------------------------------- outer(x)
define ptr @outer(i64 %x) presplitcoroutine {
entry:
  %ret.slot = alloca i64, align 8
  %task = call ptr @zell_task_new(i64 8, ptr null)
  call void @zell_retain(ptr %task)
  %id   = call token @llvm.coro.id(i32 0, ptr null, ptr null, ptr null)
  %size = call i64 @llvm.coro.size.i64()
  %mem  = call ptr @zell_coro_frame_alloc(i64 %size)
  %hdl  = call ptr @llvm.coro.begin(token %id, ptr %mem)
  ; `var a = await work(x)`
  %w = call ptr @work(i64 %x)
  %w.done = call i1 @zell_task_is_done(ptr %w)
  br i1 %w.done, label %w.ready, label %w.try_suspend

w.try_suspend:
  %w.save = call token @llvm.coro.save(ptr %hdl)
  %w.reg  = call i1 @zell_task_add_continuation(ptr %w, ptr @zell_coro_resume, ptr %hdl)
  br i1 %w.reg, label %w.suspend, label %w.ready

w.suspend:
  %w.s = call i8 @llvm.coro.suspend(token %w.save, i1 false)
  switch i8 %w.s, label %suspended [i8 0, label %w.ready
                                    i8 1, label %cleanup]

w.ready:
  %w.res = call ptr @zell_task_result(ptr %w)   ; borrowed slot
  %a = load i64, ptr %w.res                      ; (retain here if T is refcounted)
  call void @zell_release(ptr %w)
  %r = add i64 %a, 1
  store i64 %r, ptr %ret.slot
  call void @zell_task_complete(ptr %task, ptr %ret.slot)
  call void @zell_release(ptr %task)
  br label %cleanup

cleanup:
  %mem.free = call ptr @llvm.coro.free(token %id, ptr %hdl)
  call void @zell_coro_frame_free(ptr %mem.free)
  br label %suspended

suspended:
  %unused = call i1 @llvm.coro.end(ptr %hdl, i1 false, token none)
  ret ptr %task
}

; ---------------------------------------------------------------- main
define i32 @main(i32 %argc, ptr %argv) {
entry:
  call void @zell_runtime_init(i32 %argc, ptr %argv)
  %t1 = call ptr @outer(i64 10)
  %t2 = call ptr @outer(i64 20)
  %p1 = call ptr @zell_task_await(ptr %t1)      ; blocks the main thread
  %v1 = load i64, ptr %p1
  call void @zell_println_int(i64 %v1)
  %p2 = call ptr @zell_task_await(ptr %t2)
  %v2 = load i64, ptr %p2
  call void @zell_println_int(i64 %v2)
  call void @zell_release(ptr %t1)
  call void @zell_release(ptr %t2)
  call void @zell_runtime_shutdown()
  ret i32 0
}
