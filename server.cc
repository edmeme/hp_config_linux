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
    fmt::print("no host?\n");
}

template <typename F>
static void xserver(short port, const F & handler) {
  struct sockaddr_in servaddr, cli;

  std::vector<char> client_buffer;
  std::vector<char> response_buffer;

  //signal(SIGINT, sigint);
  
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
  
  while(!g_interrupted){
    unsigned len = sizeof(cli);
    int connfd = accept(sockfd, (sockaddr*)&cli, &len);
    if (connfd < 0) {
      die("accept failed");
    }
    fmt::print("Connection accepted\n");

    bool connected = true;
    while(connected){
      connected = read_http_request(connfd, client_buffer);
      fmt::print("got {} bytes from client.", client_buffer.size());
      simplify_http_request(client_buffer);
      display_buffer_hex((unsigned char *)client_buffer.data(),client_buffer.size());
      handler(client_buffer, response_buffer);
      fmt::print("responding with {}.", response_buffer.size());
      display_buffer_hex((unsigned char *)response_buffer.data(),response_buffer.size());
      connected = connected and (send(connfd, response_buffer.data(), response_buffer.size(), 0) > 1);
    }
  }
  close(sockfd);
  signal(SIGKILL, nullptr);
}
