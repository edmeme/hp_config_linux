#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <libusb.h>

#include <fmt/format.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

using namespace std::literals;

struct candidate_ifc {
  libusb_device_handle * handle;
  int interface;
  int altsetting;
};

const uint16_t VID = 0x03f0;
const uint16_t PID = 0xbe2a;

static bool g_interrupted = false;

static void sigint(int s){
  g_interrupted = true;
}

template <typename ...T>
static void _die(const std::source_location & s, fmt::format_string<T...> fmt, T && ... args){
  auto str = fmt::format(fmt, args...);
  auto location = fmt::format("{}:{}:{}", s.file_name(), s.line(), s.column());
  if(errno != 0){
    auto system = fmt::format("Last system error is {} (error {})", std::strerror(errno), errno);
    fmt::print(stderr, "{} ERROR: {}. {}.\n", location, str, system);
  } else {
    fmt::print(stderr, "{} ERROR: {}. (No system error)\n", location, str);
  }
  throw std::runtime_error(str);
}

#define die(fmt, ...) (_die(std::source_location::current(), fmt __VA_OPT__(,) __VA_ARGS__ ))

static void display_buffer_hex(unsigned char *buffer, unsigned size, unsigned limit=128)
{
  unsigned i, j, k;
  
  for (i=0; i<size; i+=16) {
    fmt::print("\n  {:08x}  ", i);
    for(j=0,k=0; k<16; j++,k++) {
      if (i+j < size) {
        fmt::print("{:02x}", buffer[i+j]);
      } else {
        fmt::print("  ");
      }
      fmt::print(" ");
    }
    fmt::print(" ");
    for(j=0,k=0; k<16; j++,k++) {
      if (i+j < size) {
        if ((buffer[i+j] < 32) || (buffer[i+j] > 126)) {
          fmt::print(".");
        } else {
          fmt::print("{:c}", buffer[i+j]);
        }
      }
    }
    if(i >= limit){
      fmt::print("\n  ... skipped {} bytes ...", size-i);
      break;
    }
  }
  fmt::print("\n");
}


