#include "pipe.hpp"
#include <sddl.h>
#include <stdexcept>

namespace physics_editor {
Pipe::Pipe(const std::wstring& name) {
    // Current logon owner only; remote clients rejected independently below.
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)",SDDL_REVISION_1,&descriptor,nullptr))
        throw std::runtime_error("Cannot create pipe security descriptor");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
    handle_=CreateNamedPipeW((L"\\\\.\\pipe\\"+name).c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,1048576,1048576,0,&security);
    LocalFree(descriptor);
    if(handle_==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create editor pipe (another editor may already use this pipe name)");
}
Pipe::~Pipe() { if(handle_!=INVALID_HANDLE_VALUE) { DisconnectNamedPipe(handle_); CloseHandle(handle_); } }
void Pipe::reset() { DisconnectNamedPipe(handle_); connected_=false; input_.clear(); output_.clear(); sent_=0; }
void Pipe::poll(const std::function<Json(const Json&)>& dispatch) {
    if(!connected_) {
        if(!ConnectNamedPipe(handle_,nullptr) && GetLastError()!=ERROR_PIPE_CONNECTED) return;
        connected_=true; started_=GetTickCount64();
    }
    if(GetTickCount64()-started_>10000) { reset(); return; }
    if(!output_.empty()) {
        if(sent_==output_.size()) {
            DWORD available=0;
            if(!PeekNamedPipe(handle_,nullptr,0,nullptr,&available,nullptr)) reset();
            return;
        }
        DWORD written=0;
        if(!WriteFile(handle_,output_.data()+sent_,static_cast<DWORD>(output_.size()-sent_),&written,nullptr)) {
            reset();
            return;
        }
        sent_+=written;
        // Let the client read before it closes. Never flush a nonblocking server.
        if(sent_==output_.size()) {
            DWORD available=0;
            if(!PeekNamedPipe(handle_,nullptr,0,nullptr,&available,nullptr)) reset();
        }
        return;
    }
    char buffer[8192]; DWORD read=0;
    for(int i=0;i<128;++i) {
        if(!ReadFile(handle_,buffer,sizeof(buffer),&read,nullptr)) {
            const auto error=GetLastError(); if(error==ERROR_BROKEN_PIPE || error==ERROR_PIPE_NOT_CONNECTED) reset();
            break;
        }
        if(!read) break;
        input_.append(buffer,read);
        if(input_.size()>1000000) { reset(); return; }
        const auto end=input_.find('\n');
        if(end!=std::string::npos) {
            Json result;
            try { result={{"ok",true},{"result",dispatch(Json::parse(input_.substr(0,end)))}}; }
            catch(const std::exception& e) { result={{"ok",false},{"error",e.what()}}; }
            output_=result.dump()+"\n";
            if(output_.size()>1000000) output_="{\"ok\":false,\"error\":\"Response exceeds 1 MB\"}\n";
            break;
        }
    }
}
}
