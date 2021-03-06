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

#include "Constants.hpp"
#include "DeferredPackets.hpp"
#include "IncomingPacket.hpp"
#include "RuntimeEnvironment.hpp"
#include "Node.hpp"

namespace ZeroTier {

DeferredPackets::DeferredPackets(const RuntimeEnvironment *renv) :
	RR(renv),
	_readPtr(0),
	_writePtr(0),
	_die(false)
{
}

DeferredPackets::~DeferredPackets()
{
	_q_m.lock();
	_die = true;
	_q_m.unlock();
	_q_s.post();
}

bool DeferredPackets::enqueue(IncomingPacket *pkt)
{
	_q_m.lock();
	const unsigned long p = _writePtr % ZT_DEFFEREDPACKETS_MAX;
	if (_q[p]) {
		_q_m.unlock();
		return false;
	} else {
		_q[p].setToUnsafe(pkt);
		++_writePtr;
		_q_m.unlock();
		_q_s.post();
		return true;
	}
}

int DeferredPackets::process()
{
	SharedPtr<IncomingPacket> pkt;

	_q_m.lock();
	if (_die) {
		_q_m.unlock();
		_q_s.post();
		return -1;
	}
	while (_readPtr == _writePtr) {
		_q_m.unlock();
		_q_s.wait();
		_q_m.lock();
		if (_die) {
			_q_m.unlock();
			_q_s.post();
			return -1;
		}
	}
	pkt.swap(_q[_readPtr++ % ZT_DEFFEREDPACKETS_MAX]);
	_q_m.unlock();

	pkt->tryDecode(RR,true);
	return 1;
}

} // namespace ZeroTier
