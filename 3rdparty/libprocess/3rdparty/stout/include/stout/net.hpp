/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_NET_HPP__
#define __STOUT_NET_HPP__

#if defined(__linux__) || defined(__APPLE__)
#include <ifaddrs.h>
#endif
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <arpa/inet.h>

#ifdef __linux__
#include <linux/if_packet.h>
#endif

#include <net/if.h>
#ifdef __APPLE__
#include <net/if_dl.h>
#include <net/if_types.h>
#endif

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <curl/curl.h>

#include <iostream>
#include <set>
#include <string>

#include "abort.hpp"
#include "error.hpp"
#include "none.hpp"
#include "option.hpp"
#include "os.hpp"
#include "result.hpp"
#include "stringify.hpp"
#include "try.hpp"


// Network utilities.
namespace net {

// Returns the HTTP response code resulting from attempting to download the
// specified HTTP or FTP URL into a file at the specified path.
inline Try<int> download(const std::string& url, const std::string& path)
{
  Try<int> fd = os::open(
      path, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (fd.isError()) {
    return Error(fd.error());
  }

  curl_global_init(CURL_GLOBAL_ALL);
  CURL* curl = curl_easy_init();

  if (curl == NULL) {
    curl_easy_cleanup(curl);
    os::close(fd.get());
    return Error("Failed to initialize libcurl");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);

  FILE* file = fdopen(fd.get(), "w");
  if (file == NULL) {
    return ErrnoError("Failed to open file handle of '" + path + "'");
  }
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

  CURLcode curlErrorCode = curl_easy_perform(curl);
  if (curlErrorCode != 0) {
    curl_easy_cleanup(curl);
    fclose(file);
    return Error(curl_easy_strerror(curlErrorCode));
  }

  long code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);

  if (fclose(file) != 0) {
    return ErrnoError("Failed to close file handle of '" + path + "'");
  }

  return Try<int>::some(code);
}


// Returns a Try of the hostname for the provided IP. If the hostname cannot
// be resolved, then a string version of the IP address is returned.
inline Try<std::string> getHostname(uint32_t ip)
{
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = ip;

  char hostname[MAXHOSTNAMELEN];
  if (getnameinfo(
      (sockaddr*)&addr,
      sizeof(addr),
      hostname,
      MAXHOSTNAMELEN,
      NULL,
      0,
      0) != 0) {
    return ErrnoError();
  }

  return std::string(hostname);
}


// Returns the names of all the link devices in the system.
inline Try<std::set<std::string> > links()
{
#if !defined(__linux__) && !defined(__APPLE__)
  return Error("Not implemented");
#else
  struct ifaddrs* ifaddr = NULL;
  if (getifaddrs(&ifaddr) == -1) {
    return ErrnoError();
  }

  std::set<std::string> names;
  for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_name != NULL) {
      names.insert(ifa->ifa_name);
    }
  }

  freeifaddrs(ifaddr);
  return names;
#endif
}


// Represents a MAC address. A MAC address is a 48-bit unique
// identifier assigned to a network interface for communications on
// the physical network segment. We use a byte array (in network
// order) to represent a MAC address. For example, for a MAC address
// 01:23:34:67:89:ab, the format is shown as follows:
//
//    MSB                                          LSB
//     |                                            |
//     v                                            v
// +--------+--------+--------+--------+--------+--------+
// |bytes[0]|bytes[1]|bytes[2]|bytes[3]|bytes[4]|bytes[5]|
// +--------+--------+--------+--------+--------+--------+
//
//     01   :   23   :   45   :   67   :   89   :   ab
class MAC
{
public:
  // Returns the byte at the given index. For example, for a MAC
  // address 01:23:45:67:89:ab, mac[0] = 01, mac[1] = 23 and etc.
  uint8_t operator [] (size_t index) const
  {
    if (index >= 6) {
      ABORT("Invalid index specified in MAC::operator []\n");
    }

    return bytes[index];
  }

private:
  friend std::ostream& operator << (std::ostream& stream, const MAC& mac);
  friend Result<MAC> mac(const std::string& name);

  explicit MAC(uint8_t* _bytes)
  {
    for (size_t i = 0; i < 6; i++) {
      bytes[i] = _bytes[i];
    }
  }

  // Byte array of this MAC address (in network order).
  uint8_t bytes[6];
};


// Returns the standard string format (IEEE 802) of the given MAC
// address, which contains six groups of two hexadecimal digits,
// separated by colons, in network order (e.g., 01:23:45:67:89:ab).
inline std::ostream& operator << (std::ostream& stream, const MAC& mac)
{
  char buffer[18];

  sprintf(
      buffer,
      "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
      mac.bytes[0],
      mac.bytes[1],
      mac.bytes[2],
      mac.bytes[3],
      mac.bytes[4],
      mac.bytes[5]);

  return stream << buffer;
}


