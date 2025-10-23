/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Common.h"
#include "Config.h"
#include "Duration.h"
#include "GameEventMgr.h"
#include "Language.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "ServerAutoShutdown.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "TaskScheduler.h"
#include "Tokenize.h"
#include "Util.h"
#include "World.h"
#include "WorldSessionMgr.h"

namespace
{
    // Scheduler - for update
    TaskScheduler scheduler;

    time_t GetNextResetTime(time_t time, uint32 restartDays, uint8 restartHour, uint8 restartMinute, uint8 restartSecond)
    {
        tm timeLocal = Acore::Time::TimeBreakdown(time);
        timeLocal.tm_hour = restartHour;
        timeLocal.tm_min = restartMinute;
        timeLocal.tm_sec = restartSecond;

        time_t midnightLocal = mktime(&timeLocal);

        if (restartDays > 1 || midnightLocal <= time)
            midnightLocal += DAY * restartDays;

        return midnightLocal;
    }

    // Returns the next time_t for the given weekday (0=Sunday, 1=Monday, ..., 6=Saturday) at the given hour/min/sec
    time_t GetNextWeekdayTime(time_t now, int weekday, uint8 restartHour, uint8 restartMinute, uint8 restartSecond)
    {
        tm timeLocal = Acore::Time::TimeBreakdown(now);
        int currentWeekday = timeLocal.tm_wday;
        int daysUntil = (weekday - currentWeekday + 7) % 7;
        if (daysUntil == 0)
        {
            // If today, check if the time has already passed
            if (timeLocal.tm_hour > restartHour ||
                (timeLocal.tm_hour == restartHour && timeLocal.tm_min > restartMinute) ||
                (timeLocal.tm_hour == restartHour && timeLocal.tm_min == restartMinute && timeLocal.tm_sec >= restartSecond))
            {
                daysUntil = 7;
            }
        }
        timeLocal.tm_mday += daysUntil;
        timeLocal.tm_hour = restartHour;
        timeLocal.tm_min = restartMinute;
        timeLocal.tm_sec = restartSecond;
        time_t result = mktime(&timeLocal);
        return result;
    }
}

/*static*/ ServerAutoShutdown* ServerAutoShutdown::instance()
{
    static ServerAutoShutdown instance;
    return &instance;
}

