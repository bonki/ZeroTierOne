/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2015  ZeroTier, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#ifndef ZT_UPNPCLIENT_HPP
#define ZT_UPNPCLIENT_HPP

#ifdef ZT_USE_MINIUPNPC

#include <vector>

#include "../node/Constants.hpp"
#include "../node/InetAddress.hpp"
#include "../node/Mutex.hpp"
#include "Thread.hpp"

/**
 * How frequently should we refresh our UPNP/NAT-PnP/whatever state?
 */
#define ZT_UPNP_CLIENT_REFRESH_DELAY 600000

namespace ZeroTier {

class UPNPClientImpl;

/**
 * UPnP/NAT-PnP daemon thread
 */
class UPNPClient
{
	friend class UPNPClientImpl;

public:
	/**
	 * Create and start UPNP client service
	 *
	 * @param localUdpPortToMap Port we want visible to the outside world
	 */
	UPNPClient(int localUdpPortToMap);

	~UPNPClient();

	/**
	 * @return All current external mappings for our port
	 */
	std::vector<InetAddress> get() const;

private:
	UPNPClientImpl *_impl;
};

} // namespace ZeroTier

#endif // ZT_USE_MINIUPNPC

#endif
