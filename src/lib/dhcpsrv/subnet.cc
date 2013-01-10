// Copyright (C) 2012-2013 Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <asiolink/io_address.h>
#include <dhcpsrv/addr_utilities.h>
#include <dhcpsrv/subnet.h>

#include <sstream>

using namespace isc::asiolink;

namespace isc {
namespace dhcp {

Subnet::Subnet(const isc::asiolink::IOAddress& prefix, uint8_t len,
               const Triplet<uint32_t>& t1,
               const Triplet<uint32_t>& t2,
               const Triplet<uint32_t>& valid_lifetime)
    :id_(getNextID()), prefix_(prefix), prefix_len_(len), t1_(t1),
     t2_(t2), valid_(valid_lifetime),
     last_allocated_(lastAddrInPrefix(prefix, len)) {
    if ((prefix.isV6() && len > 128) ||
        (prefix.isV4() && len > 32)) {
        isc_throw(BadValue, "Invalid prefix length specified for subnet: " << len);
    }
}

bool Subnet::inRange(const isc::asiolink::IOAddress& addr) const {
    IOAddress first = firstAddrInPrefix(prefix_, prefix_len_);
    IOAddress last = lastAddrInPrefix(prefix_, prefix_len_);

    return ((first <= addr) && (addr <= last));
}

void
Subnet::addOption(OptionPtr& option, bool persistent,
                  const std::string& option_space) {
    // @todo Once the #2313 is merged we need to use the OptionSpace object to
    // validate the option space name here. For now, let's check that the name
    // is not empty as the empty namespace has a special meaning here - it is
    // returned when desired namespace is not found when getOptions is called.
    if (option_space.empty()) {
        isc_throw(isc::BadValue, "option space name must not be empty");
    }
    validateOption(option);

    OptionContainerPtr container = getOptionDescriptors(option_space);
    // getOptionDescriptors is expected to return the pointer to the
    // valid container. Let's make sure it does by performing an assert.
    assert(container);
    // Actually add the new descriptor.
    container->push_back(OptionDescriptor(option, persistent));
    option_spaces_[option_space] = container;
}

void
Subnet::delOptions() {
    option_spaces_.clear();
}

Subnet::OptionContainerPtr
Subnet::getOptionDescriptors(const std::string& option_space) const {
    // Search the map to get the options container for the particular
    // option space.
    const OptionSpacesPtr::const_iterator& options =
        option_spaces_.find(option_space);
    // If the option space has not been found it means that no option
    // has been configured for this option space yet. Thus we have to
    // return an empty container to the caller.
    if (options == option_spaces_.end()) {
        // The default constructor creates an empty container.
        return (OptionContainerPtr(new OptionContainer()));
    }
    // We found some option container for the option space specified.
    // Let's return a const reference to it.
    return (options->second);
}

Subnet::OptionDescriptor
Subnet::getOptionDescriptor(const std::string& option_space,
                            const uint16_t option_code) {
    OptionContainerPtr options = getOptionDescriptors(option_space);
    if (!options || options->empty()) {
        return (OptionDescriptor(false));
    }
    const OptionContainerTypeIndex& idx = options->get<1>();
    const OptionContainerTypeRange& range = idx.equal_range(option_code);
    if (std::distance(range.first, range.second) == 0) {
        return (OptionDescriptor(false));
    }

    return (*range.first);
}

std::string Subnet::toText() const {
    std::stringstream tmp;
    tmp << prefix_.toText() << "/" << static_cast<unsigned int>(prefix_len_);
    return (tmp.str());
}

Subnet4::Subnet4(const isc::asiolink::IOAddress& prefix, uint8_t length,
                 const Triplet<uint32_t>& t1,
                 const Triplet<uint32_t>& t2,
                 const Triplet<uint32_t>& valid_lifetime)
    :Subnet(prefix, length, t1, t2, valid_lifetime) {
    if (!prefix.isV4()) {
        isc_throw(BadValue, "Non IPv4 prefix " << prefix.toText()
                  << " specified in subnet4");
    }
}

void Subnet4::addPool4(const Pool4Ptr& pool) {
    IOAddress first_addr = pool->getFirstAddress();
    IOAddress last_addr = pool->getLastAddress();

    if (!inRange(first_addr) || !inRange(last_addr)) {
        isc_throw(BadValue, "Pool4 (" << first_addr.toText() << "-" << last_addr.toText()
                  << " does not belong in this (" << prefix_.toText() << "/"
                  << static_cast<int>(prefix_len_) << ") subnet4");
    }

    /// @todo: Check that pools do not overlap

    pools_.push_back(pool);
}

Pool4Ptr Subnet4::getPool4(const isc::asiolink::IOAddress& hint /* = IOAddress("::")*/ ) {
    Pool4Ptr candidate;
    for (Pool4Collection::iterator pool = pools_.begin(); pool != pools_.end(); ++pool) {

        // if we won't find anything better, then let's just use the first pool
        if (!candidate) {
            candidate = *pool;
        }

        // if the client provided a pool and there's a pool that hint is valid in,
        // then let's use that pool
        if ((*pool)->inRange(hint)) {
            return (*pool);
        }
    }
    return (candidate);
}

void
Subnet4::validateOption(const OptionPtr& option) const {
    if (!option) {
        isc_throw(isc::BadValue, "option configured for subnet must not be NULL");
    } else if (option->getUniverse() != Option::V4) {
        isc_throw(isc::BadValue, "expected V4 option to be added to the subnet");
    }
}

bool Subnet4::inPool(const isc::asiolink::IOAddress& addr) const {

    // Let's start with checking if it even belongs to that subnet.
    if (!inRange(addr)) {
        return (false);
    }

    for (Pool4Collection::const_iterator pool = pools_.begin(); pool != pools_.end(); ++pool) {
        if ((*pool)->inRange(addr)) {
            return (true);
        }
    }
    // there's no pool that address belongs to
    return (false);
}

Subnet6::Subnet6(const isc::asiolink::IOAddress& prefix, uint8_t length,
                 const Triplet<uint32_t>& t1,
                 const Triplet<uint32_t>& t2,
                 const Triplet<uint32_t>& preferred_lifetime,
                 const Triplet<uint32_t>& valid_lifetime)
    :Subnet(prefix, length, t1, t2, valid_lifetime),
     preferred_(preferred_lifetime){
    if (!prefix.isV6()) {
        isc_throw(BadValue, "Non IPv6 prefix " << prefix.toText()
                  << " specified in subnet6");
    }
}

void Subnet6::addPool6(const Pool6Ptr& pool) {
    IOAddress first_addr = pool->getFirstAddress();
    IOAddress last_addr = pool->getLastAddress();

    if (!inRange(first_addr) || !inRange(last_addr)) {
        isc_throw(BadValue, "Pool6 (" << first_addr.toText() << "-" << last_addr.toText()
                  << ") does not belong in this (" << prefix_.toText()
                  << "/" << static_cast<int>(prefix_len_)
                  << ") subnet6");
    }
    /// @todo: Check that pools do not overlap

    pools_.push_back(pool);
}

Pool6Ptr Subnet6::getPool6(const isc::asiolink::IOAddress& hint /* = IOAddress("::")*/ ) {
    Pool6Ptr candidate;
    for (Pool6Collection::iterator pool = pools_.begin(); pool != pools_.end(); ++pool) {

        // if we won't find anything better, then let's just use the first pool
        if (!candidate) {
            candidate = *pool;
        }

        // if the client provided a pool and there's a pool that hint is valid in,
        // then let's use that pool
        if ((*pool)->inRange(hint)) {
            return (*pool);
        }
    }
    return (candidate);
}

void
Subnet6::validateOption(const OptionPtr& option) const {
    if (!option) {
        isc_throw(isc::BadValue, "option configured for subnet must not be NULL");
    } else if (option->getUniverse() != Option::V6) {
        isc_throw(isc::BadValue, "expected V6 option to be added to the subnet");
    }
}

bool Subnet6::inPool(const isc::asiolink::IOAddress& addr) const {

    // Let's start with checking if it even belongs to that subnet.
    if (!inRange(addr)) {
        return (false);
    }

    for (Pool6Collection::const_iterator pool = pools_.begin(); pool != pools_.end(); ++pool) {
        if ((*pool)->inRange(addr)) {
            return (true);
        }
    }
    // there's no pool that address belongs to
    return (false);
}

} // end of isc::dhcp namespace
} // end of isc namespace
