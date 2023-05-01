#include "server.hh"
#include "util.hh"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

using namespace std::literals;

static bool g_interrupted = false;

static void sigint(int s){
  g_interrupted = true;
}

struct sigint_sentinel {
  sigint_sentinel(sigint_sentinel &) = delete;
  sigint_sentinel(sigint_sentinel &&) = delete;

  sigint_sentinel(){
    g_interrupted = false;
    signal(SIGINT, sigint);
  }
  
  ~sigint_sentinel(){
    signal(SIGKILL, nullptr);
  }

  bool signalled() const{
    return g_interrupted;
  }
};


bool read_http_request(int connfd, std::vector<char> & client_buffer) {
  const size_t READ_SZ = 1024;
  client_buffer.clear();
  ssize_t read = 0;
  do{
    auto pre_sz = client_buffer.size();
    client_buffer.resize(pre_sz + READ_SZ);
    read = recv(connfd, &client_buffer[pre_sz], READ_SZ, 0);
    client_buffer.resize(pre_sz + std::max(ssize_t(0), read));
    if (read < 0){
      return false;
    }
  } while(read == READ_SZ);
  return true;
}

void simplify_http_request(std::vector<char> & req) {
  auto wanted_host = "\x0d\x0aHost: localhost"sv;
  auto host_start = std::ranges::search(req, "\x0d\x0aHost: "sv);
  auto host_end = std::ranges::search(host_start.next(), "\x0d\x0a"sv);
  if(!host_start.empty()){
    fmt::print("host is: {}\n", std::string_view{host_start.begin(), host_start.end()});
    req.erase(host_start.begin(), host_end.end());
    req.insert(host_start.begin(), begin(wanted_host), end(wanted_host));
  }
  else
    fmt::print("no host\n");
}


void handle_client(int s,
                   rcv_f on_receive,
                   snd_f on_idle,
                   void * user)
{
  std::vector<char> buffer;
  const size_t READ_SZ = 16*1024;
          
  fd_set reads, writes, errors;
  
  while(!g_interrupted){
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 10'000};

    FD_ZERO(&reads);
    FD_ZERO(&writes);
    FD_ZERO(&errors);
    
    FD_SET(s, &reads);
    FD_SET(s, &errors);

    int rv = select(s+1, &reads, &writes, &errors, &timeout);
    if(rv < 0)
      die("select failed");

    if(FD_ISSET(s, &errors)){
      fmt::print("client handler: connection closed\n");
      return;
    } else if(FD_ISSET(s, &reads)){    

      buffer.resize(READ_SZ);
      auto read = recv(s, &buffer[0], buffer.size(), 0);
      buffer.resize(std::max(read, 0l));

      if (read < 0){
        fmt::print("client handler: error receiving\n");
        return;
      }
      if (read == 0){
        fmt::print("client handler: connection closed\n");
        return;
      }

      fmt::print("client handler: got {} bytes:\n", read);
      display_buffer_hex(buffer.data(), buffer.size(), 128);

      simplify_http_request(buffer);
      if(!on_receive(buffer, user)){
        fmt::print("client handler: stopped by on_receive\n");
        return;
      }
    }

    buffer.clear();
    if(!on_idle(buffer, user)){
      fmt::print("client handler: stopped by on_idle\n");
      return;
    }
    if(!buffer.empty()){

      fmt::print("response of {} bytes:\n", buffer.size());
      display_buffer_hex(buffer.data(), buffer.size(), 528);
      
      if(send(s, buffer.data(), buffer.size(), 0) < 0){
          fmt::print("client handler: error sending / connection closed\n");
          return;        
      }
    }
  }
}

void server(uint16_t port,
            rcv_f on_receive,
            snd_f on_idle,
            void * user)
{
  struct sockaddr_in servaddr, cli;
  const sigint_sentinel sigint;

  // socket create and verification
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd == -1)
    die("failed to create socket");
  
  const int enable = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    die("failed to set SO_REUSEADDR");
  
  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servaddr.sin_port = htons(port);
  
  if ((bind(sockfd, (sockaddr*) &servaddr, sizeof(servaddr))) != 0) 
    die("bind failed");
  
  // Now server is ready to listen and verification
  if ((listen(sockfd, 5)) != 0)
    die("listen failed");

  fmt::print("Now listening on http://localhost:{}/\n",port);
  
  while(!sigint.signalled()){
    unsigned len = sizeof(cli);
    int connfd = accept(sockfd, (sockaddr*)&cli, &len);
    if (connfd < 0) {
      die("accept failed");
    }
    
    fmt::print("Connection accepted\n");
    handle_client(connfd,
                  on_receive,
                  on_idle,
                  user);
    close(connfd);
  }
  close(sockfd);
}