void ServerAutoShutdown::Init()
{
    _isEnableModule = sConfigMgr->GetOption<bool>("ServerAutoShutdown.Enabled", false);

    if (!_isEnableModule)
        return;

    std::string configTime = sConfigMgr->GetOption<std::string>("ServerAutoShutdown.Time", "04:00:00");
    auto const& tokens = Acore::Tokenize(configTime, ':', false);

    if (tokens.size() != 3)
    {
        LOG_ERROR("module", "> ServerAutoShutdown: Incorrect time in config option 'ServerAutoShutdown.Time' - '{}'", configTime);
        _isEnableModule = false;
        return;
    }

    // Check convert to int
    auto CheckTime = [tokens](std::initializer_list<uint8> index)
    {
        for (auto const& itr : index)
            if (!Acore::StringTo<uint8>(tokens.at(itr)))
                return false;

        return true;
    };

    if (!CheckTime({ 0, 1, 2 }))
    {
        LOG_ERROR("module", "> ServerAutoShutdown: Incorrect time in config option 'ServerAutoShutdown.Time' - '{}'", configTime);
        _isEnableModule = false;
        return;
    }

    int weekday = sConfigMgr->GetOption<int>("ServerAutoShutdown.Weekday", -1);
    uint32 restartDays =  sConfigMgr->GetOption<uint32>("ServerAutoShutdown.EveryDays", 1);
    uint8 restartHour = *Acore::StringTo<uint8>(tokens.at(0));
    uint8 restartMinute = *Acore::StringTo<uint8>(tokens.at(1));
    uint8 restartSecond = *Acore::StringTo<uint8>(tokens.at(2));

    auto nowTime = time(nullptr);
    uint64 nextResetTime = 0;

    if (weekday >= 0 && weekday <= 6)
    {
        nextResetTime = GetNextWeekdayTime(nowTime, weekday, restartHour, restartMinute, restartSecond);
    }
    else
    {
        if (restartDays < 1 || restartDays > 365)
        {
            LOG_ERROR("module", "> ServerAutoShutdown: Incorrect day in config option 'ServerAutoShutdown.EveryDays' - '{}'", restartDays);
            _isEnableModule = false;
            return;
        }
        nextResetTime = GetNextResetTime(nowTime, restartDays, restartHour, restartMinute, restartSecond);
    }

    if (restartDays < 1 || restartDays > 365)
    {
        LOG_ERROR("module", "> ServerAutoShutdown: Incorrect day in config option 'ServerAutoShutdown.EveryDays' - '{}'", restartDays);
        _isEnableModule = false;
    }
    else if (restartHour > 23)
    {
        LOG_ERROR("module", "> ServerAutoShutdown: Incorrect hour in config option 'ServerAutoShutdown.Time' - '{}'", configTime);
        _isEnableModule = false;
    }
    else if (restartMinute >= 60)
    {
        LOG_ERROR("module", "> ServerAutoShutdown: Incorrect minute in config option 'ServerAutoShutdown.Time' - '{}'", configTime);
        _isEnableModule = false;
    }
    else if (restartSecond >= 60)
    {
        LOG_ERROR("module", "> ServerAutoShutdown: Incorrect second in config option 'ServerAutoShutdown.Time' - '{}'", configTime);
        _isEnableModule = false;
    }

    uint32 diffToShutdown = nextResetTime - static_cast<uint32>(nowTime);

    if (diffToShutdown < 10)
    {
        LOG_WARN("module", "> ServerAutoShutdown: Next time to shutdown < 10 seconds, Set next period");
        if (weekday >= 0 && weekday <= 6)
            nextResetTime += WEEK;
        else
            nextResetTime += DAY * restartDays;
        diffToShutdown = nextResetTime - static_cast<uint32>(nowTime);
    }

    LOG_INFO("module", " ");
    LOG_INFO("module","> ServerAutoShutdown: System loading");

    // Cancel all task for support reload config
    scheduler.CancelAll();
    sWorld->ShutdownCancel();

    LOG_INFO("module", "> ServerAutoShutdown: Next time to shutdown - {}", Acore::Time::TimeToHumanReadable(Seconds(nextResetTime)));
    LOG_INFO("module", "> ServerAutoShutdown: Remaining time to shutdown - {}", Acore::Time::ToTimeString<Seconds>(diffToShutdown));
    LOG_INFO("module", " ");

    uint32 preAnnounceSeconds = sConfigMgr->GetOption<uint32>("ServerAutoShutdown.PreAnnounce.Seconds", HOUR);
    if (preAnnounceSeconds > DAY)
    {
        LOG_ERROR("module", "> ServerAutoShutdown: Ahah, how could this happen? Time to preannouce has been set to more than 1 day? ({}). Change to 1 hour (3600)", preAnnounceSeconds);
        preAnnounceSeconds = HOUR;
    }

    uint32 timeToPreAnnounce = static_cast<uint32>(nextResetTime) - preAnnounceSeconds;
    uint32 diffToPreAnnounce = timeToPreAnnounce - static_cast<uint32>(nowTime);

    // Ingnore pre announce time and set is left
    if (diffToShutdown < preAnnounceSeconds)
    {
        timeToPreAnnounce = static_cast<uint32>(nowTime) + 1;
        diffToPreAnnounce = 1;
        preAnnounceSeconds = diffToShutdown;
    }

    LOG_INFO("module", "> ServerAutoShutdown: Next time to pre annouce - {}", Acore::Time::TimeToHumanReadable(Seconds(timeToPreAnnounce)));
    LOG_INFO("module", "> ServerAutoShutdown: Remaining time to pre annouce - {}", Acore::Time::ToTimeString<Seconds>(diffToPreAnnounce));
    LOG_INFO("module", " ");

    StartPersistentGameEvents();

    std::string action = sConfigMgr->GetOption<std::string>("ServerAutoShutdown.Action", "restart");
    if (action == "shutdown")
    {
        _shutdownMask = SHUTDOWN_MASK_IDLE;
    }
    else
    {
        _shutdownMask = SHUTDOWN_MASK_RESTART;
    }

    // Add task for pre shutdown announce
    scheduler.Schedule(Seconds(diffToPreAnnounce), [this, preAnnounceSeconds](TaskContext /*context*/)
    {
        std::string preAnnounceMessageFormat = sConfigMgr->GetOption<std::string>("ServerAutoShutdown.PreAnnounce.Message", "[SERVER]: Automated server restart(shutdown) in {}");
        std::string message = Acore::StringFormat(preAnnounceMessageFormat, Acore::Time::ToTimeString<Seconds>(preAnnounceSeconds, TimeOutput::Seconds, TimeFormat::FullText));

        LOG_INFO("module", "> {}", message);

        sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING, message);
        sWorld->ShutdownServ(preAnnounceSeconds, _shutdownMask, SHUTDOWN_EXIT_CODE);
    });
}

void ServerAutoShutdown::OnUpdate(uint32 diff)
{
    // If module disable, why do the update? hah
    if (!_isEnableModule)
        return;

    scheduler.Update(diff);
}

void ServerAutoShutdown::StartPersistentGameEvents()
{
    std::string eventList = sConfigMgr->GetOption<std::string>("ServerAutoShutdown.StartEvents", "");

    std::vector<std::string_view> tokens = Acore::Tokenize(eventList, ' ', false);
    GameEventMgr::GameEventDataMap const& events = sGameEventMgr->GetEventMap();

    for (auto token : tokens)
    {
        if (token.empty())
            continue;

        uint32 eventId = *Acore::StringTo<uint32>(token);
        sGameEventMgr->StartEvent(eventId);

        GameEventData const& eventData = events[eventId];
        LOG_INFO("module", "> ServerAutoShutdown: Starting event {} ({}).", eventData.Description, eventId);
    }
}
