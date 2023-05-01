#include "util.hh"
#include "server.hh"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <libusb.h>

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
