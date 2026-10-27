#include <stdafx.hpp>
#include <diagnostics/journal.hpp>
#include <app/runtime_session.hpp>
#include <app/context.hpp>
#include <app/workers.hpp>
#include <scripting/runtime.hpp>
#include <app/trace_session.hpp>
#include <simulation/shot_trace.hpp>

namespace app
{
namespace
{
enum worker_id : foundation::thread_group::worker_id
{
    process_monitor,
    game_state,
    movement,
    combat,
    lineup,
    seed_trigger,
    watchdog,
    profile_timeout,
};
}
runtime_session::runtime_session(context_t& context)
    : m_context(context)
    , m_stop(m_source.get_token())
    , m_workers(worker_failed, this, m_source)
{
}

runtime_session::~runtime_session()
{
    quiesce();
    // Overlay scope cleanup has already drained render-owned futures. Keep the
    // process valid through that cleanup, not merely until worker join.
    m_context.process.detach();
}

void runtime_session::worker_failed(void* owner, foundation::thread_group::worker_id id,
                                    foundation::thread_group::worker_error error) noexcept
{
    diagnostics::emit(error.system ? diagnostics::event::runtime_worker_system_error
                                   : diagnostics::event::runtime_worker_unknown_exception,
                      diagnostics::severity::error,
                      error.system ? diagnostics::code_domain::system : diagnostics::code_domain::none,
                      static_cast<std::uint32_t>(error.code), static_cast<diagnostics::subject>(id + 1));
    static_cast<runtime_session*>(owner)->m_context.overlay.request_shutdown();
}

bool runtime_session::initialize_scripting()
{
    auto& scripts = scripting::runtime();
    m_scripting_constructed = true;
    return scripts.initialize();
}

void runtime_session::initialize_trace()
{
    m_trace_started = true;
    app::trace_session::initialize();
}

void runtime_session::start_workers()
{
    const auto process = static_cast<HANDLE>(m_context.process.native_handle());
    if (!process)
        throw std::logic_error("Workers require an attached process");
    m_workers.start(process_monitor,
                    [this, process](std::stop_token)
                    {
                        const HANDLE handles[]{m_stop.get(), process};
                        const auto result = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                        if (result == WAIT_OBJECT_0)
                            return;
                        if (result != WAIT_OBJECT_0 + 1)
                            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                                    "Process monitor");
                        // Preserve the existing game-exit cursor release; ordinary/failed startup
                        // must not clear another application's cursor constraint.
                        diagnostics::emit(diagnostics::event::runtime_game_exited);
                        ::ClipCursor(nullptr);
                        m_workers.request_stop();
                        m_context.overlay.request_shutdown();
                    });
    const auto start = [this](foundation::thread_group::worker_id id, void (*entry)(workers::control))
    { m_workers.start(id, [this, entry](std::stop_token token) { entry(workers::control{token, m_stop.get()}); }); };
    start(game_state, workers::game);
    start(movement, workers::movement);
    start(combat, workers::combat);
    start(lineup, workers::nade_helper);
#if defined(VESTA_ENABLE_CONSOLE) && VESTA_ENABLE_CONSOLE
    start(watchdog, workers::watchdog);
#endif
#if defined(VESTA_WINDOWED_PROFILE) && VESTA_WINDOWED_PROFILE
    m_workers.start(profile_timeout,
                    [this](std::stop_token token)
                    {
                        const workers::control control{token, m_stop.get()};
                        control.sleep_for(std::chrono::seconds(90));
                        if (!control.stopping())
                            m_context.overlay.request_shutdown();
                    });
#endif
}

void runtime_session::quiesce() noexcept
{
    if (m_quiesced)
        return;
    m_quiesced = true;
    diagnostics::emit(diagnostics::event::runtime_quiesce_begin);
    m_workers.stop_and_join();
    if (m_scripting_constructed)
        scripting::runtime().shutdown();
    if (m_trace_started)
    {
        app::trace_session::shutdown();
        simulation::shot_trace::shutdown();
    }
    m_context.input.disconnect();
    diagnostics::emit(diagnostics::event::runtime_quiesce_end);
}

foundation::shutdown_action runtime_session::quiesce_action() noexcept
{
    return {[](void* owner) noexcept { static_cast<runtime_session*>(owner)->quiesce(); }, this};
}

bool runtime_session::failed() const noexcept
{
    return m_workers.failed_worker() != foundation::thread_group::no_failure;
}
} // namespace app
