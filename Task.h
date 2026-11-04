// Task.h - task data model, status logic and CSV persistence
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <vector>

enum class TaskStatus { NotStarted, InProgress, Complete, Overdue };

struct Task
{
    std::wstring title;
    SYSTEMTIME   due{};        // local date + time (seconds are always 0)
    int          progress = 0; // 0..100
};

// Clamp to 0..100.
int ClampProgress(long value);

// Complete if 100; Overdue if due date < today and < 100;
// In Progress if > 0; otherwise Not Started.
TaskStatus ComputeStatus(const Task& t);
const wchar_t* StatusText(TaskStatus s);

// "yyyy-MM-dd HH:mm"
std::wstring FormatDue(const SYSTEMTIME& st);

// %APPDATA%\ScheduleProgressTracker\tasks.csv (directory is created if needed).
// CHANGE THIS FUNCTION to store the file somewhere else.
std::wstring GetDataFilePath();

// Load returns true if the file loaded OR does not exist yet.
bool LoadTasks(std::vector<Task>& tasks, const std::wstring& path);
bool SaveTasks(const std::vector<Task>& tasks, const std::wstring& path);
