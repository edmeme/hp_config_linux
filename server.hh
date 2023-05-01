#pragma once
#include <cstdint>
#include <vector>

bool read_http_request(int connfd, std::vector<char> & client_buffer);
void simplify_http_request(std::vector<char> & req);

using server_handler_f = bool (const std::vector<char> & req,
                               std::vector<char> & rsp,
                               void * param);


using rcv_f = bool (const std::vector<char> & request, void * user);
using snd_f = bool (std::vector<char> & send_buffer, void * user);

void server(uint16_t port,
            rcv_f on_receive,
            snd_f on_idle,
            void * user);
