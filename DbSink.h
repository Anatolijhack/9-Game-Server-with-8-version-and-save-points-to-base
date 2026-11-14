// DbSink.h
#pragma once
#include "ResultSink.h"
#include "MatchRepo.h"
#include "ThreadPool.h"     // ваш заголовок пула

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
            }, 0);
        if (!queued)
            std::cerr << "[db] queue full, match lost: " << guid << std::endl;
    }

private:
    ThreadPool& workers_;
    MatchRepository& repo_;
};