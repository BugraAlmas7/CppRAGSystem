#define _CRT_SECURE_NO_WARNINGS
#include <cpr/cpr.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <fstream> 
#include "crow.h"
#include <dcommon.h>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <sqlite3.h>

#define SQLITE_ROW
#define SQLITE_TRANSIENT

using namespace std;

std::mutex cache_mutex;

std::unordered_map<std::string, json> chat_sessions;

sqlite3* db;

#DATABASE

void init_db() {
    if (sqlite3_open("data.db", &db)) {
        std::cerr << "Veritabani acilmadi:" << sqlite3_errmsg(db) << std::endl;
        return;
    }
    const char* sql = "CREATE TABLE IF NOT EXISTS chat_history ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "chat_id TEXT, "
        "role TEXT, "
        "content TEXT, "
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP, );";
    char* err_msg = 0;
    sqlite3_exec(db, sql, 0, 0,&err_msg);
}
bool check_user_exists_in_db(const std::string& username, const std::string& email) {
    std::string sql = "SELECT id FROM Users WHERE username = ? OR email = ? LIMIT 1;";
    sqlite3_stmt* stmt;
    bool exists = false;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "SOrgu hatasi (check_user): " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, nullptr);
    sqlite3_bind_text(stmt, 2, email.c_str(), -1, nullptr);
    rc = sqlite3_step(stmt);
    if (rc == 100) {
        exists = true;
    }
    sqlite3_finalize(stmt);
    return exists;
}
void insert_user_to_db(const std::string& username, const std::string& email, const std::string& password) {
    std::string sql = "INSERT INTO Users (username, email, password, coin_bakiye) VALUES (?,?,?,10);";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Kayit hatasi: " << sqlite3_errmsg(db) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, nullptr);
    sqlite3_bind_text(stmt, 2, email.c_str(), -1, nullptr);
    sqlite3_bind_text(stmt, 3, password.c_str(), -1, nullptr);
    rc = sqlite3_step(stmt);
    if (rc != 101) {
        std::cerr << "kayit hatasi: " << sqlite3_errmsg(db) << std::endl;
    }
    else {
        std::cout << "kayit olundu: " << username << "Bakiye: 10 coin" << std::endl;
    }
    sqlite3_finalize(stmt);
}

void add_message_to_db(const std::string& chat_id, const std::string& role, const std::string& content) {
    std::string sql = "INSERT INTO chat_history (chat_id, role, content) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, chat_id.c_str(), -1, (void(*)(void*))-1);
    sqlite3_bind_text(stmt, 2, role.c_str(), -1, (void(*)(void*)) - 1);
    sqlite3_bind_text(stmt, 3, content.c_str(), -1, (void(*)(void*)) - 1);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

json get_recent_chat_history(const std::string& chat_id, int limit = 6) {
    std::string sql = "SELECT role, content FROM (SLECT *FROM chat_history WHERE chat_id = ? ORDER BY created_at DESC LIMIT ?) ORDER BY created_at ASC;";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db,sql.c_str(),-1,&stmt,nullptr);
    sqlite3_bind_text(stmt, 1, chat_id.c_str(), -1, (void(*)(void*)) - 1);
    sqlite3_bind_int(stmt,2,limit);
    json history = json::array();
    while (sqlite3_step(stmt) == 100) {
        std::string role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        history.push_back({
            {"role",role},
            {"parts", {{{"text",content}}}}
            });
    }
    sqlite3_finalize(stmt);
    return history;
}

#API

std::string get_api_key() {
    const char* env_p = std::getenv("GOOGLE_API_KEY");
    if (env_p != nullptr) {
        return std::string(env_p);
    }

    std::ifstream file(".env");
    if (!file.is_open()) {
        std::cerr << "KRITIK HATA: GOOGLE_API_KEY ortam degiskeni yok ve .env dosyasi bulunamadi" << std::endl;
        exit(1);
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto delimiter_pos = line.find('=');
        if (delimiter_pos != std::string::npos) {
            std::string key = line.substr(0, delimiter_pos);
            std::string value = line.substr(delimiter_pos + 1);
            value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
            value.erase(std::remove(value.begin(), value.end(), '\"'), value.end());
            value.erase(std::remove(value.begin(), value.end(), '\''), value.end());
            if (key == "GOOGLE_API_KEY") {
                return value;
            }
        }
    }
    std::cerr << "KRITIK HATA: .env dosyasi bulundu ama icinde GOOGLE_API_KEY yazmiyor!" << std::endl;
    exit(1);
}


