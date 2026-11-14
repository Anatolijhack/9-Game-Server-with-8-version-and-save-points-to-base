#pragma once
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "Instance.h"
#include "ResultSink.h"

constexpr int COUNTDOWN_SEC = 3;
constexpr int RESULTS_SEC = 8;
constexpr std::size_t MAX_NAME = 16; 
constexpr std::uint32_t MAX_ABSENT_TICKS = 90 * TICK_RATE;

enum class PartyState : std::uint8_t { Lobby = 0, Countdown = 1, Playing = 2, Results = 3 };
enum class PRes { Ok, Same, BadState, NotLeader, NotReady, BadTarget };
enum class PEvent { None, Changed, Started, Ended, ToLobby };
static std::string make_guid()
{
    unsigned char b[16];
    if (RAND_bytes(b, sizeof b) != 1) return {};
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) { s.push_back(hex[b[i] >> 4]); s.push_back(hex[b[i] & 15]); }
    return s;     // 32 hex-символа
}
struct Member
{
    std::uint8_t  num = 0;
    PlayerId      conn = 0;
    std::uint32_t joined_tick = 0;
    bool          ready = false;
    std::string   name;
    int           user_id = 0;
    bool          connected = true;
    std::uint32_t lost_until = 0;
    PlayerRecord  rec;
    bool          has_rec = false;
    std::uint32_t lost_at = 0;        // тик, когда потеряли связь
    std::uint32_t absent_ticks = 0;   // сколько уже отсутствовал в этом матче
};

struct MatchResults
{
    std::uint16_t wave = 0;
    std::uint32_t score = 0;
    std::vector<std::pair<std::uint8_t, std::uint16_t>> points;   // номер, очки
};

class Party
{
public:
    explicit Party(std::string code, ThreadPool& pool, IResultSink* sink)
        : code_(std::move(code)), pool_(pool), sink_(sink) {}

    const std::string& code() const { return code_; }
    PartyState state() const { return state_; }
    bool empty() const { return members_.empty(); }
    bool full() const { return members_.size() >= MAX_PARTY; }
    const std::vector<Member>& members() const { return members_; }
    const MatchResults& results() const { return results_; }
    std::uint8_t leader() const
    {
        for (const auto& m : members_) if (m.connected) return m.num;
        return members_.empty() ? 0 : members_.front().num;
    }
    std::uint8_t match_id() const { return match_id_; }
    std::uint8_t countdown_sec() const
    {
        if (state_ != PartyState::Countdown) return 0;
        return static_cast<std::uint8_t>((timer_ + TICK_RATE - 1) / TICK_RATE);
    }

    // 0, если нельзя: нет места, идёт игра, пользователь забанен
    std::uint8_t add_member(PlayerId conn, std::uint32_t tick, int user_id, const std::string& name)
    {
        if (full() || state_ == PartyState::Playing || banned_.count(user_id)) return 0;
        const std::uint8_t num = free_num();
        if (num == 0) return 0;
        members_.push_back({ num, conn, tick, false, sanitize(name), user_id });
        cancel_countdown();   
        std::cout << "[add_member] num=" << int(num) << " members=" << members_.size() << std::endl;            // новичок не готов: старт отменяется
        return num;
    }

    
    bool remove_member(PlayerId conn)
    {
        for (auto it = members_.begin(); it != members_.end(); ++it)
        {
            if (it->conn != conn) continue;
            stash_leaver(*it);
            if (world_) world_->remove_player(world_->get_player(it->num));
            members_.erase(it);
            cancel_countdown();                   // состав изменился (в том числе ушёл лидер)
            return true;
        }
        return false;
    }

    std::uint8_t num_of(PlayerId conn) const
    {
        for (const auto& m : members_) if (m.conn == conn) return m.num;
        return 0;
    }

    PRes set_ready(PlayerId conn, bool ready)
    {
        if (state_ != PartyState::Lobby && state_ != PartyState::Countdown) return PRes::BadState;
        for (auto& m : members_)
        {
            if (m.conn != conn) continue;
            if (m.ready == ready) return PRes::Same;
            m.ready = ready;
            if (!ready) cancel_countdown();
            return PRes::Ok;
        }
        return PRes::BadTarget;
    }

