#pragma once
// Live control of the running editor from another process (gameenginetools\engine_mcp\tbd_mcp.py, which gives Claude these as MCP
// tools). A named pipe, \\.\pipe\tbd_engine (TBD_REMOTE_PIPE overrides the name), local to this machine by nature: one request per
// connection, a line "command|arg|arg...", answered with "OK\n<text>" or "ERR\n<reason>". The pipe thread only queues; the editor runs
// each request on its own thread between frames (RemotePump in main.cpp), so commands see and change the scene exactly as the UI does.
#include <windows.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tbd::remote {

struct Request {
    std::vector<std::string> args; // args[0] is the command
    std::string reply;
    bool done = false;
    bool taken = false; // the editor holds it
};

class Server {
public:
    void Start() {
        if (thread_.joinable()) return;
        char name[128] = "\\\\.\\pipe\\tbd_engine";
        char custom[96] = {};
        if (GetEnvironmentVariableA("TBD_REMOTE_PIPE", custom, sizeof(custom)) > 0) snprintf(name, sizeof(name), "\\\\.\\pipe\\%s", custom);
        name_ = name;
        thread_ = std::thread([this] { Run(); });
        thread_.detach(); // lives as long as the process; ExitProcess ends it
    }

    // Main thread: the oldest waiting request, or null.
    Request* Next() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Request* r : queue_)
            if (!r->done && !r->taken) { r->taken = true; return r; }
        return nullptr;
    }
    // Main thread: answers a request taken with Next (now or frames later).
    void Finish(Request* r, bool ok, const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            r->reply = (ok ? "OK\n" : "ERR\n") + text;
            r->done = true;
        }
        cv_.notify_all();
    }

private:
    using Queued = Request;
    std::string name_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Queued*> queue_;

    static std::vector<std::string> Split(const std::string& line) {
        std::vector<std::string> out(1);
        for (char c : line) {
            if (c == '|') out.emplace_back();
            else if (c != '\r' && c != '\n') out.back() += c;
        }
        return out;
    }

    void Run() {
        for (;;) {
            HANDLE pipe = CreateNamedPipeA(name_.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                           1, 1 << 16, 1 << 16, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
            if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) Serve(pipe);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }

    void Serve(HANDLE pipe) {
        std::string line;
        char buf[4096];
        DWORD got = 0;
        while (line.find('\n') == std::string::npos && ReadFile(pipe, buf, sizeof(buf), &got, nullptr) && got) line.append(buf, got);
        if (line.empty()) return;
        Queued q;
        q.args = Split(line.substr(0, line.find('\n')));
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.push_back(&q);
        // A frame or a scene load can take a while; a screenshot waits for its frame. Two minutes covers a big scene opening.
        // Once the editor has taken it, wait for its answer however long: it holds a pointer to q.
        const bool answered = cv_.wait_for(lock, std::chrono::seconds(120), [&] { return q.done; });
        if (!answered && q.taken) cv_.wait(lock, [&] { return q.done; });
        else if (!answered) q.reply = "ERR\nthe editor did not take the request in 120 s (busy loading, or a modal dialog is open)";
        for (auto it = queue_.begin(); it != queue_.end(); ++it)
            if (*it == &q) { queue_.erase(it); break; }
        lock.unlock();
        DWORD wrote = 0;
        WriteFile(pipe, q.reply.data(), (DWORD)q.reply.size(), &wrote, nullptr);
    }
};

inline Server& Instance() { static Server s; return s; }

} // namespace tbd::remote
