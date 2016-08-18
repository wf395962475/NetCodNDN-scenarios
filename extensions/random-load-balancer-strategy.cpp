/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014,  Regents of the University of California,
 *                      Arizona Board of Regents,
 *                      Colorado State University,
 *                      University Pierre & Marie Curie, Sorbonne University,
 *                      Washington University in St. Louis,
 *                      Beijing Institute of Technology,
 *                      The University of Memphis
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "random-load-balancer-strategy.hpp"

#include <ndn-cxx/util/random.hpp>
#include "core/logger.hpp"

// ns-3 specifics for the seed
#include "ns3/simulator.h"

NFD_LOG_INIT("RandomLoadBalancerStrategy");

namespace nfd {
namespace fw {

const Name RandomLoadBalancerStrategy::STRATEGY_NAME("ndn:/localhost/nfd/strategy/random-load-balancer");

RandomLoadBalancerStrategy::RandomLoadBalancerStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder, name)
{
  m_randomGenerator.seed(ns3::Simulator::GetContext());
}

RandomLoadBalancerStrategy::~RandomLoadBalancerStrategy()
{
}

static bool
canForwardToNextHop(shared_ptr<pit::Entry> pitEntry, const fib::NextHop& nexthop)
{
  return pitEntry->canForwardTo(*nexthop.getFace());
}

static bool
hasFaceForForwarding(const fib::NextHopList& nexthops, shared_ptr<pit::Entry>& pitEntry)
{
  return std::find_if(nexthops.begin(), nexthops.end(), bind(&canForwardToNextHop, pitEntry, _1))
         != nexthops.end();
}

void
RandomLoadBalancerStrategy::afterReceiveInterest(const Face& inFace, const Interest& interest,
                                                 shared_ptr<fib::Entry> fibEntry,
                                                 shared_ptr<pit::Entry> pitEntry)
{
  NFD_LOG_TRACE("afterReceiveInterest");

  if (pitEntry->hasUnexpiredOutRecords()) {
    // not a new Interest, don't forward
    return;
  }

  const fib::NextHopList& nexthops = fibEntry->getNextHops();

  // Ensure there is at least 1 Face is available for forwarding
  if (!hasFaceForForwarding(nexthops, pitEntry)) {
    this->rejectPendingInterest(pitEntry);
    return;
  }

  fib::NextHopList::const_iterator selected;
  do {
    std::uniform_int_distribution<> dist(0, nexthops.size() - 1);
    const size_t randomIndex = dist(m_randomGenerator);

    uint64_t currentIndex = 0;

    for (selected = nexthops.begin(); selected != nexthops.end() && currentIndex != randomIndex;
         ++selected, ++currentIndex) {
    }
  } while (!canForwardToNextHop(pitEntry, *selected));

  this->sendInterest(pitEntry, selected->getFace());
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////// NETWORK CODING ////////////////////////////////////////////////////////////////////////////

static bool
canForwardToNextHop_NetworkCoding(shared_ptr<ncft::Entry> ncftEntry, const fib::NextHop& nexthop)
{
  return ncftEntry->canForwardTo(*nexthop.getFace());
}

static bool
hasFaceForForwarding_NetworkCoding(const fib::NextHopList& nexthops, shared_ptr<ncft::Entry>& ncftEntry)
{
  return std::find_if(nexthops.begin(), nexthops.end(), bind(&canForwardToNextHop_NetworkCoding, ncftEntry, _1))
         != nexthops.end();
}

void
RandomLoadBalancerStrategy::afterReceiveInterest_NetworkCoding( const Face& inFace, 
                                                                const Interest& interest,
                                                                shared_ptr<fib::Entry> fibEntry,
                                                                shared_ptr<ncft::Entry> ncftEntry)
{
  NFD_LOG_TRACE("afterReceiveInterest");

  // Check if this Interest should be forwarded or aggregated
  if (ncftEntry->isAggregating(inFace))
	{	
    // Get the total number of interets forwarded and pending for this node (sigma^{p}_{fwd}).
    uint32_t interestsForwarded = ncftEntry->getInterestsForwarded();

    // Get the number of pending interets for inFace (sigma^{p,f}_{pend}).
    uint32_t interetsPending_inFace = ncftEntry->getInterestsPending(inFace);

    // Check if this Interest should be aggregated (sigma^{p}_{fwd} > sigma^{p,f}_{pend})
    if (interestsForwarded > interetsPending_inFace)
    {
    	// Interest will be aggregated
		  return;
    }
  }

  // List of next hops according to FIB
  fib::NextHopList nextHops = fibEntry->getNextHops();
  //fib::NextHopList& nextHops

  // Ensure there is at least 1 Face is available for forwarding
  if (!hasFaceForForwarding_NetworkCoding(nextHops, ncftEntry)) {
    this->rejectPendingInterest_NetworkCoding(ncftEntry);
    return;
  }

  // Congestion map
  std::map<uint32_t,uint32_t> congestionMap;

  // List of OutRecords
  const ncft::OutRecordCollection& outRecords = ncftEntry->getOutRecords();

  // Check the congestion in each Out Record
  for (ncft::OutRecordCollection::const_iterator outRecord = outRecords.begin();
    outRecord != outRecords.end(); ++outRecord) 
  {
    congestionMap.emplace(outRecord->getFace()->getId(), outRecord->getInterestsForwarded());
  }

  // If a face available for forwarding at FIB had no Out Record, add congestion 0
  for (fib::NextHopList::iterator nextHop = nextHops.begin(); 
    nextHop != nextHops.end(); ++nextHop) 
  {
    if (congestionMap.count(nextHop->getFace()->getId()) == 0)
    {
      congestionMap.emplace(nextHop->getFace()->getId(), 0);
    }
  }

  // Find the lowest value of pending interests
  auto l = std::min_element(congestionMap.begin(), congestionMap.end(),
    [] (std::pair<uint32_t,uint32_t> i, std::pair<uint32_t,uint32_t> j) 
    { return i.second < j.second; });

  uint32_t lowest = l->second;

  // Remove all next hops that have more than 'lowest' pending interests
  for (fib::NextHopList::iterator nextHop = nextHops.begin(); 
    nextHop != nextHops.end(); ) 
  {
    if (congestionMap.at(nextHop->getFace()->getId()) != lowest)
    {
      nextHop = nextHops.erase(nextHop);
    } 
    else
    {
      ++nextHop;
    }
  }  

  fib::NextHopList::iterator selected;
  do {
    std::uniform_int_distribution<> dist(0, nextHops.size() - 1);
    const size_t randomIndex = dist(m_randomGenerator);

    uint64_t currentIndex = 0;

    for (selected = nextHops.begin(); selected != nextHops.end() && currentIndex != randomIndex;
         ++selected, ++currentIndex) {
    }
  } while (!canForwardToNextHop_NetworkCoding(ncftEntry, *selected));

  this->sendInterest_NetworkCoding(ncftEntry, selected->getFace());
}

} // namespace fw
} // namespace nfd