std::string ask_gemini(const std::string& chat_id, const std::string& prompt, const std::string& file_body = "", const std::string& mime_type = "") {
    std::string api_key = get_api_key();
    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=" + api_key;

    json contents = json::array();
    if (!chat_id.empty()) {
        contents = get_recent_chat_history(chat_id, 4);
    }

    json current_user_msg = { {"role","user"} };
    if (file_body.empty()) {
        current_user_msg["parts"] = { {{"text",prompt}} };
    }
    else {
        std::string base64_data = crow::utility::base64encode(file_body, file_body.size());
        current_user_msg["parts"] = {
            {{"text", prompt}},
            {{"inline_data",{{"mime_type",mime_type},{"data",base64_data}}}}
        };
    }
    contents.push_back(current_user_msg);

    json request_body = { {"contents", contents} };

    cpr::Response r = cpr::Post(
        cpr::Url{ url },
        cpr::Header{ { "Content-Type","application/json" } },
        cpr::Body{ request_body.dump() }
    );

    if (r.status_code == 200) {
        auto response_json = json::parse(r.text);
        std::string bot_reply = response_json["candidates"][0]["content"]["parts"][0]["text"].get<std::string>();
        if(!chat_id.empty()){
            std::string db_user_msg = prompt + "(Kullanici bir ses/fotograf gonderdi)";
            add_message_to_db(chat_id, "user", db_user_msg);
            add_message_to_db(chat_id, "model", bot_reply);
        }
        return bot_reply;
    }
    else {
        throw std::runtime_error("Gemini API hatasi: " + r.text);
    }
}
int main() {
    crow::SimpleApp app;
    std::thread scheduler_thread(burc_zamanlayici);
    scheduler_thread.detach();

    CROW_ROUTE(app, "/chat-sihirli-ayna").methods(crow::HTTPMethod::Post)([](const crow::request& req) {
        auto data = json::parse(req.body, nullptr, false);
        if (data.is_discarded() || !data.contains("message") || !data.contains("chat_id")) {
            return crow::response(400, "Gecersiz JSON veya eksik parametre");
        }
        std::string user_msg = data["message"];
        std::string chat_id = data["chat_id"];
        std::string system_prompt = "Prompt to RAG agent";//prompt here to what do you wantç
        if (chat_sessions.find(chat_id) == chat_sessions.end()) {
            chat_sessions[chat_id] = json::array();
        }

        add_message_to_db(chat_id, "user", user_msg);

        json chat_history = get_recent_chat_history(chat_id, 6);

        json new_user_msg = {
            {"role", "user"},
            {"parts", {{{"text", user_msg}}}}
        };
        chat_sessions[chat_id].push_back(new_user_msg);

        json gemini_payload = {
            {"system_instruction", {
                {"parts", {{{"text", system_prompt}}}}
            }},
            {"contents", chat_sessions[chat_id]}
        };

        std::string api_key = get_api_key();
        std::string url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=" + api_key;

        cpr::Response r = cpr::Post(
            cpr::Url{ url },
            cpr::Header{ {"Content-Type", "application/json"} },
            cpr::Body{ gemini_payload.dump() }
        );

        if (r.status_code == 200) {
            auto gemini_response = json::parse(r.text);
            try {
                std::string bot_reply = gemini_response["candidates"][0]["content"]["parts"][0]["text"];

                json new_model_msg = {
                    {"role", "model"},
                    {"parts", {{{"text", bot_reply}}}}
                };
                chat_sessions[chat_id].push_back(new_model_msg);

                json final_res = { {"reply", bot_reply} };
                return crow::response(200, final_res.dump());
            }
            catch (const std::exception& e) {
                chat_sessions[chat_id].erase(chat_sessions[chat_id].end() - 1);
                return crow::response(500, "parse hatasi");
            }
        }
        else {
            return crow::response(r.status_code, "api hatasi: " + r.text);
        }
        });
    
    std::cout << "8000 Portunda Calisiyor" << std::endl;
    app.bindaddr("0.0.0.0").port(8000).multithreaded().run();

    return 0;
}
