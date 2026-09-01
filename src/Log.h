#pragma once
#include <windows.h>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>

class Log {
public:
    static Log& Get() { static Log l; return l; }

    void Write(const std::string& s) {
        std::lock_guard<std::mutex> lock(m_mutex);
        SYSTEMTIME st{}; GetLocalTime(&st);
        std::ostringstream line;
        line << '[' << std::setfill('0') << std::setw(2) << st.wHour << ':'
             << std::setw(2) << st.wMinute << ':' << std::setw(2) << st.wSecond
             << '.' << std::setw(3) << st.wMilliseconds << "] " << s << "\n";
        OutputDebugStringA(line.str().c_str());
        m_file << line.str();
        m_file.flush();
        m_history.push_back(line.str());
        if (m_history.size() > 500) m_history.erase(m_history.begin());
    }
    std::vector<std::string> GetHistory() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_history;
    }
private:
    Log() : m_file("DLSSImageViewer.log", std::ios::out | std::ios::trunc) {}
    std::ofstream m_file;
    std::mutex m_mutex;
    std::vector<std::string> m_history;
};

#define LOG(x) do { std::ostringstream _oss; _oss << x; Log::Get().Write(_oss.str()); } while(0)
