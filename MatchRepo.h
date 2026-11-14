#pragma once
#include <Windows.h>
#include <sql.h>
#include <sqlext.h>
#include <iostream>
#include <string>
#include "ResultSink.h"
#include "ConectionPool.h"      // ваш
static void print_odbc_error(SQLSMALLINT handle_type, SQLHANDLE handle)
{
    SQLCHAR sqlstate[6];
    SQLCHAR message[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER native_error;
    SQLSMALLINT message_len;
    SQLSMALLINT i = 1;

    while (SQLGetDiagRecA(handle_type, handle, i, sqlstate, &native_error,
        message, sizeof(message), &message_len) == SQL_SUCCESS)
    {
        std::cerr << "ODBC Error [" << sqlstate << "] " << message << std::endl;
        i++;
    }
}
class MatchRepository
{
public:
    explicit MatchRepository(ConnectionPool& p) : pool(p) {}

    // true: записано; false: ошибка (транзакция откатана)
    bool save(const MatchResultRecord& r)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        SQLHSTMT stmt = nullptr;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt))) return false;

        SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
        const bool good = insert_all(stmt, r);
        SQLEndTran(SQL_HANDLE_DBC, dbc, good ? SQL_COMMIT : SQL_ROLLBACK);
        SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);   // вернуть пулу как было

        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return good;
    }

private:
    bool insert_all(SQLHSTMT stmt, const MatchResultRecord& r)
    {
        std::string guid = r.match_guid;
        SQLLEN guidLen = SQL_NTS;
        SQLBIGINT started = r.started_unix, ended = r.ended_unix, score = r.score;
        SQLINTEGER wave = r.wave, players = static_cast<SQLINTEGER>(r.players.size());

        const char* q1 =
            "INSERT INTO Matches(match_id,started_at,ended_at,wave,score,players) "
            "VALUES(?, DATEADD(SECOND,?,'1970-01-01'), DATEADD(SECOND,?,'1970-01-01'), ?, ?, ?)";
        SQLPrepareA(stmt, (SQLCHAR*)q1, SQL_NTS);
        SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, 32, 0, (SQLPOINTER)guid.c_str(), 0, &guidLen);
        SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &started, 0, nullptr);
        SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &ended, 0, nullptr);
        SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &wave, 0, nullptr);
        SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &score, 0, nullptr);
        SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &players, 0, nullptr);
        if (!SQL_SUCCEEDED(SQLExecute(stmt))) { print_odbc_error(SQL_HANDLE_STMT, stmt); return false; }

        for (const auto& p : r.players)
        {
            SQLFreeStmt(stmt, SQL_CLOSE);
            SQLFreeStmt(stmt, SQL_RESET_PARAMS);

            const char* q2 = "INSERT INTO MatchPlayers(match_id,user_id,points) VALUES(?,?,?)";
            SQLPrepareA(stmt, (SQLCHAR*)q2, SQL_NTS);

            SQLINTEGER uid = p.user_id, pts = p.points;
            SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, 32, 0, (SQLPOINTER)guid.c_str(), 0, &guidLen);
            SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &uid, 0, nullptr);
            SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &pts, 0, nullptr);
            if (!SQL_SUCCEEDED(SQLExecute(stmt))) { print_odbc_error(SQL_HANDLE_STMT, stmt); return false; }
        }
        return true;
    }

    ConnectionPool& pool;
};