    PRes begin_countdown(PlayerId conn)
    {
        if (num_of(conn) != leader()) return PRes::NotLeader;
        if (state_ != PartyState::Lobby) return PRes::BadState;     // повторный Start игнорируется
        if (!everyone_ready()) return PRes::NotReady;
        state_ = PartyState::Countdown;
        timer_ = COUNTDOWN_SEC * TICK_RATE;
        return PRes::Ok;
    }

    // Кик только в лобби. Участника не удаляет: это делает менеджер через leave.
    PRes kick(PlayerId by, std::uint8_t num, PlayerId& target)
    {
        if (num_of(by) != leader()) return PRes::NotLeader;
        if (state_ != PartyState::Lobby) return PRes::BadState;
        if (num == leader()) return PRes::BadTarget;
        for (const auto& m : members_)
            if (m.num == num)
            {
                banned_.insert(m.user_id);
                target = m.conn;
                return PRes::Ok;
            }
        return PRes::BadTarget;
    }

    void set_input(std::uint8_t num, const InputState& in)
    {
        if (world_) world_->set_input(world_->get_player(num), in);
    }

    PEvent tick()
    {
        switch (state_)
        {
        case PartyState::Countdown:
            if (--timer_ > 0)
                return timer_ % TICK_RATE == 0 ? PEvent::Changed : PEvent::None;
            if (!everyone_ready()) { state_ = PartyState::Lobby; return PEvent::Changed; }
            start_world();
            return PEvent::Started;

        case PartyState::Playing:
            world_->tick();
            if (world_->game_over())
            {
                capture_results();
                if (sink_) sink_->save(build_record());
                state_ = PartyState::Results;
                timer_ = RESULTS_SEC * TICK_RATE;
                return PEvent::Ended;
            }
            return PEvent::None;

        case PartyState::Results:
            if (--timer_ > 0) return PEvent::None;
            world_.reset();          // уничтожаем мир здесь
            for (auto& m : members_) m.ready = false;
            state_ = PartyState::Lobby;
            return PEvent::ToLobby;

        default:
            return PEvent::None;
        }
    }

    void write_world(proto::Writer& w) const { if (world_) world_->write_world(w); }

    bool mark_lost(PlayerId conn, std::uint32_t tick)
    {
        for (auto it = members_.begin(); it != members_.end(); ++it)
        {
            if (!it->connected || it->conn != conn) continue;

            std::uint32_t win = (state_ == PartyState::Playing ? 30 : 10) * TICK_RATE;
            if (state_ == PartyState::Playing)
            {
                if (it->absent_ticks >= MAX_ABSENT_TICKS)
                {
                    remove_member(conn);               // стирает из мира и списка, отменяет отсчёт
                    return false;
                }
                win = std::min(win, MAX_ABSENT_TICKS - it->absent_ticks);
            }

            it->connected = false;
            it->conn = 0;
            it->ready = false;
            it->lost_at = tick;
            it->lost_until = tick + win;

            if (state_ == PartyState::Playing && world_ && world_->detach_player(it->num, it->rec))
            {
                it->rec.match_id = match_id_;
                it->has_rec = true;
            }
            cancel_countdown();
            return true;
        }
        return false;
    }

    // true, если кого-то убрали по истечении окна
    bool expire(std::uint32_t tick)
    {
        bool changed = false;
        for (auto it = members_.begin(); it != members_.end();)
        {
            if (!it->connected && tick >= it->lost_until) { stash_leaver(*it); it = members_.erase(it); changed = true; }
            else ++it;
        }
        if (changed) cancel_countdown();
        return changed;
    }

    int connected_count() const
    {
        int n = 0;
        for (const auto& m : members_) if (m.connected) ++n;
        return n;
    }

    bool has_reserved(int user_id) const
    {
        for (const auto& m : members_)
            if (!m.connected && m.user_id == user_id) return true;
        return false;
    }

    // Возвращает номер участника или 0, если резервации нет
    std::uint8_t reattach(int user_id, PlayerId conn, std::uint32_t tick)
    {
        for (auto it = members_.begin(); it != members_.end(); ++it)
        {
            if (it->connected || it->user_id != user_id) continue;

            Member m = std::move(*it);
            members_.erase(it);
            m.absent_ticks += tick - m.lost_at;
            m.connected = true;
            m.conn = conn;
            m.lost_until = 0;
            m.ready = false;

            if (state_ == PartyState::Playing && world_)
            {
                if (m.has_rec && m.rec.match_id == match_id_) world_->attach_player(m.num, m.rec);
                else                                           world_->add_player(m.num);
            }
            m.has_rec = false;

            const std::uint8_t num = m.num;
            members_.push_back(std::move(m));     // в конец: вернувшийся лидер не отбирает роль
            cancel_countdown();
            return num;
        }
        return 0;
    }

