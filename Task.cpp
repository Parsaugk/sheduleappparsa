// Task.cpp - task status logic and CSV persistence (UTF-8 with BOM)
#include "Task.h"

#include <shlobj.h>
#include <cwchar>
#include <fstream>
#include <iterator>

#pragma comment(lib, "shell32.lib")

namespace
{
    int DateKey(const SYSTEMTIME& st)
    {
        return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
    }

    std::string ToUtf8(const std::wstring& s)
    {
        if (s.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
        return out;
    }

    std::wstring FromUtf8(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
        return out;
    }

    std::wstring Quote(const std::wstring& s)
    {
        std::wstring out = L"\"";
        for (wchar_t c : s)
        {
            if (c == L'\r' || c == L'\n') { out += L' '; continue; }
            if (c == L'"') out += L'"';
            out += c;
        }
        out += L'"';
        return out;
    }

    // Splits one CSV line into fields; handles "quoted, fields" and "" escapes.
    bool ParseCsvLine(const std::wstring& line, std::vector<std::wstring>& fields)
    {
        fields.clear();
        std::wstring cur;
        bool inQuotes = false;
        for (size_t i = 0; i < line.size(); ++i)
        {
            wchar_t c = line[i];
            if (inQuotes)
            {
                if (c == L'"')
                {
                    if (i + 1 < line.size() && line[i + 1] == L'"') { cur += L'"'; ++i; }
                    else inQuotes = false;
                }
                else cur += c;
            }
            else if (c == L'"') inQuotes = true;
            else if (c == L',') { fields.push_back(cur); cur.clear(); }
            else cur += c;
        }
        fields.push_back(cur);
        return !inQuotes;
    }

    bool ParseDue(const std::wstring& text, SYSTEMTIME& out)
    {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        if (swscanf_s(text.c_str(), L"%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return false;
        if (y < 1601 || y > 9999 || h < 0 || h > 23 || mi < 0 || mi > 59) return false;

        SYSTEMTIME st{};
        st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d;
        st.wHour = (WORD)h; st.wMinute = (WORD)mi;

        FILETIME ft;
        if (!SystemTimeToFileTime(&st, &ft)) return false; // rejects invalid dates
        out = st;
        return true;
    }
}

int ClampProgress(long value)
{
    if (value < 0) return 0;
    if (value > 100) return 100;
    return (int)value;
}

TaskStatus ComputeStatus(const Task& t)
{
    if (t.progress >= 100) return TaskStatus::Complete;

    SYSTEMTIME now;
    GetLocalTime(&now);
    if (DateKey(t.due) < DateKey(now)) return TaskStatus::Overdue;

    if (t.progress > 0) return TaskStatus::InProgress;
    return TaskStatus::NotStarted;
}

const wchar_t* StatusText(TaskStatus s)
{
    switch (s)
    {
    case TaskStatus::Complete:   return L"Complete";
    case TaskStatus::Overdue:    return L"Overdue";
    case TaskStatus::InProgress: return L"In Progress";
    default:                     return L"Not Started";
    }
}

std::wstring FormatDue(const SYSTEMTIME& st)
{
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d",
               (int)st.wYear, (int)st.wMonth, (int)st.wDay, (int)st.wHour, (int)st.wMinute);
    return buf;
}

std::wstring GetDataFilePath()
{
    wchar_t base[MAX_PATH] = {};
    std::wstring dir;

    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base)))
    {
        dir = std::wstring(base) + L"\\ScheduleProgressTracker";
        CreateDirectoryW(dir.c_str(), nullptr); // fine if it already exists
    }
    else
    {
        // Fallback: next to the executable.
        GetModuleFileNameW(nullptr, base, MAX_PATH);
        dir = base;
        size_t slash = dir.find_last_of(L"\\/");
        dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);
    }
    return dir + L"\\tasks.csv";
}

bool LoadTasks(std::vector<Task>& tasks, const std::wstring& path)
{
    tasks.clear();

    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in)
        return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES; // missing file is OK

    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::wstring text = FromUtf8(raw);
    if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1); // strip BOM

    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, (nl == std::wstring::npos) ? std::wstring::npos : nl - pos);
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;

        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.pop_back();
        if (line.empty()) continue;
        if (line.rfind(L"Title,", 0) == 0) continue; // header (titles are always quoted)

        std::vector<std::wstring> f;
        if (!ParseCsvLine(line, f) || f.size() < 3) continue; // skip malformed lines

        Task t;
        t.title = f[0];
        if (t.title.empty() || !ParseDue(f[1], t.due)) continue;
        t.progress = ClampProgress(wcstol(f[2].c_str(), nullptr, 10));
        tasks.push_back(t);
    }
    return true;
}

bool SaveTasks(const std::vector<Task>& tasks, const std::wstring& path)
{
    std::wstring text = L"Title,Due,Progress\r\n";
    for (const Task& t : tasks)
    {
        text += Quote(t.title);
        text += L',';
        text += FormatDue(t.due);
        text += L',';
        text += std::to_wstring(t.progress);
        text += L"\r\n";
    }

    // Write to a temp file, then replace the real file (avoids corrupting it on failure).
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) return false;
        static const char bom[] = { (char)0xEF, (char)0xBB, (char)0xBF };
        out.write(bom, sizeof(bom));
        std::string utf8 = ToUtf8(text);
        out.write(utf8.data(), (std::streamsize)utf8.size());
        out.flush();
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
