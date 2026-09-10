#include <cassert>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#define MESA_TRACE_FUNC() ((void)0)
#define likely(v) (v)
#define PIPE_FLUSH_HINT_FINISH 16
struct zink_screen { bool device_lost; };
struct pipe_context { zink_screen *screen; void (*flush)(pipe_context *, void *, unsigned); };
struct zink_batch_usage { unsigned usage, submit_count; int flush, mtx; bool unflushed; };
struct zink_batch_state { zink_batch_usage usage; };
enum pipe_reset_status { PIPE_NO_RESET, PIPE_GUILTY_CONTEXT_RESET };
struct zink_context {
    pipe_context base;
    zink_batch_state *bs;
    bool is_device_lost;
    struct { void (*reset)(void *, pipe_reset_status); void *data; } reset;
};
#define zink_screen(p) (p)
#define zink_context(p) reinterpret_cast<struct zink_context *>(p)
#define debug_printf(...) ((void)0)
static bool zink_batch_usage_exists(const zink_batch_usage *u) { return u && (u->usage || u->unflushed); }
static bool zink_batch_usage_is_unflushed(const zink_batch_usage *u) { return u && u->unflushed; }
static unsigned zink_batch_submit_count_diff(unsigned a, unsigned b) { return a > b ? a-b : b-a; }
enum class Event { Submit, Lost, SpuriousThenSubmit, Never, SubmitBeforeLock, Reuse };
static Event event;
static zink_batch_usage *waiting;
static struct zink_screen *active_screen;
static unsigned waits, flushes, callbacks;
static int lock_depth;
static bool future_deadline;
static void mtx_lock(int *) {
    assert(lock_depth++ == 0);
    if (event == Event::SubmitBeforeLock) waiting->unflushed = false;
}
static void mtx_unlock(int *) { assert(--lock_depth == 0); }
[[maybe_unused]] static void cnd_wait(int *, int *) {
    ++waits;
    throw std::runtime_error("unbounded wait cannot observe abandoned batch");
}
static int cnd_timedwait(int *, int *, const timespec *deadline) {
    assert(lock_depth == 1);
    timespec now;
    timespec_get(&now, TIME_UTC);
    future_deadline |= deadline->tv_sec > now.tv_sec ||
        (deadline->tv_sec == now.tv_sec && deadline->tv_nsec > now.tv_nsec);
    if (++waits > 3) throw std::runtime_error("wait did not recheck loss or generation");
    if (event == Event::Submit || (event == Event::SpuriousThenSubmit && waits == 2))
        waiting->unflushed = false;
    if (event == Event::Lost) active_screen->device_lost = true;
    if (event == Event::Reuse) waiting->submit_count += 2;
    return 0;
}
static void flush(pipe_context *ctx, void *, unsigned) {
    ++flushes;
    if (event == Event::Lost) ctx->screen->device_lost = true;
    else waiting->unflushed = false;
}
static void reset_callback(void *, pipe_reset_status) { ++callbacks; }
// PRODUCTION_FUNCTIONS
static int checks, failures;
static void check(bool passed, const char *name) {
    ++checks;
    if (!passed) { ++failures; std::printf("FAIL: %s\n", name); }
}
static bool invoke(zink_context *ctx, unsigned count, bool trywait) {
    try { return zink_batch_usage_unflushed_wait(ctx, waiting, count, trywait); }
    catch (const std::exception &e) { check(false, e.what()); lock_depth = 0; return false; }
}
int main() {
    const Event events[]{Event::Submit, Event::Lost, Event::SpuriousThenSubmit,
                         Event::Never, Event::SubmitBeforeLock, Event::Reuse};
    for (Event next : events) {
        struct zink_screen screen{};
        zink_batch_state current{};
        zink_batch_usage usage{0, 2532, 0, 0, true};
        zink_context ctx{{&screen, flush}, &current, false, {reset_callback, nullptr}};
        event = next; waiting = &usage; active_screen = &screen;
        waits = flushes = callbacks = 0; lock_depth = 0; future_deadline = false;
        bool completed = invoke(&ctx, 2532, event == Event::Never);
        check(completed == (event == Event::Submit || event == Event::SpuriousThenSubmit ||
                            event == Event::SubmitBeforeLock), "only submitted batch reports ready");
        check(flushes == 0 && lock_depth == 0, "foreign batch leaves context and lock intact");
        if (event == Event::Lost) {
            check(waits == 1 && !completed && usage.unflushed && !usage.usage,
                  "late loss without signal stops wait without inventing submission");
            check(zink_get_device_reset_status(&ctx.base) == PIPE_GUILTY_CONTEXT_RESET && callbacks == 1,
                  "shared device loss reports removal to frontend once");
        }
        if (event == Event::SpuriousThenSubmit) check(waits == 2, "spurious wake retains wait");
        if (event == Event::Never) check(waits == 1 && !completed, "trywait timeout is not completion");
        if (event == Event::SubmitBeforeLock) check(!waits, "predicate rechecked after lock");
        if (waits) check(future_deadline, "condition wait uses absolute future deadline");
    }
    struct zink_screen screen{true};
    zink_batch_state current{{0, 10, 0, 0, true}};
    zink_context ctx{{&screen, flush}, &current, false, {reset_callback, nullptr}};
    active_screen = &screen; waiting = &current.usage; waits = flushes = 0;
    check(!invoke(&ctx, 10, false) && !waits && !flushes, "already lost device does no wait or flush");
    screen.device_lost = false; event = Event::Lost;
    check(!invoke(&ctx, 10, false) && flushes == 1, "own flush failure is not success");
    ctx.is_device_lost = false; screen.device_lost = false; callbacks = 0;
    check(zink_get_device_reset_status(&ctx.base) == PIPE_NO_RESET && !callbacks,
          "healthy context does not report removal");
    std::printf("Zink batch wait: %d/%d PASS\n", checks-failures, checks);
    return failures ? 1 : 0;
}