    std::uint8_t rebind(int user_id, PlayerId old_conn, PlayerId new_conn)
    {
        for (auto& m : members_)
            if (m.connected && m.user_id == user_id && m.conn == old_conn)
            {
                m.conn = new_conn;
                m.ready = false;
                return m.num;
            }
        return 0;
    }


private:
    MatchResultRecord build_record() const
    {
        MatchResultRecord r;
        r.match_guid = match_guid_;
        r.started_unix = started_unix_;
        r.ended_unix = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        r.wave = results_.wave;
        r.score = results_.score;
        for (const auto& m : members_)
            for (const auto& pt : results_.points)
                if (pt.first == m.num)
                    r.players.push_back({ m.user_id, pt.second });

        for (const auto& l : left_)
            r.players.push_back(l);
        return r;
    }

    bool everyone_ready() const
    {
        if (members_.empty()) return false;
        const std::uint8_t lead = leader();
        for (const auto& m : members_)
        {
            if (!m.connected) return false;
            if (m.num != lead && !m.ready) return false;
        }
        return true;
    }

    void cancel_countdown() { if (state_ == PartyState::Countdown) { std::cout << "[party " << code_ << "] countdown CANCELLED" << std::endl; state_ = PartyState::Lobby; }; }

    void start_world()
    {
        left_.clear();
        match_guid_ = make_guid();
        started_unix_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        ++match_id_;                      // переполнение u8 допустимо: важно лишь, что номер сменился
        world_ = std::make_unique<Instance>(pool_);
        for (const auto& m : members_) world_->add_player(m.num);
        for (auto& m : members_) { m.has_rec = false; m.absent_ticks = 0; }
        state_ = PartyState::Playing;
    }

    void capture_results()
    {
        results_ = {};
        results_.wave = static_cast<std::uint16_t>(std::clamp(world_->wave(), 0, 65535));
        results_.score = static_cast<std::uint32_t>(std::max(world_->score(), 0));
        for (const auto& m : members_)
            std::cout << "[capture] num=" << int(m.num)
            << " points=" << world_->points_of(m.num) << std::endl;
        for (const auto& m : members_)
            results_.points.push_back({ m.num,
                static_cast<std::uint16_t>(std::clamp(world_->points_of(m.num), 0, 65535)) });
    }

    void stash_leaver(const Member& m)
    {
        if (state_ != PartyState::Playing) return;
        MatchPlayerResult r;
        r.user_id = m.user_id;
        if (m.connected && world_)
            r.points = static_cast<std::uint16_t>(std::clamp(world_->points_of(m.num), 0, 65535));
        else if (m.has_rec)
            r.points = static_cast<std::uint16_t>(std::clamp(m.rec.points, 0, 65535));
        left_.push_back(r);
    }
    static std::string sanitize(const std::string& s)
    {
        std::string out;
        for (char c : s)
        {
            if (out.size() >= MAX_NAME) break;
            const unsigned char u = static_cast<unsigned char>(c);
            out.push_back((u >= 32 && u < 127) ? c : '?');
        }
        return out;
    }

    std::uint8_t free_num() const
    {
        for (std::uint8_t n = 1; n <= MAX_PARTY; ++n)
        {
            bool used = false;
            for (const auto& m : members_) if (m.num == n) { used = true; break; }
            if (!used) return n;
        }
        return 0;
    }

    std::string code_;
    std::vector<Member> members_;
    std::set<int> banned_;
    std::unique_ptr<Instance> world_;
    IResultSink* sink_ = nullptr;
    std::string  match_guid_;
    std::int64_t started_unix_ = 0;
    PartyState state_ = PartyState::Lobby;
    int timer_ = 0;
    MatchResults results_;
    ThreadPool& pool_;
    std::uint8_t match_id_ = 0;
    std::vector<MatchPlayerResult> left_;

};