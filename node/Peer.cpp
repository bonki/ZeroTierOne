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

#include "../version.h"

#include "Constants.hpp"
#include "Peer.hpp"
#include "Node.hpp"
#include "Switch.hpp"
#include "Network.hpp"
#include "AntiRecursion.hpp"
#include "SelfAwareness.hpp"
#include "Cluster.hpp"
#include "Packet.hpp"

#include <algorithm>

#define ZT_PEER_PATH_SORT_INTERVAL 5000

namespace ZeroTier {

// Used to send varying values for NAT keepalive
static uint32_t _natKeepaliveBuf = 0;

Peer::Peer(const Identity &myIdentity,const Identity &peerIdentity)
	throw(std::runtime_error) :
	_lastUsed(0),
	_lastReceive(0),
	_lastUnicastFrame(0),
	_lastMulticastFrame(0),
	_lastAnnouncedTo(0),
	_lastPathConfirmationSent(0),
	_lastDirectPathPushSent(0),
	_lastDirectPathPushReceive(0),
	_lastPathSort(0),
	_vProto(0),
	_vMajor(0),
	_vMinor(0),
	_vRevision(0),
	_id(peerIdentity),
	_numPaths(0),
	_latency(0),
	_directPathPushCutoffCount(0),
	_networkComs(4),
	_lastPushedComs(4)
{
	if (!myIdentity.agree(peerIdentity,_key,ZT_PEER_SECRET_KEY_LENGTH))
		throw std::runtime_error("new peer identity key agreement failed");
}

void Peer::received(
	const RuntimeEnvironment *RR,
	const InetAddress &localAddr,
	const InetAddress &remoteAddr,
	unsigned int hops,
	uint64_t packetId,
	Packet::Verb verb,
	uint64_t inRePacketId,
	Packet::Verb inReVerb)
{
#ifdef ZT_ENABLE_CLUSTER
	bool suboptimalPath = false;
	if ((RR->cluster)&&(hops == 0)) {
		// Note: findBetterEndpoint() is first since we still want to check
		// for a better endpoint even if we don't actually send a redirect.
		InetAddress redirectTo;
		if ( (RR->cluster->findBetterEndpoint(redirectTo,_id.address(),remoteAddr,false)) && (verb != Packet::VERB_OK)&&(verb != Packet::VERB_ERROR)&&(verb != Packet::VERB_RENDEZVOUS)&&(verb != Packet::VERB_PUSH_DIRECT_PATHS) ) {
			if (_vProto >= 5) {
				// For newer peers we can send a more idiomatic verb: PUSH_DIRECT_PATHS.
				Packet outp(_id.address(),RR->identity.address(),Packet::VERB_PUSH_DIRECT_PATHS);
				outp.append((uint16_t)1); // count == 1
				outp.append((uint8_t)0); // no flags
				outp.append((uint16_t)0); // no extensions
				if (redirectTo.ss_family == AF_INET) {
					outp.append((uint8_t)4);
					outp.append((uint8_t)6);
					outp.append(redirectTo.rawIpData(),4);
				} else {
					outp.append((uint8_t)6);
					outp.append((uint8_t)18);
					outp.append(redirectTo.rawIpData(),16);
				}
				outp.append((uint16_t)redirectTo.port());
				outp.armor(_key,true);
				RR->antiRec->logOutgoingZT(outp.data(),outp.size());
				RR->node->putPacket(localAddr,remoteAddr,outp.data(),outp.size());
			} else {
				// For older peers we use RENDEZVOUS to coax them into contacting us elsewhere.
				Packet outp(_id.address(),RR->identity.address(),Packet::VERB_RENDEZVOUS);
				outp.append((uint8_t)0); // no flags
				RR->identity.address().appendTo(outp);
				outp.append((uint16_t)redirectTo.port());
				if (redirectTo.ss_family == AF_INET) {
					outp.append((uint8_t)4);
					outp.append(redirectTo.rawIpData(),4);
				} else {
					outp.append((uint8_t)16);
					outp.append(redirectTo.rawIpData(),16);
				}
				outp.armor(_key,true);
				RR->antiRec->logOutgoingZT(outp.data(),outp.size());
				RR->node->putPacket(localAddr,remoteAddr,outp.data(),outp.size());
			}
			suboptimalPath = true;
		}
	}
#endif

	const uint64_t now = RR->node->now();
	bool needMulticastGroupAnnounce = false;
	bool pathIsConfirmed = false;

	{	// begin _lock
		Mutex::Lock _l(_lock);

		_lastReceive = now;
		if ((verb == Packet::VERB_FRAME)||(verb == Packet::VERB_EXT_FRAME))
			_lastUnicastFrame = now;
		else if (verb == Packet::VERB_MULTICAST_FRAME)
			_lastMulticastFrame = now;

		if ((now - _lastAnnouncedTo) >= ((ZT_MULTICAST_LIKE_EXPIRE / 2) - 1000)) {
			_lastAnnouncedTo = now;
			needMulticastGroupAnnounce = true;
		}

		if (hops == 0) {
			unsigned int np = _numPaths;
			for(unsigned int p=0;p<np;++p) {
				if ((_paths[p].address() == remoteAddr)&&(_paths[p].localAddress() == localAddr)) {
					_paths[p].received(now);
#ifdef ZT_ENABLE_CLUSTER
					_paths[p].setClusterSuboptimal(suboptimalPath);
#endif
					pathIsConfirmed = true;
					break;
				}
			}

			if (!pathIsConfirmed) {
				if (verb == Packet::VERB_OK) {

					Path *slot = (Path *)0;
					if (np < ZT_MAX_PEER_NETWORK_PATHS) {
						slot = &(_paths[np++]);
					} else {
						uint64_t slotLRmin = 0xffffffffffffffffULL;
						for(unsigned int p=0;p<ZT_MAX_PEER_NETWORK_PATHS;++p) {
							if (_paths[p].lastReceived() <= slotLRmin) {
								slotLRmin = _paths[p].lastReceived();
								slot = &(_paths[p]);
							}
						}
					}
					if (slot) {
						*slot = Path(localAddr,remoteAddr);
						slot->received(now);
#ifdef ZT_ENABLE_CLUSTER
						slot->setClusterSuboptimal(suboptimalPath);
#endif
						_numPaths = np;
						pathIsConfirmed = true;
						_sortPaths(now);
					}

#ifdef ZT_ENABLE_CLUSTER
					if (RR->cluster)
						RR->cluster->broadcastHavePeer(_id);
#endif

				} else {

					/* If this path is not known, send a HELLO. We don't learn
					 * paths without confirming that a bidirectional link is in
					 * fact present, but any packet that decodes and authenticates
					 * correctly is considered valid. */
					if ((now - _lastPathConfirmationSent) >= ZT_MIN_PATH_CONFIRMATION_INTERVAL) {
						_lastPathConfirmationSent = now;
						TRACE("got %s via unknown path %s(%s), confirming...",Packet::verbString(verb),_id.address().toString().c_str(),remoteAddr.toString().c_str());
						sendHELLO(RR,localAddr,remoteAddr,now);
					}

				}
			}
		}
	}	// end _lock

	if (needMulticastGroupAnnounce) {
		const std::vector< SharedPtr<Network> > networks(RR->node->allNetworks());
		for(std::vector< SharedPtr<Network> >::const_iterator n(networks.begin());n!=networks.end();++n)
			(*n)->tryAnnounceMulticastGroupsTo(SharedPtr<Peer>(this));
	}
}

void Peer::sendHELLO(const RuntimeEnvironment *RR,const InetAddress &localAddr,const InetAddress &atAddress,uint64_t now,unsigned int ttl)
{
	// _lock not required here since _id is immutable and nothing else is accessed

	Packet outp(_id.address(),RR->identity.address(),Packet::VERB_HELLO);
	outp.append((unsigned char)ZT_PROTO_VERSION);
	outp.append((unsigned char)ZEROTIER_ONE_VERSION_MAJOR);
	outp.append((unsigned char)ZEROTIER_ONE_VERSION_MINOR);
	outp.append((uint16_t)ZEROTIER_ONE_VERSION_REVISION);
	outp.append(now);
	RR->identity.serialize(outp,false);
	atAddress.serialize(outp);
	outp.append((uint64_t)RR->topology->worldId());
	outp.append((uint64_t)RR->topology->worldTimestamp());

	outp.armor(_key,false); // HELLO is sent in the clear
	RR->antiRec->logOutgoingZT(outp.data(),outp.size());
	RR->node->putPacket(localAddr,atAddress,outp.data(),outp.size(),ttl);
}

bool Peer::doPingAndKeepalive(const RuntimeEnvironment *RR,uint64_t now,int inetAddressFamily)
{
	Path *p = (Path *)0;

	Mutex::Lock _l(_lock);
	if (inetAddressFamily != 0) {
		p = _getBestPath(now,inetAddressFamily);
	} else {
		p = _getBestPath(now);
	}

	if (p) {
		if ((now - p->lastReceived()) >= ZT_PEER_DIRECT_PING_DELAY) {
			//TRACE("PING %s(%s) after %llums/%llums send/receive inactivity",_id.address().toString().c_str(),p->address().toString().c_str(),now - p->lastSend(),now - p->lastReceived());
			sendHELLO(RR,p->localAddress(),p->address(),now);
			p->sent(now);
		} else if (((now - p->lastSend()) >= ZT_NAT_KEEPALIVE_DELAY)&&(!p->reliable())) {
			//TRACE("NAT keepalive %s(%s) after %llums/%llums send/receive inactivity",_id.address().toString().c_str(),p->address().toString().c_str(),now - p->lastSend(),now - p->lastReceived());
			_natKeepaliveBuf += (uint32_t)((now * 0x9e3779b1) >> 1); // tumble this around to send constantly varying (meaningless) payloads
			RR->node->putPacket(p->localAddress(),p->address(),&_natKeepaliveBuf,sizeof(_natKeepaliveBuf));
			p->sent(now);
		} else {
			//TRACE("no PING or NAT keepalive: addr==%s reliable==%d %llums/%llums send/receive inactivity",p->address().toString().c_str(),(int)p->reliable(),now - p->lastSend(),now - p->lastReceived());
		}
		return true;
	}

	return false;
}

void Peer::pushDirectPaths(const RuntimeEnvironment *RR,Path *path,uint64_t now,bool force)
{
#ifdef ZT_ENABLE_CLUSTER
	// Cluster mode disables normal PUSH_DIRECT_PATHS in favor of cluster-based peer redirection
	if (RR->cluster)
		return;
#endif

	Mutex::Lock _l(_lock);

	if (((now - _lastDirectPathPushSent) >= ZT_DIRECT_PATH_PUSH_INTERVAL)||(force)) {
		_lastDirectPathPushSent = now;

		std::vector<InetAddress> dps(RR->node->directPaths());
		if (dps.empty())
			return;

#ifdef ZT_TRACE
		{
			std::string ps;
			for(std::vector<InetAddress>::const_iterator p(dps.begin());p!=dps.end();++p) {
				if (ps.length() > 0)
					ps.push_back(',');
				ps.append(p->toString());
			}
			TRACE("pushing %u direct paths to %s: %s",(unsigned int)dps.size(),_id.address().toString().c_str(),ps.c_str());
		}
#endif

		std::vector<InetAddress>::const_iterator p(dps.begin());
		while (p != dps.end()) {
			Packet outp(_id.address(),RR->identity.address(),Packet::VERB_PUSH_DIRECT_PATHS);
			outp.addSize(2); // leave room for count

			unsigned int count = 0;
			while ((p != dps.end())&&((outp.size() + 24) < ZT_PROTO_MAX_PACKET_LENGTH)) {
				uint8_t addressType = 4;
				switch(p->ss_family) {
					case AF_INET:
						break;
					case AF_INET6:
						addressType = 6;
						break;
					default: // we currently only push IP addresses
						++p;
						continue;
				}

				uint8_t flags = 0;
				/* TODO: path trust is not implemented yet
				switch(p->trust()) {
					default:
						break;
					case Path::TRUST_PRIVACY:
						flags |= 0x04; // no encryption
						break;
					case Path::TRUST_ULTIMATE:
						flags |= (0x04 | 0x08); // no encryption, no authentication (redundant but go ahead and set both)
						break;
				}
				*/

				outp.append(flags);
				outp.append((uint16_t)0); // no extensions
				outp.append(addressType);
				outp.append((uint8_t)((addressType == 4) ? 6 : 18));
				outp.append(p->rawIpData(),((addressType == 4) ? 4 : 16));
				outp.append((uint16_t)p->port());

				++count;
				++p;
			}

			if (count) {
				outp.setAt(ZT_PACKET_IDX_PAYLOAD,(uint16_t)count);
				outp.armor(_key,true);
				path->send(RR,outp.data(),outp.size(),now);
			}
		}
	}
}

bool Peer::resetWithinScope(const RuntimeEnvironment *RR,InetAddress::IpScope scope,uint64_t now)
{
	Mutex::Lock _l(_lock);
	unsigned int np = _numPaths;
	unsigned int x = 0;
	unsigned int y = 0;
	while (x < np) {
		if (_paths[x].address().ipScope() == scope) {
			sendHELLO(RR,_paths[x].localAddress(),_paths[x].address(),now);
		} else {
			_paths[y++] = _paths[x];
		}
		++x;
	}
	_numPaths = y;
	_sortPaths(now);
	return (y < np);
}

void Peer::getBestActiveAddresses(uint64_t now,InetAddress &v4,InetAddress &v6) const
{
	Mutex::Lock _l(_lock);
	uint64_t bestV4 = 0,bestV6 = 0;
	for(unsigned int p=0,np=_numPaths;p<np;++p) {
		if (_paths[p].active(now)) {
			uint64_t lr = _paths[p].lastReceived();
			if (lr) {
				if (_paths[p].address().isV4()) {
					if (lr >= bestV4) {
						bestV4 = lr;
						v4 = _paths[p].address();
					}
				} else if (_paths[p].address().isV6()) {
					if (lr >= bestV6) {
						bestV6 = lr;
						v6 = _paths[p].address();
					}
				}
			}
		}
	}
}

bool Peer::networkMembershipCertificatesAgree(uint64_t nwid,const CertificateOfMembership &com) const
{
	Mutex::Lock _l(_lock);
	const _NetworkCom *ourCom = _networkComs.get(nwid);
	if (ourCom)
		return ourCom->com.agreesWith(com);
	return false;
}

bool Peer::validateAndSetNetworkMembershipCertificate(const RuntimeEnvironment *RR,uint64_t nwid,const CertificateOfMembership &com)
{
	// Sanity checks
	if ((!com)||(com.issuedTo() != _id.address()))
		return false;

	// Return true if we already have this *exact* COM
	{
		Mutex::Lock _l(_lock);
		_NetworkCom *ourCom = _networkComs.get(nwid);
		if ((ourCom)&&(ourCom->com == com))
			return true;
	}

	// Check signature, log and return if cert is invalid
	if (com.signedBy() != Network::controllerFor(nwid)) {
		TRACE("rejected network membership certificate for %.16llx signed by %s: signer not a controller of this network",(unsigned long long)_id,com.signedBy().toString().c_str());
		return false; // invalid signer
	}

	if (com.signedBy() == RR->identity.address()) {

		// We are the controller: RR->identity.address() == controller() == cert.signedBy()
		// So, verify that we signed th cert ourself
		if (!com.verify(RR->identity)) {
			TRACE("rejected network membership certificate for %.16llx self signed by %s: signature check failed",(unsigned long long)_id,com.signedBy().toString().c_str());
			return false; // invalid signature
		}

	} else {

		SharedPtr<Peer> signer(RR->topology->getPeer(com.signedBy()));

		if (!signer) {
			// This would be rather odd, since this is our controller... could happen
			// if we get packets before we've gotten config.
			RR->sw->requestWhois(com.signedBy());
			return false; // signer unknown
		}

		if (!com.verify(signer->identity())) {
			TRACE("rejected network membership certificate for %.16llx signed by %s: signature check failed",(unsigned long long)_id,com.signedBy().toString().c_str());
			return false; // invalid signature
		}
	}

	// If we made it past all those checks, add or update cert in our cert info store
	{
		Mutex::Lock _l(_lock);
		_networkComs.set(nwid,_NetworkCom(RR->node->now(),com));
	}

	return true;
}

bool Peer::needsOurNetworkMembershipCertificate(uint64_t nwid,uint64_t now,bool updateLastPushedTime)
{
	Mutex::Lock _l(_lock);
	uint64_t &lastPushed = _lastPushedComs[nwid];
	const uint64_t tmp = lastPushed;
	if (updateLastPushedTime)
		lastPushed = now;
	return ((now - tmp) >= (ZT_NETWORK_AUTOCONF_DELAY / 2));
}

void Peer::clean(const RuntimeEnvironment *RR,uint64_t now)
{
	Mutex::Lock _l(_lock);

	{
		unsigned int np = _numPaths;
		unsigned int x = 0;
		unsigned int y = 0;
		while (x < np) {
			if (_paths[x].active(now))
				_paths[y++] = _paths[x];
			++x;
		}
		_numPaths = y;
	}

	{
		uint64_t *k = (uint64_t *)0;
		_NetworkCom *v = (_NetworkCom *)0;
		Hashtable< uint64_t,_NetworkCom >::Iterator i(_networkComs);
		while (i.next(k,v)) {
			if ( (!RR->node->belongsToNetwork(*k)) && ((now - v->ts) >= ZT_PEER_NETWORK_COM_EXPIRATION) )
				_networkComs.erase(*k);
		}
	}

	{
		uint64_t *k = (uint64_t *)0;
		uint64_t *v = (uint64_t *)0;
		Hashtable< uint64_t,uint64_t >::Iterator i(_lastPushedComs);
		while (i.next(k,v)) {
			if ((now - *v) > (ZT_NETWORK_AUTOCONF_DELAY * 2))
				_lastPushedComs.erase(*k);
		}
	}
}

struct _SortPathsByQuality
{
	uint64_t _now;
	_SortPathsByQuality(const uint64_t now) : _now(now) {}
	inline bool operator()(const Path &a,const Path &b) const
	{
		const uint64_t qa = (
			((uint64_t)a.active(_now) << 63) |
			(((uint64_t)(a.preferenceRank() & 0xfff)) << 51) |
			((uint64_t)a.lastReceived() & 0x7ffffffffffffULL) );
		const uint64_t qb = (
			((uint64_t)b.active(_now) << 63) |
			(((uint64_t)(b.preferenceRank() & 0xfff)) << 51) |
			((uint64_t)b.lastReceived() & 0x7ffffffffffffULL) );
		return (qb < qa); // invert sense to sort in descending order
	}
};
void Peer::_sortPaths(const uint64_t now)
{
	// assumes _lock is locked
	_lastPathSort = now;
	std::sort(&(_paths[0]),&(_paths[_numPaths]),_SortPathsByQuality(now));
}

Path *Peer::_getBestPath(const uint64_t now)
{
	// assumes _lock is locked
	if ((now - _lastPathSort) >= ZT_PEER_PATH_SORT_INTERVAL)
		_sortPaths(now);
	if (_paths[0].active(now)) {
		return &(_paths[0]);
	} else {
		_sortPaths(now);
		if (_paths[0].active(now))
			return &(_paths[0]);
	}
	return (Path *)0;
}

Path *Peer::_getBestPath(const uint64_t now,int inetAddressFamily)
{
	// assumes _lock is locked
	if ((now - _lastPathSort) >= ZT_PEER_PATH_SORT_INTERVAL)
		_sortPaths(now);
	for(int k=0;k<2;++k) { // try once, and if it fails sort and try one more time
		for(unsigned int i=0;i<_numPaths;++i) {
			if ((_paths[i].active(now))&&((int)_paths[i].address().ss_family == inetAddressFamily))
				return &(_paths[i]);
		}
		_sortPaths(now);
	}
	return (Path *)0;
}

} // namespace ZeroTier
