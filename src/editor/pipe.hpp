#pragma once
#include "document.hpp"
#include <windows.h>

namespace physics_editor {
// One UI-thread-owned service: no concurrent scene mutation and no blocking
// pipe calls in the render loop. One request per client connection.
class Pipe {
public:
    explicit Pipe(const std::wstring& name);
    ~Pipe();
    Pipe(const Pipe&)=delete;
    Pipe& operator=(const Pipe&)=delete;
    void poll(const std::function<Json(const Json&)>& dispatch);
private:
    HANDLE handle_=INVALID_HANDLE_VALUE;
    std::string input_, output_;
    size_t sent_=0;
    ULONGLONG started_=0;
    bool connected_=false;
    void reset();
};
}
