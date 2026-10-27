#include <stdafx.hpp>
#include <game/state/services.hpp>
#include <foundation/identity.hpp>
#include <render/presentation.hpp>
#include <config/settings.hpp>
#include <app/context.hpp>
#include <render/fonts.hpp>
#include <game/memory/bound_services.hpp>
#include <config/misc.hpp>
#include <simulation/collision.hpp>
#include <app/trace_session.hpp>
#include <simulation/shot_trace.hpp>
#include <game/state/level.hpp>
#include <nlohmann/json.hpp>
#include <fstream>

namespace app::trace_session
{
namespace
{
std::jthread sampler;
void save(const std::filesystem::path& path, const nlohmann::json& value)
{
    std::ofstream file(path, std::ios::trunc);
    file << value.dump(2) << '\n';
}
} // namespace
void initialize()
{
    if (!simulation::shot_trace::enabled())
        return;
    wchar_t directory[32768]{};
    const auto size = GetEnvironmentVariableW(L"VESTA_SEED_TRACE_DIR", directory, std::size(directory));
    if (!size || size >= std::size(directory))
        return;
    const auto root = std::filesystem::path(directory).parent_path();
    try
    {
        std::filesystem::create_directories(root);
        save(root / "config-at-launch.json", config::build_config_json());
        save(root / "schema.json",
             {{"bullet_services", SCHEMA("C_CSPlayerPawn", "m_pBulletServices"_id)},
              {"server_hits", SCHEMA("CCSPlayer_BulletServices", "m_totalHitsOnServer"_id)},
              {"action_services", SCHEMA("CCSPlayerController", "m_pActionTrackingServices"_id)},
              {"round_damage", SCHEMA("CCSPlayerController_ActionTrackingServices", "m_flTotalRoundDamageDealt"_id)},
              {"round_kills", SCHEMA("CCSPlayerController_ActionTrackingServices", "m_iNumRoundKills"_id)},
              {"health", SCHEMA("C_BaseEntity", "m_iHealth"_id)},
              {"shots_fired", SCHEMA("C_CSPlayerPawn", "m_iShotsFired"_id)},
              {"simulation_tick", SCHEMA("C_BaseEntity", "m_nSimulationTick"_id)},
              {"view", SCHEMA("C_BasePlayerPawn", "v_angle"_id)},
              {"ping", SCHEMA("CCSPlayerController", "m_iPing"_id)}});
        sampler = std::jthread(
            [root](std::stop_token stop)
            {
                LARGE_INTEGER frequency{};
                QueryPerformanceFrequency(&frequency);
                std::ofstream file(root / "scene.jsonl", std::ios::app);
                while (!stop.stop_requested() && file)
                {
                    try
                    {
                        if (std::filesystem::exists(root / "STOP"))
                            break;
                        const auto map = game::level().name();
                        const nlohmann::json row{
                            {"event", "scene"},
                            {"qpc", simulation::shot_trace::qpc_now()},
                            {"qpc_frequency", frequency.QuadPart},
                            {"pid", GetCurrentProcessId()},
                            {"game_pid", game::bound_memory().process.process_id()},
                            {"map", map ? *map : ""},
                            {"collision_ready", simulation::collision().valid()},
                            {"menu_open", render::menu_open()},
                            {"combat_input_ready", render::combat_input_ready()}};
                        file << row.dump() << '\n';
                        file.flush();
                    }
                    catch (...)
                    {
                        diagnostics::process_log().warning("[trace] scene sampler stopped: {}",
                                                           diagnostics::current_exception_message());
                        break;
                    }
                    for (int i = 0; i < 10 && !stop.stop_requested(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
    }
    catch (...)
    {
        // The trace session is diagnostics only; a failure never changes a firing decision.
        diagnostics::process_log().warning("[trace] session not started: {}", diagnostics::current_exception_message());
    }
}
void shutdown() noexcept
{
    if (sampler.joinable())
    {
        sampler.request_stop();
        sampler.join();
    }
}
} // namespace app::trace_session
