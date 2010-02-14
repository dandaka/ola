/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * InterfacePicker.cpp
 * Chooses an interface to listen on
 * Copyright (C) 2005-2009 Simon Newton
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif


#ifdef OLA_USE_GETIFADDRS
  #include <ifaddrs.h>
  #include <linux/types.h>
  #include <linux/if_packet.h>
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#ifdef HAVE_SOCKADDR_DL_STRUCT
#include <net/if_dl.h>
#endif
#include <string.h>
#include <sys/ioctl.h>
#include <algorithm>
#include <string>
#include <vector>

#include "ola/Logging.h"
#include "ola/network/InterfacePicker.h"
#include "ola/network/NetworkUtils.h"

namespace ola {
namespace network {

using std::string;
using std::vector;


Interface::Interface() {
  ip_address.s_addr = 0;
  bcast_address.s_addr = 0;
  memset(hw_address, 0, MAC_LENGTH);
}


Interface::Interface(const Interface &other) {
  name = other.name;
  ip_address = other.ip_address;
  bcast_address = other.bcast_address;
  memcpy(hw_address, other.hw_address, MAC_LENGTH);
}


Interface& Interface::operator=(const Interface &other) {
  if (this != &other) {
    name = other.name;
    ip_address = other.ip_address;
    bcast_address = other.bcast_address;
    memcpy(hw_address, other.hw_address, MAC_LENGTH);
  }
  return *this;
}


/*
 * Select an interface to use
 * @param interface, the interface to populate
 * @param preferred_ip the ip address of the local interface we'd prefer to use
 * @return true if we found an interface, false otherwise
 */
bool InterfacePicker::ChooseInterface(Interface *interface,
                                      const string &preferred_ip) const {
  struct in_addr wanted_ip;
  bool use_preferred = false;
  vector<Interface> interfaces = GetInterfaces();

  if (!interfaces.size()) {
    OLA_INFO << "No interfaces found";
    return false;
  }

  if (!preferred_ip.empty()) {
    if (StringToAddress(preferred_ip, wanted_ip))
      use_preferred = true;
  }

  if (use_preferred) {
    vector<Interface>::const_iterator iter;
    for (iter = interfaces.begin(); iter != interfaces.end(); ++iter) {
      if ((*iter).ip_address.s_addr == wanted_ip.s_addr) {
        *interface = *iter;
        return true;
      }
    }
  }
  *interface = interfaces[0];
  return true;
}


/*
 * Return a vector of interfaces on the system.
 */
vector<Interface> InterfacePicker::GetInterfaces() const {
  vector<Interface> interfaces;
  string last_dl_iface_name;
  uint8_t hwlen = 0;
  char *hwaddr = NULL;

  // create socket to get iface config
  int sd = socket(PF_INET, SOCK_DGRAM, 0);

  if (sd < 0) {
    OLA_WARN << "Could not create socket " << strerror(errno);
    return interfaces;
  }

  // use ioctl to get a listing of interfaces
  char *buffer;  // holds the iface data
  unsigned int lastlen = 0;  // the amount of data returned by the last ioctl
  unsigned int len = INITIAL_IFACE_COUNT;

  while (true) {
    struct ifconf ifc;
    ifc.ifc_len = len * sizeof(struct ifreq);
    buffer = new char[ifc.ifc_len];
    ifc.ifc_buf = buffer;

    if (ioctl(sd, SIOCGIFCONF, &ifc) < 0) {
      if (errno != EINVAL || lastlen != 0) {
        OLA_WARN << "ioctl error " << strerror(errno);
        delete[] buffer;
        return interfaces;
      }
    } else {
      if (static_cast<unsigned int>(ifc.ifc_len) == lastlen) {
        lastlen = ifc.ifc_len;
        break;
      }
      lastlen = ifc.ifc_len;
    }
    len += IFACE_COUNT_INC;
    delete[] buffer;
  }

  // loop through each iface
  for (char *ptr = buffer; ptr < buffer + lastlen;) {
    struct ifreq *iface = (struct ifreq*) ptr;
    ptr += GetIfReqSize(ptr);

#ifdef HAVE_SOCKADDR_DL_STRUCT
    if (iface->ifr_addr.sa_family == AF_LINK) {
      struct sockaddr_dl *sdl = (struct sockaddr_dl*) &iface->ifr_addr;
      last_dl_iface_name.assign(sdl->sdl_data, sdl->sdl_nlen);
      hwaddr = sdl->sdl_data + sdl->sdl_nlen;
      hwlen = sdl->sdl_alen;
    }
#endif

    // look for AF_INET interfaces only
    if (iface->ifr_addr.sa_family != AF_INET) {
      OLA_DEBUG << "skipping " << iface->ifr_name <<
        " because it's not af_inet";
      continue;
    }

    struct ifreq ifrcopy = *iface;
    if (ioctl(sd, SIOCGIFFLAGS, &ifrcopy) < 0) {
      OLA_WARN << "ioctl error for " << iface->ifr_name << ":"
        << strerror(errno);
      continue;
    }

    if (!(ifrcopy.ifr_flags & IFF_UP)) {
      OLA_DEBUG << "skipping " << iface->ifr_name << " because it's down";
      continue;
    }

    if (ifrcopy.ifr_flags & IFF_LOOPBACK) {
      OLA_DEBUG << "skipping " << iface->ifr_name <<
        " because it's a loopback";
      continue;
    }

    Interface interface;
    interface.name = iface->ifr_name;
    if (interface.name == last_dl_iface_name && hwaddr) {
      memcpy(interface.hw_address, hwaddr,
             std::min(hwlen, (uint8_t) MAC_LENGTH));
    }
    struct sockaddr_in *sin = (struct sockaddr_in *) &iface->ifr_addr;
    interface.ip_address = sin->sin_addr;

    // fetch bcast address
#ifdef SIOCGIFBRDADDR
    if (ifrcopy.ifr_flags & IFF_BROADCAST) {
      if (ioctl(sd, SIOCGIFBRDADDR, &ifrcopy) < 0) {
        OLA_WARN << "ioctl error " << strerror(errno);
      } else {
        sin = (struct sockaddr_in *) &ifrcopy.ifr_broadaddr;
        interface.bcast_address = sin->sin_addr;
      }
    }
#endif

    // fetch hardware address
#ifdef SIOCGIFHWADDR
    if (ifrcopy.ifr_flags & SIOCGIFHWADDR) {
      if (ioctl(sd, SIOCGIFHWADDR, &ifrcopy) < 0) {
        OLA_WARN << "ioctl error" << strerror(errno);
      } else {
        memcpy(interface.hw_address, ifrcopy.ifr_hwaddr.sa_data, MAC_LENGTH);
      }
    }
#endif

    /* ok, if that all failed we should prob try and use sysctl to work out the bcast
     * and hware addresses
     * i'll leave that for another day
     */
    interfaces.push_back(interface);
  }
  close(sd);
  delete[] buffer;
  return interfaces;
}


/*
 * Return the size of an ifreq structure in a cross platform manner.
 * @param data a pointer to an ifreq structure
 * @return the size of the ifreq structure
 */
unsigned int InterfacePicker::GetIfReqSize(const char *data) const {
  const struct ifreq *iface = (struct ifreq*) data;

#ifdef HAVE_SOCKADDR_SA_LEN
  unsigned int socket_len = iface->ifr_addr.sa_len;
#else
  unsigned int socket_len = sizeof(struct sockaddr);
  switch (iface->ifr_addr.sa_family) {
    case AF_INET:
      socket_len = sizeof(struct sockaddr_in);
      break;
#ifdef IPV6
    case AF_INET6:
      socket_len = sizeof(struct sockaddr_in6);
      break;
#endif
#ifdef HAVE_SOCKADDR_DL_STRUCT
    case AF_LINK:
      socket_len = sizeof(struct sockaddr_dl);
      break;
#endif
  }
#endif

  // We can't assume sizeof(ifreq) = IFNAMSIZ + sizeof(sockaddr), this isn't
  // the case on some 64bit linux systems.
  if (socket_len > sizeof(struct ifreq) - IFNAMSIZ)
    return IFNAMSIZ + socket_len;
  else
    return sizeof(struct ifreq);
}
}  // network
}  // ola