static bool read_http_request(int connfd, std::vector<char> & client_buffer) {
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

static void simplify_http_request(std::vector<char> & req) {
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
static void server(short port, const F & handler) {
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

static int poke(libusb_device_handle *handle
                , const std::vector<char> & request
                , std::vector<char> & response)
{
  int size = 0;
  int out_ep = 0x03;
  int in_ep = 0x84;
  
  int r = libusb_bulk_transfer(handle, out_ep, (unsigned char *) request.data(), request.size(), &size, 1000);
  if (r == LIBUSB_ERROR_PIPE) {
    fmt::print("transfer request failed\n");
    libusb_clear_halt(handle, out_ep);
    return 1;
  }

  fmt::print("successfully sent {} bytes of {}, fetching response...\n", size, request.size());

  const size_t READ_SZ = 1024;
  response.clear();
  do{
    size = 0;
    auto pre_sz = response.size();
    response.resize(pre_sz + READ_SZ);

    r = libusb_bulk_transfer(handle, in_ep
                             ,(unsigned char *) &response[pre_sz], READ_SZ
                             , &size
                             , 1000);
    if (r == LIBUSB_ERROR_PIPE) {
      fmt::print("transfer request failed\n");
      libusb_clear_halt(handle, in_ep);
      return 1;
    }
    fmt::print("successfully read {} bytes:\n", size);
  } while(size == READ_SZ or response.empty());

  return 0;
}


static void print_device_info(libusb_device_handle * handle) {
  struct libusb_config_descriptor *conf_desc;
  const struct libusb_endpoint_descriptor *endpoint;
  struct libusb_device_descriptor dev_desc;

  auto dev = libusb_get_device(handle);
  fmt::print("\nReading device descriptor:\n");
  libusb_get_device_descriptor(dev, &dev_desc);

  auto print_descriptor =
    [handle](const char * desc, uint8_t string_index){
      char string[128];
      string[0] = 0;
      if (string_index
          and libusb_get_string_descriptor_ascii(handle, string_index, (unsigned char*)string, sizeof(string)) > 0
        ) 
        fmt::print("   {} : \"{}\"\n", desc, string);
      else
        fmt::print("   {} : unkown\n", desc);
    };
  
  print_descriptor(" manufacturer", dev_desc.iManufacturer);
  print_descriptor("      product", dev_desc.iProduct);
  print_descriptor("serial number", dev_desc.iSerialNumber);

  libusb_get_config_descriptor(dev, 0, &conf_desc);
  auto n_ifaces = conf_desc->bNumInterfaces;
  if (n_ifaces > 0){
    fmt::print("{} interfaces found:\n", n_ifaces);
  } else {
    fmt::print("No interfaces found!\n", n_ifaces);
  }

  for (auto interface : std::span{conf_desc->interface, n_ifaces}) {
    int id = interface.altsetting[0].bInterfaceNumber;
    fmt::print(" - interface {}\n", id);
    for (auto altsetting : std::span{interface.altsetting, (size_t)interface.num_altsetting}) {
      int aid = altsetting.bAlternateSetting;
      fmt::print("   - altsetting {}\n", aid);
      fmt::print("     Class.SubClass.Protocol: {:02X}.{:02X}.{:02X}\n",
             altsetting.bInterfaceClass,
             altsetting.bInterfaceSubClass,
             altsetting.bInterfaceProtocol);

      for (int k=0; k<altsetting.bNumEndpoints; k++) {
        struct libusb_ss_endpoint_companion_descriptor *ep_comp = NULL;
        endpoint = &altsetting.endpoint[k];
        fmt::print("     - endpoint {:02X}\n", endpoint->bEndpointAddress);
        fmt::print("          max packet size: {:04X}\n", endpoint->wMaxPacketSize);
        fmt::print("          polling interval: {:02X}\n", endpoint->bInterval);
        libusb_get_ss_endpoint_companion_descriptor(NULL, endpoint, &ep_comp);
        if (ep_comp) {
          fmt::print("          max burst: {:02X}   (USB 3.0)\n", ep_comp->bMaxBurst);
          fmt::print("          bytes per interval: {:04X} (USB 3.0)\n", ep_comp->wBytesPerInterval);
          libusb_free_ss_endpoint_companion_descriptor(ep_comp);
        }
      }
    }
  }
  libusb_free_config_descriptor(conf_desc);
}

static std::vector<candidate_ifc> find_candidate_interfaces(libusb_device_handle * handle) {
  std::vector<candidate_ifc> candidates;
  struct libusb_config_descriptor *conf_desc;
  const struct libusb_endpoint_descriptor *endpoint;
  struct libusb_device_descriptor dev_desc;

  auto dev = libusb_get_device(handle);
  libusb_get_device_descriptor(dev, &dev_desc);
  libusb_get_config_descriptor(dev, 0, &conf_desc);
  
  auto n_ifaces = conf_desc->bNumInterfaces;
  for (auto interface : std::span{conf_desc->interface, n_ifaces}) {
    int id = interface.altsetting[0].bInterfaceNumber;
    for (auto altsetting : std::span{interface.altsetting, (size_t)interface.num_altsetting}) {
      int aid = altsetting.bAlternateSetting;
      bool has_bulk_03 = false;
      bool has_bulk_84 = false;
      
      for (int k=0; k<altsetting.bNumEndpoints; k++) {
        struct libusb_ss_endpoint_companion_descriptor *ep_comp = NULL;
        endpoint = &altsetting.endpoint[k];
        if (endpoint->bEndpointAddress == 0x03
            and endpoint->bmAttributes & 2){
          has_bulk_03 = true;
        }
        if (endpoint->bEndpointAddress == 0x84
            and endpoint->bmAttributes & 2){
          has_bulk_84 = true;
        }
        libusb_get_ss_endpoint_companion_descriptor(NULL, endpoint, &ep_comp);
        if (ep_comp) {
          libusb_free_ss_endpoint_companion_descriptor(ep_comp);
        }
      }
      if(has_bulk_03 and has_bulk_84){
        int ifc = interface.altsetting[0].bInterfaceNumber;
        int alt = aid;
        candidates.push_back(candidate_ifc{handle, ifc, alt});
      }
    }
  }
  libusb_free_config_descriptor(conf_desc);

  return candidates;
}


static int test_device(uint16_t vid, uint16_t pid)
{
  
  libusb_device_handle *handle;
  int r;
  
  fmt::print("Opening device {:04X}:{:04X}...\n", vid, pid);
  handle = libusb_open_device_with_vid_pid(NULL, vid, pid);  
  if (handle == NULL) {
    die("Failed to open usb device.");
    return -1;
  }

  print_device_info(handle);  
  auto candidates = find_candidate_interfaces(handle);
  
  if(candidates.empty()){
    fmt::print("It does not seem like the device is a compatible printer, \n");
    fmt::print("It is possible that the device is compatible but uses different endpoints than expected.");
    return 1;
  }
  
  libusb_set_auto_detach_kernel_driver(handle, true);
  
  auto candidate = candidates[0];
  int ret = libusb_kernel_driver_active(handle, candidate.interface);
  fmt::print("Kernel driver attached for interface {}: {}\n", candidate.interface, ret);
  fmt::print("Claiming interface {}...\n", candidate.interface);
  r = libusb_claim_interface(handle, candidate.interface);
  if (r != LIBUSB_SUCCESS) {
    die("Failed to claim usb interface.");
  }
  
  server(8102,
         [handle](const std::vector<char> & request, std::vector<char> & response) {
           return 0 == poke(handle, request, response);
         });
  
  
  fmt::print("\n");
  fmt::print("Releasing interface.\n");
  libusb_release_interface(handle, candidate.interface);
  
  fmt::print("Closing device...\n");
  libusb_close(handle);
  
  return 0;
}


int main() {  
  putenv("LIBUSB_DEBUG=3");
  
  auto version = libusb_get_version();
  fmt::print("Using libusb v{}.{}.{}.{}\n\n", version->major, version->minor, version->micro, version->nano);
  auto r = libusb_init(NULL);
  if (r < 0) die("failed to initialize libusb context");
  libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);

  int rv = test_device(VID, PID);
  
  libusb_exit(NULL);
  
  return rv;
}
