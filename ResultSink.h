#pragma once
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include "MatchRepo.h"

struct MatchPlayerResult
{
    int           user_id = 0;
    std::uint16_t points = 0;
};

struct MatchResultRecord
{
    std::string   match_guid;          // создаётся при старте матча
    std::int64_t  started_unix = 0;
    std::int64_t  ended_unix = 0;
    std::uint16_t wave = 0;
    std::uint32_t score = 0;
    std::vector<MatchPlayerResult> players;
};

struct IResultSink
{
    virtual void save(MatchResultRecord r) = 0;
    virtual ~IResultSink() = default;
};

// Для проверки без базы: просто печатает
struct LogSink : IResultSink
{
    void save(MatchResultRecord r) override
    {
        std::cout << "[result] " << r.match_guid
            << " wave=" << r.wave << " score=" << r.score
            << " duration=" << (r.ended_unix - r.started_unix) << "s players:";
        for (const auto& p : r.players)
            std::cout << " (user " << p.user_id << ": " << p.points << ")";
        std::cout << std::endl;
    }
};

class DbSink : public IResultSink
{
public:
    DbSink(ThreadPool& workers, MatchRepository& repo) : workers_(workers), repo_(repo) {}

    void save(MatchResultRecord r) override
    {
        const std::string guid = r.match_guid;
        const bool queued = workers_.try_push_task(
            [this, rec = std::move(r)]()
            {
                if (!repo_.save(rec))
                    std::cerr << "[db] FAILED to save match " << rec.match_guid << std::endl;
            }, 0);      // приоритет: подставьте свой

        if (!queued)
            std::cerr << "[db] queue full, match lost: " << guid << std::endl;   // позже: очередь повторов
    }

private:
    ThreadPool& workers_;
    MatchRepository& repo_;
};