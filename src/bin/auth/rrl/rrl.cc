// Copyright (C) 2013  Internet Systems Consortium, Inc. ("ISC")
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

#include <auth/rrl/rrl.h>
#include <auth/rrl/rrl_rate.h>
#include <auth/rrl/rrl_table.h>
#include <auth/rrl/rrl_entry.h>
#include <auth/rrl/rrl_name_pool.h>
#include <auth/rrl/rrl_response_type.h>

#include <dns/name.h>
#include <dns/labelsequence.h>
#include <dns/rcode.h>
#include <dns/rrtype.h>
#include <dns/rrclass.h>

#include <exceptions/exceptions.h>

#include <asiolink/io_endpoint.h>

#include <boost/bind.hpp>
#include <boost/functional/hash.hpp>

#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

using isc::asiolink::IOEndpoint;
using namespace isc::dns;

namespace isc {
namespace auth {
namespace rrl {
using namespace detail;

namespace {
void
setMask(void* mask, size_t mask_len, int plen) {
    std::vector<uint8_t> buf;
    while (plen > 8) {
        buf.push_back(0xff);
        plen -= 8;
    }
    if (plen > 0) {
        buf.push_back(0xff << (8 - plen));
    }
    assert(buf.size() <= mask_len);
    buf.insert(buf.end(), mask_len - buf.size(), 0);
    assert(buf.size() == mask_len);

    std::memcpy(mask, &buf[0], mask_len);
}

// Calculate reasonably (though not cryptographically) unpredictable hash
// seed.
uint32_t
getHashSeed(std::time_t now) {
    size_t hash_val = 0;
    boost::hash_combine(hash_val, static_cast<size_t>(now));
    boost::hash_combine(hash_val, static_cast<size_t>(getpid()));

    return (hash_val);
}
}

struct ResponseLimiter::ResponseLimiterImpl {
    ResponseLimiterImpl(size_t max_table_size, int min_table_size,
                        int responses_per_second,
                        int nxdomains_per_second, int errors_per_second,
                        int window, int slip, int ipv4_prefixlen,
                        int ipv6_prefixlen, bool log_only, std::time_t now) :
        table_(max_table_size),
        rates_(responses_per_second, nxdomains_per_second, errors_per_second),
        window_(window), slip_(slip),
        ts_bases_(now, boost::bind(&RRLTable::timestampBaseUpdated, &table_,
                                   _1)),
        log_only_(log_only),
        ipv4_prefixlen_(ipv4_prefixlen), ipv6_prefixlen_(ipv6_prefixlen),
        hash_seed_(getHashSeed(now)),
        log_names_(RRLEntry::createNamePool())
    {
        if (ipv4_prefixlen_ < 0 || ipv4_prefixlen_ > 32) {
            isc_throw(InvalidParameter, "bad IPv4 prefix: " <<
                      ipv4_prefixlen_);
        }
        if (ipv6_prefixlen_ < 0 || ipv6_prefixlen_ > 128) {
            isc_throw(InvalidParameter, "bad IPv6 prefix: " <<
                      ipv6_prefixlen_);
        }
        setMask(&ipv4_mask_, sizeof(ipv4_mask_), ipv4_prefixlen_);
        setMask(&ipv6_mask_, sizeof(ipv6_mask_), ipv6_prefixlen_);
        if (max_table_size < min_table_size) {
            isc_throw(InvalidParameter, "max-table-size (" << max_table_size
                      << ") must not be smaller than min-table-size ("
                      << min_table_size << ")");
        }

        table_.expandEntries(min_table_size);
        table_.expand(now);
    }
    RRLTable table_;
    RRLRate rates_;
    const int window_;
    const int slip_;
    RRLEntry::TimestampBases ts_bases_;
    const bool log_only_;
    const int ipv4_prefixlen_;
    uint32_t ipv4_mask_;
    const int ipv6_prefixlen_;
    uint32_t ipv6_mask_[4];
    const uint32_t hash_seed_;
    boost::scoped_ptr<NamePool> log_names_;
};

ResponseLimiter::ResponseLimiter(size_t max_table_size, size_t min_table_size,
                                 int responses_per_second,
                                 int nxdomains_per_second,
                                 int errors_per_second, int window, int slip,
                                 int ipv4_prefixlen, int ipv6_prefixlen,
                                 bool log_only, std::time_t now) :
    impl_(new ResponseLimiterImpl(max_table_size, min_table_size,
                                  responses_per_second, nxdomains_per_second,
                                  errors_per_second, window, slip,
                                  ipv4_prefixlen, ipv6_prefixlen, log_only,
                                  now))
{}

ResponseLimiter::~ResponseLimiter() {
    delete impl_;
}

int
ResponseLimiter::getResponseRate() const {
    return (impl_->rates_.getRate(RESPONSE_QUERY));
}

int
ResponseLimiter::getNXDOMAINRate() const {
    return (impl_->rates_.getRate(RESPONSE_NXDOMAIN));
}

int
ResponseLimiter::getErrorRate() const {
    return (impl_->rates_.getRate(RESPONSE_ERROR));
}

size_t
ResponseLimiter::getEntryCount() const {
    return (impl_->table_.getEntryCount());
}

int
ResponseLimiter::getWindow() const {
    return (impl_->window_);
}

int
ResponseLimiter::getSlip() const {
    return (impl_->slip_);
}

std::time_t
ResponseLimiter::getCurrentTimestampBase(std::time_t now) const {
    return (impl_->ts_bases_.getCurrentBase(now).first);
}

bool
ResponseLimiter::isLogOnly() const {
    return (impl_->log_only_);
}

uint32_t
ResponseLimiter::getIPv4Mask() const {
    return (impl_->ipv4_mask_);
}

const uint32_t*
ResponseLimiter::getIPv6Mask() const {
    return (impl_->ipv6_mask_);
}

namespace {
inline
ResponseType
convertRcode(const Rcode& rcode) {
    if (rcode == Rcode::NOERROR()) {
        return (RESPONSE_QUERY);
    } else if (rcode == Rcode::NXDOMAIN()) {
        return (RESPONSE_NXDOMAIN);
    }
    return (RESPONSE_ERROR);
}
}

Result
ResponseLimiter::check(const asiolink::IOEndpoint& client_addr,
                       bool is_tcp, const RRClass& qclass,
                       const RRType& qtype, const LabelSequence* qname,
                       const Rcode& rcode, std::time_t now,
                       std::string& /*log_msg*/)
{
#if 0
    // Do possible maintenance for logging.
    impl_->table_.stopLog(now)
#endif

    // Notice TCP responses when scaling limits by qps (not yet)
    // Do not try to rate limit TCP responses.
    if (is_tcp) {
        return (RRL_OK);
    }

    const ResponseType resp_type = convertRcode(rcode);

    // Find the right kind of entry, creating it if necessary.
    // If that is impossible (it's actually never impossible, we assert it),
    // then nothing more can be done.
    RRLEntry* entry =
        impl_->table_.getEntry(RRLKey(client_addr, qtype, qname, qclass,
                                      resp_type, impl_->ipv4_mask_,
                                      impl_->ipv6_mask_, impl_->hash_seed_),
                               impl_->ts_bases_, impl_->rates_, now,
                               impl_->window_);
    assert(entry);

    const Result result =
        entry->updateBalance(impl_->ts_bases_, impl_->rates_, impl_->slip_, 0,
                             now, impl_->window_);
    if (result == RRL_OK) {
        return (RRL_OK);
    }

    // Log occasionally non-OK results
#if 0
    if (entry->dumpLog(qname, *impl_->log_names_, impl_->log_only_,
                       impl_->ipv4_prefixlen_, impl_->ipv6_prefixlen_,
                       log_msg))
    {
        impl_->table_.startLog(entry);
    }
#endif

    return (result);
}

} // namespace rrl
} // namespace auth
} // namespace isc
