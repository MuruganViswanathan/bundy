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
#include <dhcpsrv/pool.h>
#include <sstream>

using namespace bundy::asiolink;

namespace bundy {
namespace dhcp {

Pool::Pool(Lease::Type type, const bundy::asiolink::IOAddress& first,
           const bundy::asiolink::IOAddress& last)
    :id_(getNextID()), first_(first), last_(last), type_(type) {
}

bool Pool::inRange(const bundy::asiolink::IOAddress& addr) const {
    return (first_.smallerEqual(addr) && addr.smallerEqual(last_));
}

std::string
Pool::toText() const {
    std::stringstream tmp;
    tmp << "type=" << Lease::typeToText(type_) << ", " << first_
        << "-" << last_;
    return (tmp.str());
}

Pool4::Pool4(const bundy::asiolink::IOAddress& first,
             const bundy::asiolink::IOAddress& last)
:Pool(Lease::TYPE_V4, first, last) {
    // check if specified address boundaries are sane
    if (!first.isV4() || !last.isV4()) {
        bundy_throw(BadValue, "Invalid Pool4 address boundaries: not IPv4");
    }

    if (last < first) {
        bundy_throw(BadValue, "Upper boundary is smaller than lower boundary.");
    }
}

Pool4::Pool4( const bundy::asiolink::IOAddress& prefix, uint8_t prefix_len)
:Pool(Lease::TYPE_V4, prefix, IOAddress("0.0.0.0")) {

    // check if the prefix is sane
    if (!prefix.isV4()) {
        bundy_throw(BadValue, "Invalid Pool4 address boundaries: not IPv4");
    }

    // check if the prefix length is sane
    if (prefix_len == 0 || prefix_len > 32) {
        bundy_throw(BadValue, "Invalid prefix length");
    }

    // Let's now calculate the last address in defined pool
    last_ = lastAddrInPrefix(prefix, prefix_len);
}


Pool6::Pool6(Lease::Type type, const bundy::asiolink::IOAddress& first,
             const bundy::asiolink::IOAddress& last)
    :Pool(type, first, last), prefix_len_(128) {

    // check if specified address boundaries are sane
    if (!first.isV6() || !last.isV6()) {
        bundy_throw(BadValue, "Invalid Pool6 address boundaries: not IPv6");
    }

    if ( (type != Lease::TYPE_NA) && (type != Lease::TYPE_TA) &&
         (type != Lease::TYPE_PD)) {
        bundy_throw(BadValue, "Invalid Pool6 type: " << static_cast<int>(type)
                  << ", must be TYPE_IA, TYPE_TA or TYPE_PD");
    }

    if (last < first) {
        bundy_throw(BadValue, "Upper boundary is smaller than lower boundary.");
        // This check is a bit strict. If we decide that it is too strict,
        // we need to comment it and uncomment lines below.
        // On one hand, letting the user specify 2001::f - 2001::1 is nice, but
        // on the other hand, 2001::1 may be a typo and the user really meant
        // 2001::1:0 (or 1 followed by some hex digit), so a at least a warning
        // would be useful.

        // first_  = last;
        // last_ = first;
    }

    // TYPE_PD is not supported by this constructor. first-last style
    // parameters are for IA and TA only. There is another dedicated
    // constructor for that (it uses prefix/length)
    if ((type != Lease::TYPE_NA) && (type != Lease::TYPE_TA)) {
        bundy_throw(BadValue, "Invalid Pool6 type specified:"
                  << static_cast<int>(type));
    }
}

Pool6::Pool6(Lease::Type type, const bundy::asiolink::IOAddress& prefix,
             uint8_t prefix_len, uint8_t delegated_len /* = 128 */)
    :Pool(type, prefix, IOAddress("::")), prefix_len_(delegated_len) {

    // check if the prefix is sane
    if (!prefix.isV6()) {
        bundy_throw(BadValue, "Invalid Pool6 address boundaries: not IPv6");
    }

    // check if the prefix length is sane
    if (prefix_len == 0 || prefix_len > 128) {
        bundy_throw(BadValue, "Invalid prefix length: " << prefix_len);
    }

    if (prefix_len > delegated_len) {
        bundy_throw(BadValue, "Delegated length (" << static_cast<int>(delegated_len)
                  << ") must be longer than prefix length ("
                  << static_cast<int>(prefix_len) << ")");
    }

    if ( ( (type == Lease::TYPE_NA) || (type == Lease::TYPE_TA)) &&
         (delegated_len != 128)) {
        bundy_throw(BadValue, "For IA or TA pools, delegated prefix length must "
                  << " be 128.");
    }

    /// @todo: We should probably implement checks against weird addresses
    /// here, like ::, starting with fe80, starting with ff etc. .

    // Let's now calculate the last address in defined pool
    last_ = lastAddrInPrefix(prefix, prefix_len);
}

std::string
Pool6::toText() const {
    std::stringstream tmp;
    tmp << "type=" << Lease::typeToText(type_) << ", " << first_
        << "-" << last_ << ", delegated_len="
        << static_cast<int>(prefix_len_);
    return (tmp.str());
}


}; // end of bundy::dhcp namespace
}; // end of bundy namespace
