#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#include <libusb.h>

#include <fmt/format.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

struct candidate_ifc {
  libusb_device_handle * handle;
  int interface;
  int altsetting;
};

using proto_handler = void(const std::vector<char> & request,
                             std::vector<char> & response);

const uint16_t VID = 0x03f0;
const uint16_t PID = 0xbe2a;

static bool g_interrupted = false;

static void sigkill(int s){
  g_interrupted = true;
}

static void die(const std::string & fmts){
  fmt::print("{}\n", fmts);
  exit(1);
}

static void display_buffer_hex(unsigned char *buffer, unsigned size)
{
  unsigned i, j, k;
  
  for (i=0; i<size; i+=16) {
    printf("\n  %08x  ", i);
    for(j=0,k=0; k<16; j++,k++) {
      if (i+j < size) {
        printf("%02x", buffer[i+j]);
      } else {
        printf("  ");
      }
      printf(" ");
    }
    printf(" ");
    for(j=0,k=0; k<16; j++,k++) {
      if (i+j < size) {
        if ((buffer[i+j] < 32) || (buffer[i+j] > 126)) {
          printf(".");
        } else {
          printf("%c", buffer[i+j]);
        }
      }
    }
  }
  printf("\n" );
}


static bool read_http_request(int connfd, std::vector<char> & client_buffer) {
  const size_t READ_SZ = 1024;
  client_buffer.resize(0);
  ssize_t read = 0;
  do{
    auto pre_sz = client_buffer.size();
    client_buffer.resize(pre_sz + READ_SZ);
    read = recv(connfd, &client_buffer[pre_sz], READ_SZ, 0);
    if(read > 0)
      client_buffer.resize(pre_sz + read);
    if (read < 0)
      return false;
  } while(read == READ_SZ);
  return true;
}

