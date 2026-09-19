// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "process.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pf_proc {

Child::~Child() {
  // A helper process that outlives its owner would keep the executable locked
  // and leak a coordinator into the next run.
  if (process_ != nullptr) {
    (void)kill_hard();
    int exit_code = 0;
    (void)wait(exit_code);
  }
  close_handles();
}

Child::Child(Child&& other) noexcept : process_(other.process_), pid_(other.pid_) {
  other.process_ = nullptr;
  other.pid_ = 0;
}

Child& Child::operator=(Child&& other) noexcept {
  if (this != &other) {
    close_handles();
    process_ = other.process_;
    pid_ = other.pid_;
    other.process_ = nullptr;
    other.pid_ = 0;
  }
  return *this;
}

void Child::close_handles() {
#if defined(_WIN32)
  if (process_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
#endif
  pid_ = 0;
}

void Child::release() { close_handles(); }

bool Child::spawn(const std::string& executable, const std::vector<std::string>& arguments,
                  Child& out) {
#if defined(_WIN32)
  std::string command_line = "\"" + executable + "\"";
  for (const auto& argument : arguments) {
    command_line += " \"" + argument + "\"";
  }
  std::vector<char> mutable_line(command_line.begin(), command_line.end());
  mutable_line.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  const BOOL created =
      ::CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &info);
  if (created == FALSE) return false;
  ::CloseHandle(info.hThread);
  out.release();
  out.process_ = static_cast<void*>(info.hProcess);
  out.pid_ = static_cast<unsigned long>(info.dwProcessId);
  return true;
#else
  (void)executable;
  (void)arguments;
  (void)out;
  return false;
#endif
}

bool Child::running() const {
#if defined(_WIN32)
  if (process_ == nullptr) return false;
  return ::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
#else
  return false;
#endif
}

bool Child::kill_hard() {
#if defined(_WIN32)
  if (process_ == nullptr) return false;
  return ::TerminateProcess(static_cast<HANDLE>(process_), 0xDEADu) != FALSE;
#else
  return false;
#endif
}

bool Child::wait(int& exit_code) {
#if defined(_WIN32)
  if (process_ == nullptr) return false;
  const DWORD waited = ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  if (waited != WAIT_OBJECT_0) return false;
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(process_), &code) == FALSE) return false;
  exit_code = static_cast<int>(code);
  close_handles();
  return true;
#else
  (void)exit_code;
  return false;
#endif
}

std::uint16_t pick_free_loopback_port() {
#if defined(_WIN32)
  WSADATA data{};
  const bool started = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    if (started) ::WSACleanup();
    return 0;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(s, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
    ::closesocket(s);
    if (started) ::WSACleanup();
    return 0;
  }
  int length = static_cast<int>(sizeof(address));
  if (::getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    ::closesocket(s);
    if (started) ::WSACleanup();
    return 0;
  }
  const std::uint16_t port = ntohs(address.sin_port);
  ::closesocket(s);
  if (started) ::WSACleanup();
  return port;
#else
  return 0;
#endif
}

bool wait_for_file(const std::string& path, unsigned attempts, unsigned delay_ms) {
  for (unsigned attempt = 0; attempt < attempts; ++attempt) {
    std::ifstream probe(path);
    if (probe.good()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }
  return false;
}

bool read_text_file(const std::string& path, std::string& out) {
  std::ifstream in(path);
  if (!in.good()) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  out = buffer.str();
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
    out.pop_back();
  }
  return true;
}

}  // namespace pf_proc
