#pragma once
#include <cstdint>
#include <vector>

bool read_http_request(int connfd, std::vector<char> & client_buffer);
void simplify_http_request(std::vector<char> & req);

template <typename F>
static void server(short port, const F & handler) {
}