static void server(short port, proto_handler & handler) {
  struct sockaddr_in servaddr, cli;

  std::vector<char> client_buffer;
  std::vector<char> response_buffer;

  signal(SIGKILL, sigkill);
  
  // socket create and verification
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd == -1)
    die("failed to create socket\n");

  const int enable = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    die("failed to set SO_REUSEADDR\n");
  
  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servaddr.sin_port = htons(port);
  
  if ((bind(sockfd, (sockaddr*) &servaddr, sizeof(servaddr))) != 0) 
    die("bind failed\n");
  
  // Now server is ready to listen and verification
  if ((listen(sockfd, 5)) != 0)
    die("listen failed");

  fmt::print("Now listening on http://localhost:{}/\n",port);
  
  while(!g_interrupted){
    unsigned len = sizeof(cli);
    int connfd = accept(sockfd, (sockaddr*)&cli, &len);
    if (connfd < 0) {
      perror("accept failed");
    }
    fmt::print("Connection accepted\n");

    bool connected = true;
    while(connected){
      connected = read_http_request(connfd, client_buffer);
      fmt::print("got {} bytes from client.", client_buffer.size());
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

static int poke(libusb_device_handle *handle)
{
  unsigned char buffer[1024*16];
  unsigned char request[] = ("GET / HTTP/1.1"
                             "\x0d\x0a"
                             "HOST: localhost"
                             "\x0d\x0a"
                             "\x0d\x0a");

  int size = 0;
  int out_ep = 0x03;
  int in_ep = 0x84;
  
  int r = libusb_bulk_transfer(handle, out_ep, request, sizeof(request) - 1, &size, 1000);
  if (r == LIBUSB_ERROR_PIPE) {
    fmt::print("transfer request failed\n");
    libusb_clear_halt(handle, out_ep);
    return 1;
  }

  fmt::print("successfully sent {} bytes, fetching response...\n", size);
  size = 0;
  for(int i = 0; i < 10 && !size; ++i){
    r = libusb_bulk_transfer(handle, in_ep, buffer, sizeof(buffer), &size, 1000);
    if (r == LIBUSB_ERROR_PIPE) {
      fmt::print("transfer request failed\n");
      libusb_clear_halt(handle, in_ep);
      return 1;
    }
    fmt::print("successfully read {} bytes:\n", size);
  }
  display_buffer_hex(buffer, size);
  
  return 0;
}

static void print_device_cap(struct libusb_bos_dev_capability_descriptor *dev_cap)
{
  switch(dev_cap->bDevCapabilityType) {
  case LIBUSB_BT_USB_2_0_EXTENSION: {
    struct libusb_usb_2_0_extension_descriptor *usb_2_0_ext = NULL;
    libusb_get_usb_2_0_extension_descriptor(NULL, dev_cap, &usb_2_0_ext);
    if (usb_2_0_ext) {
      printf("    USB 2.0 extension:\n");
      printf("      attributes             : %02X\n", usb_2_0_ext->bmAttributes);
      libusb_free_usb_2_0_extension_descriptor(usb_2_0_ext);
    }
    break;
  }
  case LIBUSB_BT_SS_USB_DEVICE_CAPABILITY: {
    struct libusb_ss_usb_device_capability_descriptor *ss_usb_device_cap = NULL;
    libusb_get_ss_usb_device_capability_descriptor(NULL, dev_cap, &ss_usb_device_cap);
    if (ss_usb_device_cap) {
      printf("    USB 3.0 capabilities:\n");
      printf("      attributes             : %02X\n", ss_usb_device_cap->bmAttributes);
      printf("      supported speeds       : %04X\n", ss_usb_device_cap->wSpeedSupported);
      printf("      supported functionality: %02X\n", ss_usb_device_cap->bFunctionalitySupport);
      libusb_free_ss_usb_device_capability_descriptor(ss_usb_device_cap);
    }
    break;
  }
  case LIBUSB_BT_CONTAINER_ID: {
    struct libusb_container_id_descriptor *container_id = NULL;
    libusb_get_container_id_descriptor(NULL, dev_cap, &container_id);
    if (container_id) {
      printf("    Container ID:\n      --uff--\n");
      libusb_free_container_id_descriptor(container_id);
    }
    break;
  }
  default:
    printf("    Unknown BOS device capability %02x:\n", dev_cap->bDevCapabilityType);
  }
}

static int test_device(uint16_t vid, uint16_t pid)
{
  std::vector<candidate_ifc> candidates;
  
  libusb_device_handle *handle;
  libusb_device *dev;
  uint8_t bus, port_path[8];
  struct libusb_bos_descriptor *bos_desc;
  struct libusb_config_descriptor *conf_desc;
  const struct libusb_endpoint_descriptor *endpoint;
  int i, j, k, r;
  int iface, nb_ifaces, first_iface = -1;
  struct libusb_device_descriptor dev_desc;
  const char* const speed_name[6] = { "Unknown", "1.5 Mbit/s (USB LowSpeed)", "12 Mbit/s (USB FullSpeed)",
                                      "480 Mbit/s (USB HighSpeed)", "5000 Mbit/s (USB SuperSpeed)", "10000 Mbit/s (USB SuperSpeedPlus)" };
  char string[128];
  uint8_t string_index[3];	// indexes of the string descriptors
  uint8_t endpoint_in = 0, endpoint_out = 0;	// default IN and OUT endpoints
  
  printf("Opening device %04X:%04X...\n", vid, pid);
  handle = libusb_open_device_with_vid_pid(NULL, vid, pid);
  
  if (handle == NULL) {
    perror("  Failed.\n");
    return -1;
  }

  dev = libusb_get_device(handle);
  bus = libusb_get_bus_number(dev);

  printf("\nReading device descriptor:\n");
  libusb_get_device_descriptor(dev, &dev_desc);
  printf("            length: %d\n", dev_desc.bLength);
  printf("      device class: %d\n", dev_desc.bDeviceClass);
  printf("               S/N: %d\n", dev_desc.iSerialNumber);
  printf("           VID:PID: %04X:%04X\n", dev_desc.idVendor, dev_desc.idProduct);
  printf("         bcdDevice: %04X\n", dev_desc.bcdDevice);
  printf("   iMan:iProd:iSer: %d:%d:%d\n", dev_desc.iManufacturer, dev_desc.iProduct, dev_desc.iSerialNumber);
  printf("          nb confs: %d\n", dev_desc.bNumConfigurations);
  // Copy the string descriptors for easier parsing
  string_index[0] = dev_desc.iManufacturer;
  string_index[1] = dev_desc.iProduct;
  string_index[2] = dev_desc.iSerialNumber;

  printf("\nReading BOS descriptor: ");
  if (libusb_get_bos_descriptor(handle, &bos_desc) == LIBUSB_SUCCESS) {
    printf("%d caps\n", bos_desc->bNumDeviceCaps);
    for (i = 0; i < bos_desc->bNumDeviceCaps; i++)
      print_device_cap(bos_desc->dev_capability[i]);
    libusb_free_bos_descriptor(bos_desc);
  } else {
    printf("no descriptor\n");
  }

  printf("\nReading first configuration descriptor:\n");
  libusb_get_config_descriptor(dev, 0, &conf_desc);
  printf("              total length: %d\n", conf_desc->wTotalLength);
  printf("         descriptor length: %d\n", conf_desc->bLength);
  nb_ifaces = conf_desc->bNumInterfaces;
  printf("             nb interfaces: %d\n", nb_ifaces);
  if (nb_ifaces > 0)
    first_iface = conf_desc->interface[0].altsetting[0].bInterfaceNumber;
  for (i=0; i<nb_ifaces; i++) {    
    printf("              interface[%d]: id = %d\n", i,
           conf_desc->interface[i].altsetting[0].bInterfaceNumber);
    for (j=0; j<conf_desc->interface[i].num_altsetting; j++) {
      bool has_bulk_03 = false;
      bool has_bulk_84 = false;

      printf("interface[%d].altsetting[%d]: num endpoints = %d\n",
             i, j, conf_desc->interface[i].altsetting[j].bNumEndpoints);
      printf("   Class.SubClass.Protocol: %02X.%02X.%02X\n",
             conf_desc->interface[i].altsetting[j].bInterfaceClass,
             conf_desc->interface[i].altsetting[j].bInterfaceSubClass,
             conf_desc->interface[i].altsetting[j].bInterfaceProtocol);
      if ( (conf_desc->interface[i].altsetting[j].bInterfaceClass == LIBUSB_CLASS_MASS_STORAGE)
           && ( (conf_desc->interface[i].altsetting[j].bInterfaceSubClass == 0x01)
                || (conf_desc->interface[i].altsetting[j].bInterfaceSubClass == 0x06) )
           && (conf_desc->interface[i].altsetting[j].bInterfaceProtocol == 0x50) ) {
        // Mass storage devices that can use basic SCSI commands
      }
      for (k=0; k<conf_desc->interface[i].altsetting[j].bNumEndpoints; k++) {
        struct libusb_ss_endpoint_companion_descriptor *ep_comp = NULL;
        endpoint = &conf_desc->interface[i].altsetting[j].endpoint[k];
        printf("       endpoint[%d].address: %02X\n", k, endpoint->bEndpointAddress);
        // Use the first interrupt or bulk IN/OUT endpoints as default for testing
        if ((endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) & (LIBUSB_TRANSFER_TYPE_BULK | LIBUSB_TRANSFER_TYPE_INTERRUPT)) {
          if (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
            if (!endpoint_in)
              endpoint_in = endpoint->bEndpointAddress;
          } else {
            if (!endpoint_out)
              endpoint_out = endpoint->bEndpointAddress;
          }
        }
        if (endpoint->bEndpointAddress == 0x03
            and endpoint->bmAttributes & 2){
          has_bulk_03 = true;
        }
        if (endpoint->bEndpointAddress == 0x84
            and endpoint->bmAttributes & 2){
          has_bulk_84 = true;
        }
        printf("           max packet size: %04X\n", endpoint->wMaxPacketSize);
        printf("          polling interval: %02X\n", endpoint->bInterval);
        libusb_get_ss_endpoint_companion_descriptor(NULL, endpoint, &ep_comp);
        if (ep_comp) {
          printf("                 max burst: %02X   (USB 3.0)\n", ep_comp->bMaxBurst);
          printf("        bytes per interval: %04X (USB 3.0)\n", ep_comp->wBytesPerInterval);
          libusb_free_ss_endpoint_companion_descriptor(ep_comp);
        }
      }
      if(has_bulk_03 and has_bulk_84){
        int ifc = conf_desc->interface[i].altsetting[0].bInterfaceNumber;
        int alt = j;
        fmt::print("candidate ifc {} alt {}\n", ifc, alt);
        candidates.push_back(candidate_ifc{handle, ifc, alt});
      }
    }
  }
  libusb_free_config_descriptor(conf_desc);
  
  libusb_set_auto_detach_kernel_driver(handle, 1);
  for (iface = 0; iface < nb_ifaces; iface++)
  {
    int ret = libusb_kernel_driver_active(handle, iface);
    printf("\nKernel driver attached for interface %d: %d\n", iface, ret);
    printf("\nClaiming interface %d...\n", iface);
    r = libusb_claim_interface(handle, iface);
    if (r != LIBUSB_SUCCESS) {
      perror("   Failed.\n");
    }
  }
  
  printf("\nReading string descriptors:\n");
  for (i=0; i<3; i++) {
    if (string_index[i] == 0) {
      continue;
    }
    if (libusb_get_string_descriptor_ascii(handle, string_index[i], (unsigned char*)string, sizeof(string)) > 0) {
      printf("   String (0x%02X): \"%s\"\n", string_index[i], string);
    }
  }

  poke(handle);
  
  
  printf("\n");
  for (iface = 0; iface<nb_ifaces; iface++) {
    printf("Releasing interface %d...\n", iface);
    libusb_release_interface(handle, iface);
  }
  
  printf("Closing device...\n");
  libusb_close(handle);
  
  return 0;
}

void h(const std::vector<char> & request,
       std::vector<char> & response) {
  response.resize(0);
  std::string rsp = ("HTTP/1.1 200 OK"
                     "\x0d\x0a"
                     "Content-Type: text/html"
                     "\x0d\x0a"
                     "Content-Length: 11"
                     "\x0d\x0a"
                     "\x0d\x0a"
                     "hello peeps");
  for(auto c : rsp) response.push_back(c);
}


int main() {
  server(8818, h);
  return 0;
  
  putenv("LIBUSB_DEBUG=3");
  
  auto version = libusb_get_version();
  fmt::print("Using libusb v{}.{}.{}.{}\n\n", version->major, version->minor, version->micro, version->nano);
  auto r = libusb_init(/*ctx=*/NULL);
  if (r < 0) die("failed to initialize libusb context");
  libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);

  test_device(VID, PID);
  
  libusb_exit(NULL);
  
  return 0;
}