// Returns the MAC address of a given link device. The link device is
// specified using its name (e.g., eth0). Returns error if the link
// device is not found. Returns none if the link device is found, but
// does not have a MAC address (e.g., loopback).
inline Result<MAC> mac(const std::string& name)
{
#if !defined(__linux__) && !defined(__APPLE__)
  return Error("Not implemented");
#else
  struct ifaddrs* ifaddr = NULL;
  if (getifaddrs(&ifaddr) == -1) {
    return ErrnoError();
  }

  // Indicates whether the link device is found or not.
  bool found = false;

  for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_name != NULL && !strcmp(ifa->ifa_name, name.c_str())) {
      found = true;

# if defined(__linux__)
     if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_PACKET) {
        struct sockaddr_ll* link = (struct sockaddr_ll*) ifa->ifa_addr;

        if (link->sll_halen == 6) {
          MAC mac((uint8_t*) link->sll_addr);

          // Ignore if the address is 0 so that the results are
          // consistent on both OSX and Linux.
          if (stringify(mac) == "00:00:00:00:00:00") {
            continue;
          }

          freeifaddrs(ifaddr);
          return mac;
        }
      }
# elif defined(__APPLE__)
      if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_LINK) {
        struct sockaddr_dl* link = (struct sockaddr_dl*) ifa->ifa_addr;

        if (link->sdl_type == IFT_ETHER && link->sdl_alen == 6) {
          MAC mac((uint8_t*) LLADDR(link));

          freeifaddrs(ifaddr);
          return mac;
        }
      }
# endif
    }
  }

  freeifaddrs(ifaddr);

  if (!found) {
    return Error("Cannot find the link device");
  }

  return None();
#endif
}


// Represents an IPv4 address. Besides the actual IP address, we also
// store additional information about the address such as the net mask
// which defines the subnet.
class IP
{
public:
  // Returns the IP address (in network order).
  uint32_t address() const { return address_; }

  // Returns the net mask (in network order).
  Option<uint32_t> netmask() const { return netmask_; }

  // Returns the prefix of the subnet defined by the net mask.
  Option<size_t> prefix() const
  {
    if (netmask_.isNone()) {
      return None();
    }

    // Convert the net mask to host order.
    uint32_t mask = ntohl(netmask_.get());

    size_t value = 0;
    while (mask != 0) {
      value += mask & 0x1;
      mask >>= 1;
    }

    return value;
  }

private:
  friend std::ostream& operator << (std::ostream& stream, const IP& ip);
  friend Result<IP> ip(const std::string& name);

  explicit IP(uint32_t _address) : address_(_address) {}

  IP(uint32_t _address, uint32_t _netmask)
    : address_(_address), netmask_(_netmask) {}

  // IP address (in network order).
  uint32_t address_;

  // The optional net mask (in network order).
  Option<uint32_t> netmask_;
};


// Returns the string representation of the given IP address using the
// canonical dot-decimal form with prefix. For example: "10.0.0.1/8".
inline std::ostream& operator << (std::ostream& stream, const IP& ip)
{
  char buffer[INET_ADDRSTRLEN];

  const char* addr = inet_ntop(AF_INET, &ip.address_, buffer, sizeof(buffer));
  if (addr == NULL) {
    // We do not expect inet_ntop to fail because all parameters
    // passed in are valid.
    std::string message =
      "inet_ntop returns error for address " + stringify(ip.address());

    perror(message.c_str());
    abort();
  }

  stream << addr;

  if (ip.prefix().isSome()) {
    stream << "/" << ip.prefix().get();
  }

  return stream;
}


// Returns the first available IPv4 address of a given link device.
// The link device is specified using its name (e.g., eth0). Returns
// error if the link device is not found. Returns none if the link
// device is found, but does not have an IPv4 address.
// TODO(jieyu): It is uncommon, but likely that a link device has
// multiple IPv4 addresses. In that case, consider returning the
// primary IP address instead of the first one.
inline Result<IP> ip(const std::string& name)
{
#if !defined(__linux__) && !defined(__APPLE__)
  return Error("Not implemented");
#else
  struct ifaddrs* ifaddr = NULL;
  if (getifaddrs(&ifaddr) == -1) {
    return ErrnoError();
  }

  // Indicates whether the link device is found or not.
  bool found = false;

  for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_name != NULL && !strcmp(ifa->ifa_name, name.c_str())) {
      found = true;

      if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_INET) {
        struct sockaddr_in* addr = (struct sockaddr_in*) ifa->ifa_addr;

        if (ifa->ifa_netmask != NULL &&
            ifa->ifa_netmask->sa_family == AF_INET) {
          struct sockaddr_in* netmask = (struct sockaddr_in*) ifa->ifa_netmask;

          IP ip((uint32_t) addr->sin_addr.s_addr,
                (uint32_t) netmask->sin_addr.s_addr);

          freeifaddrs(ifaddr);
          return ip;
        }

        // Note that this is the case where net mask is not specified.
        // We've seen such cases when VPN is used.
        IP ip((uint32_t) addr->sin_addr.s_addr);

        freeifaddrs(ifaddr);
        return ip;
      }
    }
  }

  freeifaddrs(ifaddr);

  if (!found) {
    return Error("Cannot find the link device");
  }

  return None();
#endif
}

} // namespace net {

#endif // __STOUT_NET_HPP__
