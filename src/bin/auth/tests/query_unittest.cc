// Copyright (C) 2010  Internet Systems Consortium, Inc. ("ISC")
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

#include <boost/bind.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/static_assert.hpp>

#include <exceptions/exceptions.h>

#include <dns/masterload.h>
#include <dns/message.h>
#include <dns/master_loader.h>
#include <dns/name.h>
#include <dns/labelsequence.h>
#include <dns/nsec3hash.h>
#include <dns/opcode.h>
#include <dns/rcode.h>
#include <dns/rrcollator.h>
#include <dns/rrttl.h>
#include <dns/rrtype.h>
#include <dns/rdataclass.h>

#include <datasrc/client.h>
#include <datasrc/client_list.h>

#include <auth/query.h>

#include <testutils/dnsmessage_test.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

using namespace std;
using namespace bundy::dns;
using namespace bundy::dns::rdata;
using namespace bundy::datasrc;
using namespace bundy::auth;
using namespace bundy::testutils;

namespace {

// Simple wrapper for a single data source client.
// The list simply delegates all the answers to the single
// client.
class SingletonList : public ClientList {
public:
    SingletonList(DataSourceClient& client) :
        client_(client)
    {}
    virtual FindResult find(const Name& zone, bool exact, bool) const {
        DataSourceClient::FindResult result(client_.findZone(zone));
        // We don't complicate the tests with real life keepers, but we
        // need to put something to the parameter anyway.
        const boost::shared_ptr<ClientList::FindResult::LifeKeeper> keeper;
        switch (result.code) {
            case result::SUCCESS:
                return (FindResult(&client_, result.zone_finder, true,
                                   result.label_count, keeper));
            case result::PARTIALMATCH:
                if (!exact) {
                    return (FindResult(&client_, result.zone_finder, false,
                                       result.label_count, keeper));
                }
            default:
                return (FindResult());
        }
    }
    virtual ConstZoneTableAccessorPtr
    getZoneTableAccessor(const std::string&, bool) const {
        bundy_throw(bundy::NotImplemented,
                  "getZoneTableAccessor not implemented for SingletonList");
    }

private:
    DataSourceClient& client_;
};

// These are commonly used data (auto-generated).  There are some exceptional
// data that are only used in a limited scenario, which are defined separately
// below.
#include <auth/tests/example_base_inc.cc>
#include <auth/tests/example_nsec3_inc.cc>

// This SOA is used in negative responses; its RRTTL is set to SOA's MINTTL
const char* const soa_minttl_txt =
    "example.com. 0 IN SOA . . 1 0 0 0 0\n";

// This is used only in one pathological test case.
const char* const zone_ds_txt =
    "example.com. 3600 IN DS 57855 5 1 "
        "B6DCD485719ADCA18E5F3D48A2331627FDD3 636B\n";

// This is not inside the zone, this is created at runtime
const char* const synthetized_cname_txt =
    "www.dname.example.com. 3600 IN CNAME "
    "www.somethinglong.dnametarget.example.com.\n";

// NSEC3 for wild.example.com (used in wildcard tests, will be added on
// demand not to confuse other tests)
const char* const nsec3_atwild_txt =
    "ji6neoaepv8b5o6k4ev33abha8ht9fgc.example.com. 3600 IN NSEC3 1 1 12 "
    "aabbccdd r53bq7cc2uvmubfu5ocmm6pers9tk9en\n";

// NSEC3 for cnamewild.example.com (used in wildcard tests, will be added on
// demand not to confuse other tests)
const char* const nsec3_atcnamewild_txt =
    "k8udemvp1j2f7eg6jebps17vp3n8i58h.example.com. 3600 IN NSEC3 1 1 12 "
    "aabbccdd r53bq7cc2uvmubfu5ocmm6pers9tk9en\n";

// NSEC3 for *.uwild.example.com (will be added on demand not to confuse
// other tests)
const char* const nsec3_wild_txt =
    "b4um86eghhds6nea196smvmlo4ors995.example.com. 3600 IN NSEC3 1 1 12 "
    "aabbccdd r53bq7cc2uvmubfu5ocmm6pers9tk9en A RRSIG\n";
// NSEC3 for uwild.example.com. (will be added on demand)
const char* const nsec3_uwild_txt =
    "t644ebqk9bibcna874givr6joj62mlhv.example.com. 3600 IN NSEC3 1 1 12 "
    "aabbccdd r53bq7cc2uvmubfu5ocmm6pers9tk9en A RRSIG\n";

// (Secure) delegation data; Delegation without DS record (and both NSEC
// and NSEC3 denying its existence)
// This one will be added on demand
const char* const unsigned_delegation_nsec3_txt =
    "q81r598950igr1eqvc60aedlq66425b5.example.com. 3600 IN NSEC3 1 1 12 "
    "aabbccdd 0p9mhaveqvm6t7vbl5lop2u3t2rp3tom NS RRSIG\n";

// Name of an "empty" zone: used to simulate the case of
// configured-but-available zone (due to load errors, etc).
// Each tested data source client is expected to have this zone (SQLite3
// currently doesn't have this concept so it's skipped)
const char* const EMPTY_ZONE_NAME = "empty.example.org";

// A helper function that generates a textual representation of RRSIG RDATA
// for the given covered type.  The resulting RRSIG may not necessarily make
// sense in terms of the DNSSEC protocol, but for our testing purposes it's
// okay.
string
getCommonRRSIGText(const string& type) {
    return (type +
            string(" 5 3 3600 20000101000000 20000201000000 12345 "
                   "example.com. FAKEFAKEFAKE"));
}

// A helper callback of masterLoad() used in InMemoryZoneFinderTest.
void
setRRset(RRsetPtr rrset, vector<RRsetPtr*>::iterator& it) {
    *(*it) = rrset;
    ++it;
}

// A helper function that converts a textual form of a single RR into a
// RRsetPtr object.  If it's SOA, origin must be set to its owner name;
// otherwise masterLoad() will reject it.
RRsetPtr
textToRRset(const string& text_rrset, const Name& origin = Name::ROOT_NAME()) {
    stringstream ss(text_rrset);
    RRsetPtr rrset;
    vector<RRsetPtr*> rrsets;
    rrsets.push_back(&rrset);
    masterLoad(ss, origin, RRClass::IN(),
               boost::bind(setRRset, _1, rrsets.begin()));
    return (rrset);
}

// Setup for faked NSEC3 hash used throughout this test.
class TestNSEC3Hash : public NSEC3Hash {
private:
    typedef map<Name, string> NSEC3HashMap;
    typedef NSEC3HashMap::value_type NSEC3HashPair;
    NSEC3HashMap hash_map_;
public:
    TestNSEC3Hash() {
        // (Faked) NSEC3 hash map.  For convenience we use hardcoded built-in
        // map instead of calculating and using actual hash.
        // The used hash values are borrowed from RFC5155 examples (they are
        // based on the query name, not that they would correspond directly
        // to the name).
        hash_map_[Name("example.com")] = "0p9mhaveqvm6t7vbl5lop2u3t2rp3tom";
        hash_map_[Name("www.example.com")] =
            "q04jkcevqvmu85r014c7dkba38o0ji5r";
        hash_map_[Name("nxdomain.example.com")] =
            "v644ebqk9bibcna874givr6joj62mlhv";
        hash_map_[Name("nx.domain.example.com")] =
            "v644ebqk9bibcna874givr6joj62mlhv";
        hash_map_[Name("domain.example.com")] =
            "v644ebqk9bibcna874givr6joj62mlhv";
        hash_map_[Name("nxdomain2.example.com")] =
            "q00jkcevqvmu85r014c7dkba38o0ji5r";
        hash_map_[Name("nxdomain3.example.com")] =
            "009mhaveqvm6t7vbl5lop2u3t2rp3tom";
        hash_map_[Name("*.example.com")] =
            "r53bq7cc2uvmubfu5ocmm6pers9tk9en";
        hash_map_[Name("unsigned-delegation.example.com")] =
            "q81r598950igr1eqvc60aedlq66425b5"; // a bit larger than H(www)
        hash_map_[Name("*.uwild.example.com")] =
            "b4um86eghhds6nea196smvmlo4ors995";
        hash_map_[Name("unsigned-delegation-optout.example.com")] =
            "vld46lphhasfapj8og1pglgiasa5o5gt";

        // For wildcard proofs
        hash_map_[Name("wild.example.com")] =
            "ji6neoaepv8b5o6k4ev33abha8ht9fgc";
        hash_map_[Name("y.wild.example.com")] =
            "0p9mhaveqvm6t7vbl5lop2u3t2rp3ton"; // a bit larger than H(<apex>)
        hash_map_[Name("x.y.wild.example.com")] =
            "q04jkcevqvmu85r014c7dkba38o0ji6r"; // a bit larger than H(www)
        hash_map_[Name("cnamewild.example.com")] =
            "k8udemvp1j2f7eg6jebps17vp3n8i58h";
        hash_map_[Name("www.cnamewild.example.com")] =
            "q04jkcevqvmu85r014c7dkba38o0ji6r"; // a bit larger than H(www)

        // For closest encloser proof for www1.uwild.example.com:
        hash_map_[Name("uwild.example.com")] =
            "t644ebqk9bibcna874givr6joj62mlhv";
        hash_map_[Name("www1.uwild.example.com")] =
            "q04jkcevqvmu85r014c7dkba38o0ji6r"; // a bit larger than H(www)

        // For empty-non-terminal derived from insecure delegation (we don't
        // need a hash for the delegation point itself for that test).  the
        // hash for empty name is the same as that for unsigned-delegation
        // above, as the case is similar to that.
        hash_map_[Name("empty.example.com")] =
            "q81r598950igr1eqvc60aedlq66425b5"; // a bit larger than H(www)
    }
    virtual string calculate(const Name& name) const {
        const NSEC3HashMap::const_iterator found = hash_map_.find(name);
        if (found != hash_map_.end()) {
            return (found->second);
        }
        bundy_throw(bundy::Unexpected, "unexpected name for NSEC3 test: "
                  << name);
    }
    virtual string calculate(const LabelSequence& ls) const {
        assert(ls.isAbsolute());
        // This is not very optimal, but it's only going to be used in
        // tests.
        const Name name(ls.toText());
        return (calculate(name));
    }
    virtual bool match(const rdata::generic::NSEC3PARAM&) const {
        return (true);
    }
    virtual bool match(const rdata::generic::NSEC3&) const {
        return (true);
    }
};

class TestNSEC3HashCreator : public bundy::dns::NSEC3HashCreator {
public:
    TestNSEC3HashCreator() {}
    virtual bundy::dns::NSEC3Hash*
    create(const bundy::dns::rdata::generic::NSEC3PARAM&) const {
        return (new TestNSEC3Hash);
    }

    virtual bundy::dns::NSEC3Hash*
    create(const bundy::dns::rdata::generic::NSEC3&) const {
        return (new TestNSEC3Hash);
    }

    virtual bundy::dns::NSEC3Hash*
    create(uint8_t, uint16_t, const uint8_t*, size_t) const {
        return (new TestNSEC3Hash);
    }
};

// This is a mock Zone Finder class for testing.
// It is a derived class of ZoneFinder for the convenient of tests.
// Its find() method emulates the common behavior of protocol compliant
// ZoneFinder classes, but simplifies some minor cases and also supports broken
// behavior.
// For simplicity, most names are assumed to be "in zone"; delegations
// to child zones are identified by the existence of non origin NS records.
// Another special name is "dname.example.com".  Query names under this name
// will result in DNAME.
class MockZoneFinder : public ZoneFinder {
public:
    MockZoneFinder() :
        origin_(Name("example.com")),
        bad_signed_delegation_name_("bad-delegation.example.com"),
        dname_name_("dname.example.com"),
        has_SOA_(true),
        has_apex_NS_(true),
        rrclass_(RRClass::IN()),
        include_rrsig_anyway_(false),
        use_nsec3_(false),
        nsec_name_(origin_),
        nsec3_fake_(NULL),
        nsec3_name_(NULL)
    {
        RRCollator collator(boost::bind(&MockZoneFinder::loadRRset, this, _1));
        MasterLoader loader(TEST_OWN_DATA_BUILDDIR "/example-nsec3.zone",
                            origin_, rrclass_,
                            MasterLoaderCallbacks::getNullCallbacks(),
                            collator.getCallback());
        loader.load();

        empty_nsec_rrset_ = ConstRRsetPtr(new RRset(Name::ROOT_NAME(),
                                                    RRClass::IN(),
                                                    RRType::NSEC(),
                                                    RRTTL(3600)));
    }
    virtual bundy::dns::Name getOrigin() const { return (origin_); }
    virtual bundy::dns::RRClass getClass() const { return (rrclass_); }
    virtual ZoneFinderContextPtr find(const bundy::dns::Name& name,
                                      const bundy::dns::RRType& type,
                                      const FindOptions options =
                                      FIND_DEFAULT);
    virtual ZoneFinderContextPtr findAll(const bundy::dns::Name& name,
                                         std::vector<ConstRRsetPtr>& target,
                                         const FindOptions options =
                                         FIND_DEFAULT);

    virtual ZoneFinder::FindNSEC3Result
    findNSEC3(const Name& name, bool recursive);

    // If false is passed, it makes the zone broken as if it didn't have the
    // SOA.
    void setSOAFlag(bool on) { has_SOA_ = on; }

    // If false is passed, it makes the zone broken as if it didn't have
    // the apex NS.
    void setApexNSFlag(bool on) { has_apex_NS_ = on; }

    // Turn this on if you want it to return RRSIGs regardless of FIND_GLUE_OK
    void setIncludeRRSIGAnyway(bool on) { include_rrsig_anyway_ = on; }

    // Once called, this "faked" result will be returned when NSEC is expected
    // for the specified query name.
    void setNSECResult(const Name& nsec_name, Result code,
                       ConstRRsetPtr rrset)
    {
        nsec_name_ = nsec_name;
        nsec_context_.reset(
            new GenericContext(*this, FIND_DEFAULT, // a fake value
                               ResultContext(code, rrset, RESULT_NSEC_SIGNED),
                               nsec_name.getLabelCount()));
    }

    // Once called, the findNSEC3 will return the provided result for the next
    // query. After that, it'll return to operate normally.
    // NULL disables. Does not take ownership of the pointer (it is generally
    // expected to be a local variable in the test function).
    void setNSEC3Result(const FindNSEC3Result* result,
                        const Name* name = NULL)
    {
        nsec3_fake_ = result;
        nsec3_name_ = name;
    }

    // If true is passed return an empty NSEC3 RRset for some negative
    // answers when DNSSEC is required.
    void setNSEC3Flag(bool on) { use_nsec3_ = on; }

    // This method allows tests to insert new record in the middle of the test.
    //
    // \param record_txt textual RR representation of RR (such as soa_txt, etc)
    void addRecord(const string& record_txt) {
        stringstream record_stream;
        record_stream << record_txt;
        masterLoad(record_stream, origin_, rrclass_,
                   boost::bind(&MockZoneFinder::loadRRset, this, _1));
    }

public:
    // We allow the tests to use these for convenience
    ConstRRsetPtr dname_rrset_; // could be used as an arbitrary bogus RRset
    ConstRRsetPtr empty_nsec_rrset_;

protected:
    // A convenient shortcut.  Will also be used by further derived mocks.
    ZoneFinderContextPtr createContext(FindOptions options,
                                       Result code,
                                       bundy::dns::ConstRRsetPtr rrset,
                                       FindResultFlags flags = RESULT_DEFAULT)
    {
        ConstRRsetPtr rp = stripRRsigs(rrset, options);
        return (ZoneFinderContextPtr(
                    new GenericContext(*this, options,
                                       ResultContext(code, rp, flags),
                                       rrset ?
                                       rrset->getName().getLabelCount() : 0)));
    }

private:
    typedef map<RRType, ConstRRsetPtr> RRsetStore;
    typedef map<Name, RRsetStore> Domains;
    Domains domains_;
    Domains delegations_;
    Domains nsec3_domains_;

    // This is used to identify delegation to a child zone, and used to
    // find a matching entry in delegations_.  Note that first found entry
    // is returned, so it's not a longest match.  Test data must be set up
    // to ensure the first match is always the longest match.
    struct SubdomainMatch {
        SubdomainMatch(const Name& name) : name_(name) {}
        bool operator()(const pair<Name, RRsetStore>& domain_elem) const {
            return (name_ == domain_elem.first ||
                    name_.compare(domain_elem.first).getRelation() ==
                    NameComparisonResult::SUBDOMAIN);
        }
    private:
        const Name& name_;
    };

    void loadRRset(RRsetPtr rrset) {
        // For simplicity we dynamically generate RRSIGs and add them below.
        // The RRSIG RDATA should be consistent with that defined in the
        // zone file.
        if (rrset->getType() == RRType::RRSIG()) {
            return;
        }

        // NSEC3PARAM is not used in the mock data source (and it would confuse
        // non-NSEC3 test cases).
        if (rrset->getType() == RRType::NSEC3PARAM()) {
            return;
        }

        if (rrset->getType() == RRType::NSEC3()) {
            // NSEC3 should go to the dedicated table
            nsec3_domains_[rrset->getName()][rrset->getType()] = rrset;

            // By nature it should have RRSIG.  (We may want to selectively
            // omit this to test pathological cases).
            rrset->addRRsig(RdataPtr(new generic::RRSIG(
                                         getCommonRRSIGText(rrset->getType().
                                                            toText()))));
            return;
        }
        domains_[rrset->getName()][rrset->getType()] = rrset;

        // Remember delegation (NS/DNAME) related RRsets separately.
        if (rrset->getType() == RRType::NS() && rrset->getName() != origin_) {
            delegations_[rrset->getName()][rrset->getType()] = rrset;
        } else if (rrset->getName() == dname_name_ &&
                   rrset->getType() == RRType::DNAME()) {
            dname_rrset_ = rrset;
        }

        // Add some signatures.  For NS, we only have RRSIG for the origin
        // name. For others generate RRSIG unconditionally.  Technically this
        // is wrong because we shouldn't have it for names under a zone
        // cut.  But in our tests that doesn't matter, so we add them
        // just for simplicity.
        // Note that this includes RRSIG for DS with secure delegations.
        // They should have RRSIGs, so that's actually expected data, not just
        // for simplicity.
        if (rrset->getType() != RRType::NS() || rrset->getName() == origin_) {
            rrset->addRRsig(RdataPtr(new generic::RRSIG(
                                         getCommonRRSIGText(rrset->getType().
                                                            toText()))));
        }
    }

    const Name origin_;
    // Names where we delegate somewhere else
    const Name bad_signed_delegation_name_;
    const Name dname_name_;
    bool has_SOA_;
    bool has_apex_NS_;
    const RRClass rrclass_;
    bool include_rrsig_anyway_;
    bool use_nsec3_;
    // The following two will be used for faked NSEC cases
    Name nsec_name_;
    ZoneFinderContextPtr nsec_context_;
    // The following two are for faking bad NSEC3 responses
    // Enabled when not NULL
    const FindNSEC3Result* nsec3_fake_;
    const Name* nsec3_name_;
    TestNSEC3Hash nsec3_hash_;
};

// A helper function that generates a new RRset based on "wild_rrset",
// replacing its owner name with 'real_name'.
ConstRRsetPtr
substituteWild(const AbstractRRset& wild_rrset, const Name& real_name) {
    RRsetPtr rrset(new RRset(real_name, wild_rrset.getClass(),
                             wild_rrset.getType(), wild_rrset.getTTL()));
    // For simplicity we only consider the case with one RDATA (for now)
    rrset->addRdata(wild_rrset.getRdataIterator()->getCurrent());
    ConstRRsetPtr wild_sig = wild_rrset.getRRsig();
    if (wild_sig) {
        RRsetPtr sig(new RRset(real_name, wild_sig->getClass(),
                               wild_sig->getType(), wild_sig->getTTL()));
        sig->addRdata(wild_sig->getRdataIterator()->getCurrent());
        rrset->addRRsig(sig);
    }
    return (rrset);
}

ZoneFinderContextPtr
MockZoneFinder::findAll(const Name& name, std::vector<ConstRRsetPtr>& target,
                        const FindOptions options)
{
    ZoneFinderContextPtr result(find(name, RRType::ANY(), options));
    if (result->code == NXRRSET) {
        const Domains::const_iterator found_domain = domains_.find(name);
        if (!found_domain->second.empty()) {
            for (RRsetStore::const_iterator found_rrset =
                 found_domain->second.begin();
                 found_rrset != found_domain->second.end(); ++found_rrset) {
                // Insert RRs under the domain name into target
                target.push_back(stripRRsigs(found_rrset->second, options));
            }
            return (ZoneFinderContextPtr(
                        new GenericContext(*this, options,
                                           ResultContext(SUCCESS, RRsetPtr()),
                                           target, name.getLabelCount())));
        }
    }

    return (result);
}

ZoneFinder::FindNSEC3Result
MockZoneFinder::findNSEC3(const Name& name, bool recursive) {
    // Do we have a fake result set? If so, use it.
    if (nsec3_fake_ != NULL &&
        (nsec3_name_ == NULL || *nsec3_name_ == name)) {
        const FindNSEC3Result* result(nsec3_fake_);
        return (*result);
    }

    ConstRRsetPtr covering_proof;
    const int labels = name.getLabelCount();

    // For brevity, we assume several things below: maps should have an
    // expected entry when operator[] is used; maps are not empty.
    for (int i = 0; i < labels; ++i) {
        const string hlabel = nsec3_hash_.calculate(name.split(i, labels - i));
        const Name hname = Name(hlabel + ".example.com");
        // We don't use const_iterator so that we can use operator[] below
        Domains::iterator found_domain = nsec3_domains_.lower_bound(hname);

        // If the given hash is larger than the largest stored hash or
        // the first label doesn't match the target, identify the "previous"
        // hash value and remember it as the candidate next closer proof.
        if (found_domain == nsec3_domains_.end() ||
            found_domain->first.split(0, 1).toText(true) != hlabel) {
            // If the given hash is larger or smaller than everything,
            // the covering proof is the NSEC3 that has the largest hash.
            if (found_domain == nsec3_domains_.end() ||
                found_domain == nsec3_domains_.begin()) {
                covering_proof =
                    nsec3_domains_.rbegin()->second[RRType::NSEC3()];
            } else {
                // Otherwise, H(found_domain-1) < given_hash < H(found_domain)
                // The covering proof is the first one.
                covering_proof = (--found_domain)->second[RRType::NSEC3()];
            }
            if (!recursive) {   // in non recursive mode, we are done.
                return (ZoneFinder::FindNSEC3Result(false,
                                                    name.getLabelCount(),
                                                    covering_proof,
                                                    ConstRRsetPtr()));
            }
        } else {                // exact match
            return (ZoneFinder::FindNSEC3Result(
                        true, name.getLabelCount() - i,
                        found_domain->second[RRType::NSEC3()],
                        covering_proof));
        }
    }
    bundy_throw(bundy::Unexpected, "findNSEC3() isn't expected to fail");
}

ZoneFinderContextPtr
MockZoneFinder::find(const Name& name, const RRType& type,
                     const FindOptions options)
{
    // Emulating a broken zone: mandatory apex RRs are missing if specifically
    // configured so (which are rare cases).
    if (name == origin_ && type == RRType::SOA() && !has_SOA_) {
        return (createContext(options, NXDOMAIN, RRsetPtr()));
    } else if (name == origin_ && type == RRType::NS() && !has_apex_NS_) {
        return (createContext(options, NXDOMAIN, RRsetPtr()));
    }

    // Special case for names on or under a zone cut and under DNAME
    Domains::iterator it;
    if ((options & FIND_GLUE_OK) == 0 &&
        (it = find_if(delegations_.begin(), delegations_.end(),
                      SubdomainMatch(name))) != delegations_.end()) {
        ConstRRsetPtr delegation_ns = it->second[RRType::NS()];
        assert(delegation_ns); // should be ensured by how we construct it

        // DS query for the delegated domain (i.e. an exact match) will be
        // handled just like an in-zone case below.  Others result in
        // DELEGATION.
        if (type != RRType::DS() || it->first != name) {
            return (createContext(options, DELEGATION, delegation_ns));
        }
    } else if (name.compare(dname_name_).getRelation() ==
               NameComparisonResult::SUBDOMAIN) {
        return (createContext(options, DNAME, dname_rrset_));
    }

    // normal cases.  names are searched for only per exact-match basis
    // for simplicity.
    const Domains::const_iterator found_domain = domains_.find(name);
    if (found_domain != domains_.end()) {
        // First, try exact match.
        RRsetStore::const_iterator found_rrset =
            found_domain->second.find(type);
        if (found_rrset != found_domain->second.end()) {
            ConstRRsetPtr rrset = ZoneFinder::stripRRsigs(found_rrset->second,
                                                          options);
            return (createContext(options, SUCCESS, rrset));
        }

        // Otherwise, if this domain name has CNAME, return it.
        found_rrset = found_domain->second.find(RRType::CNAME());
        if (found_rrset != found_domain->second.end()) {
            return (createContext(options, CNAME, found_rrset->second));
        }

        // Otherwise it's NXRRSET case...
        // ...but a special pathological case first:
        if (found_domain->first == bad_signed_delegation_name_ &&
            type == RRType::DS()) {
            return (createContext(options, NXDOMAIN, RRsetPtr()));
        }
        // normal cases follow.
        if ((options & FIND_DNSSEC) != 0) {
            if (use_nsec3_) {
                return (createContext(options, NXRRSET, RRsetPtr(),
                                      RESULT_NSEC3_SIGNED));
            }
            found_rrset = found_domain->second.find(RRType::NSEC());
            if (found_rrset != found_domain->second.end()) {
                return (createContext(options, NXRRSET, found_rrset->second,
                                      RESULT_NSEC_SIGNED));
            }
        }
        // If no NSEC is found or DNSSEC isn't specified, behave as if the
        // zone is unsigned.
        return (createContext(options, NXRRSET, RRsetPtr()));
    }

    // query name isn't found in our domains.
    // We first check if the query name is an empty non terminal name
    // of the zone by naive linear search.
    Domains::const_iterator domain;
    for (domain = domains_.begin(); domain != domains_.end(); ++domain) {
        if (name.compare((*domain).first).getRelation() ==
            NameComparisonResult::SUPERDOMAIN) {
            break;
        }
    }
    if (domain != domains_.end()) {
        // The query name is in an empty non terminal node followed by 'domain'
        // (for simplicity we ignore the pathological case of 'domain' is
        // the origin of the zone)
        --domain;               // reset domain to the "previous name"
        if ((options & FIND_DNSSEC) != 0) {
            if (use_nsec3_) {
                return (createContext(options, NXRRSET, RRsetPtr(),
                                      RESULT_NSEC3_SIGNED));
            }
            RRsetStore::const_iterator found_rrset =
                (*domain).second.find(RRType::NSEC());
            if (found_rrset != (*domain).second.end()) {
                return (createContext(options, NXRRSET, found_rrset->second,
                                      RESULT_NSEC_SIGNED));
            }
        }
        return (createContext(options, NXRRSET, RRsetPtr()));
    }

    // Another possibility is wildcard.  For simplicity we only check
    // hardcoded specific cases, ignoring other details such as canceling
    // due to the existence of closer name.
    if ((options & NO_WILDCARD) == 0) {
        const Name wild_suffix(name == Name("x.y.wild.example.com") ?
                               Name("wild.example.com") : name.split(1));
        // Unit Tests use those domains for Wildcard test.
        if (name.equals(Name("www.wild.example.com")) ||
            name.equals(Name("x.y.wild.example.com")) ||
            name.equals(Name("www1.uwild.example.com")) ||
            name.equals(Name("a.t.example.com"))) {
            if (name.compare(wild_suffix).getRelation() ==
                NameComparisonResult::SUBDOMAIN) {
                domain = domains_.find(Name("*").concatenate(wild_suffix));
                // Matched the QNAME
                if (domain != domains_.end()) {
                    RRsetStore::const_iterator found_rrset =
                        domain->second.find(type);
                    // Matched the QTYPE
                    if(found_rrset != domain->second.end()) {
                        return (createContext(options,SUCCESS, substituteWild(
                                                  *found_rrset->second, name),
                                              RESULT_WILDCARD |
                                              (use_nsec3_ ?
                                               RESULT_NSEC3_SIGNED :
                                               RESULT_NSEC_SIGNED)));
                    } else {
                        // No matched QTYPE, this case is for NXRRSET with
                        // WILDCARD
                        if (use_nsec3_) {
                            return (createContext(options, NXRRSET, RRsetPtr(),
                                                  RESULT_WILDCARD |
                                                  RESULT_NSEC3_SIGNED));
                        }
                        const Name new_name =
                            Name("*").concatenate(wild_suffix);
                        found_rrset = domain->second.find(RRType::NSEC());
                        assert(found_rrset != domain->second.end());
                        return (createContext(options, NXRRSET, substituteWild(
                                                  *found_rrset->second,
                                                  new_name),
                                              RESULT_WILDCARD |
                                              RESULT_NSEC_SIGNED));
                    }
                } else {
                    // This is empty non terminal name case on wildcard.
                    const Name empty_name = Name("*").concatenate(wild_suffix);
                    if (use_nsec3_) {
                        return (createContext(options, NXRRSET, RRsetPtr(),
                                              RESULT_WILDCARD |
                                              RESULT_NSEC3_SIGNED));
                    }
                    for (Domains::reverse_iterator it = domains_.rbegin();
                         it != domains_.rend();
                         ++it) {
                        RRsetStore::const_iterator nsec_it;
                        if ((*it).first < empty_name &&
                            (nsec_it = (*it).second.find(RRType::NSEC()))
                            != (*it).second.end()) {
                            return (createContext(options, NXRRSET,
                                                  (*nsec_it).second,
                                                  RESULT_WILDCARD |
                                                  RESULT_NSEC_SIGNED));
                        }
                    }
                }
                return (createContext(options, NXRRSET, RRsetPtr(),
                                      RESULT_WILDCARD));
             }
        }
        const Name cnamewild_suffix("cnamewild.example.com");
        if (name.compare(cnamewild_suffix).getRelation() ==
            NameComparisonResult::SUBDOMAIN) {
            domain = domains_.find(Name("*").concatenate(cnamewild_suffix));
            assert(domain != domains_.end());
            RRsetStore::const_iterator found_rrset =
                domain->second.find(RRType::CNAME());
            assert(found_rrset != domain->second.end());
            return (createContext(options, CNAME,
                                  substituteWild(*found_rrset->second, name),
                                  RESULT_WILDCARD |
                                  (use_nsec3_ ? RESULT_NSEC3_SIGNED :
                                   RESULT_NSEC_SIGNED)));
        }
    }

    // This is an NXDOMAIN case.
    // If we need DNSSEC proof, find the "previous name" that has an NSEC RR
    // and return NXDOMAIN with the found NSEC.  Otherwise, just return the
    // NXDOMAIN code and NULL.  If DNSSEC proof is requested but no NSEC is
    // found, we return NULL, too.  (For simplicity under the test conditions
    // we don't care about pathological cases such as the name is "smaller"
    // than the origin)
    if ((options & FIND_DNSSEC) != 0) {
        if (use_nsec3_) {
            return (createContext(options, NXDOMAIN, RRsetPtr(),
                                  RESULT_NSEC3_SIGNED));
        }

        // Emulate a broken DataSourceClient for some special names.
        if (nsec_context_ && nsec_name_ == name) {
            return (nsec_context_);
        }

        // Normal case
        // XXX: some older g++ complains about operator!= if we use
        // const_reverse_iterator
        for (Domains::reverse_iterator it = domains_.rbegin();
             it != domains_.rend();
             ++it) {
            RRsetStore::const_iterator nsec_it;
            if ((*it).first < name &&
                (nsec_it = (*it).second.find(RRType::NSEC()))
                != (*it).second.end()) {
                return (createContext(options, NXDOMAIN, (*nsec_it).second,
                                      RESULT_NSEC_SIGNED));
            }
        }
    }
    return (createContext(options,NXDOMAIN, RRsetPtr()));
}

enum DataSrcType {
    MOCK,
    INMEMORY,
    SQLITE3
};

boost::shared_ptr<ClientList>
createDataSrcClientList(DataSrcType type, DataSourceClient& client) {
    boost::shared_ptr<ConfigurableClientList> list;
    switch (type) {
    case MOCK:
        return (boost::shared_ptr<ClientList>(new SingletonList(client)));
    case INMEMORY:
        list.reset(new ConfigurableClientList(RRClass::IN()));
        // Configure one normal zone and one "empty" zone.
        list->configure(bundy::data::Element::fromJSON(
                            "[{\"type\": \"MasterFiles\","
                            "  \"cache-enable\": true, "
                            "  \"params\": {\"example.com\": \"" +
                            string(TEST_OWN_DATA_BUILDDIR "/example.zone\",") +
                            + "\"" + EMPTY_ZONE_NAME + "\": \"" +
                            string(TEST_OWN_DATA_BUILDDIR "/nosuchfile.zone") +
                            "\"}}]"), true);
        return (list);
    case SQLITE3:
        // The copy should succeed; if it failed we should notice it in
        // test cases.  However, we check the return value to avoid problems
        // in some glibcs where "system()" is annotated with the "warn unused
        // result" attribute.
        EXPECT_EQ(0, std::system(INSTALL_PROG " -c " TEST_OWN_DATA_BUILDDIR
                                 "/example-base.sqlite3 "
                                 TEST_OWN_DATA_BUILDDIR
                                 "/example-base.sqlite3.copied"));
        list.reset(new ConfigurableClientList(RRClass::IN()));
        list->configure(bundy::data::Element::fromJSON(
                            "[{\"type\": \"sqlite3\","
                            "  \"cache-enable\": false, "
                            "  \"cache-zones\": [], "
                            "  \"params\": {\"database_file\": \"" +
                            string(TEST_OWN_DATA_BUILDDIR
                                   "/example-base.sqlite3.copied") +
                            "\"}}]"), true);
         return (list);
    default:
        bundy_throw(bundy::Unexpected,
                  "Unexpected data source type, should be a bug in test code");
    }
}

class MockClient : public DataSourceClient {
public:
    MockClient() : DataSourceClient("mock") {}

    virtual FindResult findZone(const bundy::dns::Name& origin) const {
        // Identify the next (strictly) larger name than the given 'origin' in
        // the map.  Its predecessor (if any) is the longest matching name
        // if it's either an exact match or a super domain; otherwise there's
        // no match in the map.  See also datasrc/tests/mock_client.cc.

        // Eliminate the case of empty map to simplify the rest of the code
        if (zone_finders_.empty()) {
            return (FindResult(result::NOTFOUND, ZoneFinderPtr(), 0));
        }

        std::map<Name, ZoneFinderPtr>::const_iterator it =
            zone_finders_.upper_bound(origin);
        if (it == zone_finders_.begin()) { // no predecessor
            return (FindResult(result::NOTFOUND, ZoneFinderPtr(), 0));
        }

        --it;                   // get the predecessor
        const result::ResultFlags flags =
            it->second ? result::FLAGS_DEFAULT : result::ZONE_EMPTY;
        const NameComparisonResult compar(it->first.compare(origin));
        switch (compar.getRelation()) {
        case NameComparisonResult::EQUAL:
            return (FindResult(result::SUCCESS, it->second,
                               it->first.getLabelCount(), flags));
        case NameComparisonResult::SUPERDOMAIN:
            return (FindResult(result::PARTIALMATCH, it->second,
                               it->first.getLabelCount(), flags));
        default:
            return (FindResult(result::NOTFOUND, ZoneFinderPtr(), 0));
        }
    }

    virtual ZoneUpdaterPtr getUpdater(const bundy::dns::Name&, bool, bool) const
    {
        bundy_throw(bundy::NotImplemented,
                  "Updater isn't supported in the MockClient");
    }

    virtual std::pair<ZoneJournalReader::Result, ZoneJournalReaderPtr>
    getJournalReader(const bundy::dns::Name&, uint32_t, uint32_t) const {
        bundy_throw(bundy::NotImplemented,
                  "Journaling isn't supported in the MockClient");
    }

    result::Result addZone(ZoneFinderPtr finder) {
        zone_finders_[finder->getOrigin()] = finder;
        return (result::SUCCESS);
    }

    // "configure" a zone with no data.  This will cause the ZONE_EMPTY flag
    // on in finZone().
    result::Result addEmptyZone(const Name& zone_name) {
        zone_finders_[zone_name] = ZoneFinderPtr();
        return (result::SUCCESS);
    }

private:
    // Note that because we no longer have the old RBTree, and the new
    // in-memory DomainTree is not useful as it returns const nodes, we
    // use a std::map instead.
    std::map<Name, ZoneFinderPtr> zone_finders_;
};

class QueryTest : public ::testing::TestWithParam<DataSrcType> {
protected:
    QueryTest() :
        qname(Name("www.example.com")), qclass(RRClass::IN()),
        qtype(RRType::A()), response(Message::RENDER),
        qid(response.getQid()), query_code(Opcode::QUERY().getCode()),
        ns_addrs_and_sig_txt(string(ns_addrs_txt) +
                             "glue.delegation.example.com. 3600 IN RRSIG " +
                             getCommonRRSIGText("A") + "\n" +
                             "glue.delegation.example.com. 3600 IN RRSIG " +
                             getCommonRRSIGText("AAAA") + "\n" +
                             "noglue.example.com. 3600 IN RRSIG " +
                             getCommonRRSIGText("A")),
        base_zone_file(TEST_OWN_DATA_BUILDDIR "/example-base.zone"),
        nsec3_zone_file(TEST_OWN_DATA_BUILDDIR "/example-nsec3.zone"),
        common_zone_file(TEST_OWN_DATA_BUILDDIR "/example-common-inc.zone"),
        rrsets_added_(false)
    {
        // Set up the faked hash calculator.
        setNSEC3HashCreator(&nsec3hash_creator_);

        response.setRcode(Rcode::NOERROR());
        response.setOpcode(Opcode::QUERY());
        // create and add a matching zone.  One is a "broken, empty" zone.
        mock_finder = new MockZoneFinder();
        mock_client.addZone(ZoneFinderPtr(mock_finder));
        mock_client.addEmptyZone(Name(EMPTY_ZONE_NAME));
    }

    virtual void SetUp() {
        // clear the commonly included zone file.
        ASSERT_EQ(0, std::system(INSTALL_PROG " -c " TEST_OWN_DATA_DIR
                                 "/example-common-inc-template.zone "
                                 TEST_OWN_DATA_BUILDDIR
                                 "/example-common-inc.zone"));

        // We create data source clients here, not in the constructor, so this
        // doesn't happen for derived test class.  This also ensures the
        // data source clients are configured after setting NSEC3 hash in case
        // there's dependency.
        list_ = createDataSrcClientList(GetParam(), mock_client);
    }

    virtual void TearDown() {
        // make sure to clear the commonly included zone file to prevent
        // any remaining contents from affecting the next test.
        ASSERT_EQ(0, std::system(INSTALL_PROG " -c " TEST_OWN_DATA_DIR
                                 "/example-common-inc-template.zone "
                                 TEST_OWN_DATA_BUILDDIR
                                 "/example-common-inc.zone"));
    }

    virtual ~QueryTest() {
        // Make sure we reset the hash creator to the default
        setNSEC3HashCreator(NULL);
    }

    bool isEmptyZoneSupported() const {
        // Not all data sources support the concept of empty zones.
        // Specifically for this test, SQLite3-based data source doesn't.
        return (GetParam() != SQLITE3);
    }

    void enableNSEC3(const vector<string>& rrsets_to_add) {
        boost::shared_ptr<ConfigurableClientList> new_list;
        switch (GetParam()) {
        case MOCK:
            mock_finder->setNSEC3Flag(true);
            addRRsets(rrsets_to_add, *list_, "");
            break;
        case INMEMORY:
            addRRsets(rrsets_to_add, *list_, nsec3_zone_file);
            break;
        case SQLITE3:
            ASSERT_EQ(0, std::system(INSTALL_PROG " -c " TEST_OWN_DATA_BUILDDIR
                                     "/example-nsec3.sqlite3 "
                                     TEST_OWN_DATA_BUILDDIR
                                     "/example-nsec3.sqlite3.copied"));
            new_list.reset(new ConfigurableClientList(RRClass::IN()));
            new_list->configure(bundy::data::Element::fromJSON(
                                    "[{\"type\": \"sqlite3\","
                                    "  \"cache-enable\": false, "
                                    "  \"cache-zones\": [], "
                                    "  \"params\": {\"database_file\": \"" +
                                    string(TEST_OWN_DATA_BUILDDIR
                                           "/example-nsec3.sqlite3.copied") +
                                    "\"}}]"), true);
            addRRsets(rrsets_to_add, *new_list, "");
            list_ = new_list;
            break;
        }
    }

    // A helper to add some RRsets to the test zone in the middle of a test
    // case.  The detailed behavior is quite different depending on the
    // data source type, and not all parameters are used in all cases.
    //
    // Note: due to limitation of its implementation, this method doesn't
    // work correctly for in-memory if called more than once.  This condition
    // is explicitly checked so any accidental violation would be noted as a
    // test failure.
    void addRRsets(const vector<string>& rrsets_to_add, ClientList& list,
                   const string& zone_file)
    {
        boost::shared_ptr<ConfigurableClientList> new_list;
        ofstream ofs;

        switch (GetParam()) {
        case MOCK:
            // directly add them to the mock data source; ignore the passed
            // list.
            for (vector<string>::const_iterator it = rrsets_to_add.begin();
                 it != rrsets_to_add.end();
                 ++it) {
                mock_finder->addRecord(*it);
            }
            break;
        case INMEMORY:
            ASSERT_FALSE(rrsets_added_);
            rrsets_added_ = true;

            // dump the RRsets to be added to the placeholder of commonly
            // included zone file (removing any existing contents) and do
            // full reconfiguration.
            ofs.open(common_zone_file.c_str(), ios_base::trunc);
            for (vector<string>::const_iterator it = rrsets_to_add.begin();
                 it != rrsets_to_add.end();
                 ++it) {
                ofs << *it << "\n";
                ofs << createRRSIG(textToRRset(*it))->toText() << "\n";
            }
            ofs.close();

            new_list.reset(new ConfigurableClientList(RRClass::IN()));
            new_list->configure(bundy::data::Element::fromJSON(
                                    "[{\"type\": \"MasterFiles\","
                                    "  \"cache-enable\": true, "
                                    "  \"params\": {\"example.com\": \"" +
                                    zone_file + "\"}}]"), true);
            list_ = new_list;
            break;
        case SQLITE3:
            const Name origin("example.com");
            ZoneUpdaterPtr updater =
                list.find(origin, true, false).dsrc_client_->
                getUpdater(origin, false);
            for (vector<string>::const_iterator it = rrsets_to_add.begin();
                 it != rrsets_to_add.end();
                 ++it) {
                ConstRRsetPtr rrset = textToRRset(*it);
                updater->addRRset(*rrset);
                updater->addRRset(*createRRSIG(rrset));
            }
            updater->commit();
            break;
        }
    }

private:
    // A helper for enableNSEC3, creating an RRSIG RRset for the corresponding
    // non-sig RRset, using the commonly used parameters.
    static ConstRRsetPtr createRRSIG(ConstRRsetPtr rrset) {
        RRsetPtr sig_rrset(new RRset(rrset->getName(), rrset->getClass(),
                                     RRType::RRSIG(), rrset->getTTL()));
        sig_rrset->addRdata(generic::RRSIG(
                                getCommonRRSIGText(rrset->getType().
                                                   toText())));
        return (sig_rrset);
    }

protected:
    MockZoneFinder* mock_finder;
    MockClient mock_client;

    boost::shared_ptr<ClientList> list_;
    const Name qname;
    const RRClass qclass;
    const RRType qtype;
    Message response;
    const qid_t qid;
    const uint16_t query_code;
    const string ns_addrs_and_sig_txt; // convenient shortcut
    Query query;
    TestNSEC3Hash nsec3_hash_;
    vector<string> rrsets_to_add_;
    const string base_zone_file;
private:
    const string nsec3_zone_file;
    const string common_zone_file;
    const TestNSEC3HashCreator nsec3hash_creator_;
    bool rrsets_added_;
};

// We test the in-memory and SQLite3 data source implementations.  SQLite3
// will require a loadable module, which doesn't work with static link for
// all platforms.
INSTANTIATE_TEST_CASE_P(, QueryTest,
                        ::testing::Values(MOCK, INMEMORY
#ifndef USE_STATIC_LINK
                                          , SQLITE3
#endif
                            ));

// This inherit the QueryTest cases except for the parameterized setup;
// it's intended to be used selected test cases that only work for mock
// data sources either because of some limitation or because of the type of
// tests (relying on a "broken" data source behavior that can't happen with
// production data source implementations).
class QueryTestForMockOnly : public QueryTest {
protected:
    // Override SetUp() to avoid parameterized setup
    virtual void SetUp() {
        list_ = createDataSrcClientList(MOCK, mock_client);
    }
};

// A wrapper to check resulting response message commonly used in
// tests below.
// check_origin needs to be specified only when the authority section has
// an SOA RR.  The interface is not generic enough but should be okay
// for our test cases in practice.
void
responseCheck(Message& response, const bundy::dns::Rcode& rcode,
              unsigned int flags, const unsigned int ancount,
              const unsigned int nscount, const unsigned int arcount,
              const char* const expected_answer,
              const char* const expected_authority,
              const char* const expected_additional,
              const Name& check_origin = Name::ROOT_NAME())
{
    // In our test cases QID, Opcode, and QDCOUNT should be constant, so
    // we don't bother the test cases specifying these values.
    headerCheck(response, response.getQid(), rcode, Opcode::QUERY().getCode(),
                flags, 0, ancount, nscount, arcount);
    if (expected_answer != NULL) {
        rrsetsCheck(expected_answer,
                    response.beginSection(Message::SECTION_ANSWER),
                    response.endSection(Message::SECTION_ANSWER),
                    check_origin);
    }
    if (expected_authority != NULL) {
        rrsetsCheck(expected_authority,
                    response.beginSection(Message::SECTION_AUTHORITY),
                    response.endSection(Message::SECTION_AUTHORITY),
                    check_origin);
    }
    if (expected_additional != NULL) {
        rrsetsCheck(expected_additional,
                    response.beginSection(Message::SECTION_ADDITIONAL),
                    response.endSection(Message::SECTION_ADDITIONAL));
    }
}

TEST_P(QueryTest, noZone) {
    // There's no zone in the memory datasource.  So the response should have
    // REFUSED.
    MockClient empty_mock_client;
    SingletonList empty_list(empty_mock_client);
    EXPECT_NO_THROW(query.process(empty_list, qname, qtype, response));
    EXPECT_EQ(Rcode::REFUSED(), response.getRcode());
}

TEST_P(QueryTest, emptyZone) {
    // Query for an "empty (broken)" zone.  If the concept is supported by
    // the underlying data source, the result should be SERVFAIL; otherwise
    // it would be handled as a nonexistent zone, resulting in REFUSED.
    const Rcode expected_rcode =
        isEmptyZoneSupported() ? Rcode::SERVFAIL() : Rcode::REFUSED();

    query.process(*list_, Name(EMPTY_ZONE_NAME), qtype, response);
    responseCheck(response, expected_rcode, 0, 0, 0, 0, NULL, NULL, NULL);

    // Same for the partial match case
    response.clear(bundy::dns::Message::RENDER);
    response.setRcode(Rcode::NOERROR());
    response.setOpcode(Opcode::QUERY());
    query.process(*list_, Name(string("www.") + EMPTY_ZONE_NAME), qtype,
                  response);
    responseCheck(response, expected_rcode, 0, 0, 0, 0, NULL, NULL, NULL);
}

TEST_P(QueryTest, exactMatch) {
    EXPECT_NO_THROW(query.process(*list_, qname, qtype, response));
    // find match rrset
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 3,
                  www_a_txt, zone_ns_txt, ns_addrs_txt);
}

TEST_P(QueryTest, exactMatchMultipleQueries) {
    EXPECT_NO_THROW(query.process(*list_, qname, qtype, response));
    // find match rrset
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 3,
                  www_a_txt, zone_ns_txt, ns_addrs_txt);

    // clean up response for second query
    response.clear(bundy::dns::Message::RENDER);
    response.setRcode(Rcode::NOERROR());
    response.setOpcode(Opcode::QUERY());
    EXPECT_NO_THROW(query.process(*list_, qname, qtype, response));
    // find match rrset
    SCOPED_TRACE("Second query");
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 3,
                  www_a_txt, zone_ns_txt, ns_addrs_txt);
}

TEST_P(QueryTest, qtypeIsRRSIG) {
    // Directly querying for RRSIGs should result in rcode=REFUSED.
    EXPECT_NO_THROW(query.process(*list_, qname, RRType::RRSIG(), response));
    responseCheck(response, Rcode::REFUSED(), AA_FLAG, 0, 0, 0,
                  "", "", "");
}

TEST_P(QueryTest, exactMatchIgnoreSIG) {
    // Check that we do not include the RRSIG when not requested even when
    // we receive it from the data source.
    mock_finder->setIncludeRRSIGAnyway(true);
    EXPECT_NO_THROW(query.process(*list_, qname, qtype, response));
    // find match rrset
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 3,
                  www_a_txt, zone_ns_txt, ns_addrs_txt);
}

TEST_P(QueryTest, dnssecPositive) {
    // Just like exactMatch, but the signatures should be included as well
    EXPECT_NO_THROW(query.process(*list_, qname, qtype, response, true));
    // find match rrset
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 2, 4, 6,
                  (www_a_txt + std::string("www.example.com. 3600 IN RRSIG "
                                           "A 5 3 3600 20000101000000 "
                                           "20000201000000 12345 example.com. "
                                           "FAKEFAKEFAKE\n")).c_str(),
                  (zone_ns_txt + std::string("example.com. 3600 IN RRSIG NS 5 "
                                             "3 3600 20000101000000 "
                                             "20000201000000 12345 "
                                             "example.com. FAKEFAKEFAKE\n")).
                  c_str(),
                  ns_addrs_and_sig_txt.c_str());
}

TEST_P(QueryTest, exactAddrMatch) {
    // find match rrset, omit additional data which has already been provided
    // in the answer section from the additional.
    EXPECT_NO_THROW(query.process(*list_,
                                  Name("noglue.example.com"),
                                  qtype, response));

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 2,
                  "noglue.example.com. 3600 IN A 192.0.2.53\n", zone_ns_txt,
                  "glue.delegation.example.com. 3600 IN A 192.0.2.153\n"
                  "glue.delegation.example.com. 3600 IN AAAA 2001:db8::53\n");
}

TEST_P(QueryTest, apexNSMatch) {
    // find match rrset, omit authority data which has already been provided
    // in the answer section from the authority section.
    EXPECT_NO_THROW(query.process(*list_, Name("example.com"),
                                  RRType::NS(), response));

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 3, 0, 3,
                  zone_ns_txt, NULL, ns_addrs_txt);
}

// test type any query logic
TEST_P(QueryTest, exactAnyMatch) {
    // find match rrset, omit additional data which has already been provided
    // in the answer section from the additional.
    EXPECT_NO_THROW(query.process(*list_, Name("noglue.example.com"),
                                  RRType::ANY(), response));

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 2, 3, 2,
                  (string("noglue.example.com. 3600 IN A 192.0.2.53\n") +
                   string(nsec_nxdomain_txt)).c_str(),
                  zone_ns_txt,
                  "glue.delegation.example.com. 3600 IN A 192.0.2.153\n"
                  "glue.delegation.example.com. 3600 IN AAAA 2001:db8::53\n");
}

TEST_P(QueryTest, apexAnyMatch) {
    // find match rrset, omit additional data which has already been provided
    // in the answer section from the additional.
    EXPECT_NO_THROW(query.process(*list_, Name("example.com"),
                                  RRType::ANY(), response));
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 5, 0, 3,
                  (string(soa_txt) + string(zone_ns_txt) +
                   string(nsec_apex_txt)).c_str(),
                  NULL, ns_addrs_txt, mock_finder->getOrigin());
}

TEST_P(QueryTest, mxANYMatch) {
    EXPECT_NO_THROW(query.process(*list_, Name("mx.example.com"),
                                  RRType::ANY(), response));
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 4, 3, 4,
                  (string(mx_txt) + string(nsec_mx_txt)).c_str(), zone_ns_txt,
                  (string(ns_addrs_txt) + string(www_a_txt)).c_str());
}

TEST_P(QueryTest, glueANYMatch) {
    EXPECT_NO_THROW(query.process(*list_, Name("delegation.example.com"),
                                  RRType::ANY(), response));
    responseCheck(response, Rcode::NOERROR(), 0, 0, 4, 3,
                  NULL, delegation_txt, ns_addrs_txt);
}

TEST_P(QueryTest, nodomainANY) {
    EXPECT_NO_THROW(query.process(*list_, Name("nxdomain.example.com"),
                                  RRType::ANY(), response));
    responseCheck(response, Rcode::NXDOMAIN(), AA_FLAG, 0, 1, 0,
                  NULL, soa_minttl_txt, NULL, mock_finder->getOrigin());
}

// This tests that when we need to look up Zone's apex NS records for
// authoritative answer, and there is no apex NS records. It should
// throw in that case.
//
// This only works with mock data source (for production datasrc the
// post-load would reject such a zone)
TEST_F(QueryTestForMockOnly, noApexNS) {
    // Disable apex NS record
    mock_finder->setApexNSFlag(false);

    EXPECT_THROW(query.process(*list_, Name("noglue.example.com"), qtype,
                               response), Query::NoApexNS);
    // We don't look into the response, as it threw
}

TEST_P(QueryTest, delegation) {
    EXPECT_NO_THROW(query.process(*list_,
                                  Name("delegation.example.com"),
                                  qtype, response));

    responseCheck(response, Rcode::NOERROR(), 0, 0, 4, 3,
                  NULL, delegation_txt, ns_addrs_txt);
}

TEST_P(QueryTest, delegationWithDNSSEC) {
    // Similar to the previous one, but with requesting DNSSEC.
    // In this case the parent zone would behave as unsigned, so the result
    // should be just like non DNSSEC delegation.
    query.process(*list_, Name("www.nosec-delegation.example.com"),
                  qtype, response, true);

    responseCheck(response, Rcode::NOERROR(), 0, 0, 1, 0,
                  NULL, nosec_delegation_txt, NULL);
}

TEST_P(QueryTest, secureDelegation) {
    EXPECT_NO_THROW(query.process(*list_,
                                  Name("foo.signed-delegation.example.com"),
                                  qtype, response, true));

    // Should now contain RRSIG and DS record as well.
    responseCheck(response, Rcode::NOERROR(), 0, 0, 3, 0,
                  NULL,
                  (string(signed_delegation_txt) +
                   string(signed_delegation_ds_txt) +
                   string("signed-delegation.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("DS")).c_str(),
                  NULL);
}

TEST_P(QueryTest, secureUnsignedDelegation) {
    EXPECT_NO_THROW(query.process(*list_,
                                  Name("foo.unsigned-delegation.example.com"),
                                  qtype, response, true));

    // Should now contain RRSIG and NSEC record as well.
    responseCheck(response, Rcode::NOERROR(), 0, 0, 3, 0,
                  NULL,
                  (string(unsigned_delegation_txt) +
                   string(unsigned_delegation_nsec_txt) +
                   string("unsigned-delegation.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC")).c_str(),
                  NULL);
}

TEST_P(QueryTest, secureUnsignedDelegationWithNSEC3) {
    // Similar to the previous case, but the zone is signed with NSEC3,
    // and this delegation is NOT an optout.
    rrsets_to_add_.push_back(unsigned_delegation_nsec3_txt);
    enableNSEC3(rrsets_to_add_);

    query.process(*list_,
                  Name("foo.unsigned-delegation.example.com"),
                  qtype, response, true);

    // The response should contain the NS and matching NSEC3 with its RRSIG
    const Name insecurechild_name("unsigned-delegation.example.com");
    responseCheck(response, Rcode::NOERROR(), 0, 0, 3, 0,
                  NULL,
                  (string(unsigned_delegation_txt) +
                   string(unsigned_delegation_nsec3_txt) +
                   nsec3_hash_.calculate(insecurechild_name) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3")).c_str(),
                  NULL);
}

TEST_P(QueryTest, secureUnsignedDelegationWithNSEC3OptOut) {
    // Similar to the previous case, but the delegation is an optout.
    enableNSEC3(rrsets_to_add_);

    query.process(*list_,
                  Name("foo.unsigned-delegation.example.com"),
                  qtype, response, true);

    // The response should contain the NS and the closest provable encloser
    // proof (and their RRSIGs).  The closest encloser is the apex (origin),
    // and with our faked hash the covering NSEC3 for the next closer
    // (= child zone name) is that for www.example.com.
    responseCheck(response, Rcode::NOERROR(), 0, 0, 5, 0,
                  NULL,
                  (string(unsigned_delegation_txt) +
                   string(nsec3_apex_txt) +
                   nsec3_hash_.calculate(mock_finder->getOrigin()) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3") + "\n" +
                   string(nsec3_www_txt) +
                   nsec3_hash_.calculate(Name("www.example.com")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3")).c_str(),
                  NULL);
}

TEST_F(QueryTestForMockOnly, badSecureDelegation) {
    // This is a broken data source scenario; works only with mock.

    // Test whether exception is raised if DS query at delegation results in
    // something different than SUCCESS or NXRRSET
    EXPECT_THROW(query.process(*list_,
                               Name("bad-delegation.example.com"),
                               qtype, response, true), Query::BadDS);

    // But only if DNSSEC is requested (it shouldn't even try to look for
    // the DS otherwise)
    EXPECT_NO_THROW(query.process(*list_,
                                  Name("bad-delegation.example.com"),
                                  qtype, response));
}

TEST_P(QueryTest, nxdomain) {
    EXPECT_NO_THROW(query.process(*list_,
                                  Name("nxdomain.example.com"), qtype,
                                  response));
    responseCheck(response, Rcode::NXDOMAIN(), AA_FLAG, 0, 1, 0,
                  NULL, soa_minttl_txt, NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, nxdomainWithNSEC) {
    // NXDOMAIN with DNSSEC proof.  We should have SOA, NSEC that proves
    // NXDOMAIN and NSEC that proves nonexistence of matching wildcard,
    // as well as their RRSIGs.
    EXPECT_NO_THROW(query.process(*list_,
                                  Name("nxdomain.example.com"), qtype,
                                  response, true));
    responseCheck(response, Rcode::NXDOMAIN(), AA_FLAG, 0, 6, 0,
                  NULL, (string(soa_minttl_txt) +
                         string("example.com. 0 IN RRSIG ") +
                         getCommonRRSIGText("SOA") + "\n" +
                         string(nsec_nxdomain_txt) + "\n" +
                         string("noglue.example.com. 3600 IN RRSIG ") +
                         getCommonRRSIGText("NSEC") + "\n" +
                         string(nsec_apex_txt) + "\n" +
                         string("example.com. 3600 IN RRSIG ") +
                         getCommonRRSIGText("NSEC")).c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, nxdomainWithNSEC2) {
    // See comments about no_txt.  In this case the best possible wildcard
    // is derived from the next domain of the NSEC that proves NXDOMAIN, and
    // the NSEC to provide the non existence of wildcard is different from
    // the first NSEC.
    query.process(*list_, Name("!.no.example.com"), qtype, response,
                  true);
    responseCheck(response, Rcode::NXDOMAIN(), AA_FLAG, 0, 6, 0,
                  NULL, (string(soa_minttl_txt) +
                         string("example.com. 0 IN RRSIG ") +
                         getCommonRRSIGText("SOA") + "\n" +
                         string(nsec_mx_txt) + "\n" +
                         string("mx.example.com. 3600 IN RRSIG ") +
                         getCommonRRSIGText("NSEC") + "\n" +
                         string(nsec_no_txt) + "\n" +
                         string("&.no.example.com. 3600 IN RRSIG ") +
                         getCommonRRSIGText("NSEC")).c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, nxdomainWithNSECDuplicate) {
    // See comments about nz_txt.  In this case we only need one NSEC,
    // which proves both NXDOMAIN and the non existence of wildcard.
    query.process(*list_, Name("nx.no.example.com"), qtype, response,
                  true);
    responseCheck(response, Rcode::NXDOMAIN(), AA_FLAG, 0, 4, 0,
                  NULL, (string(soa_minttl_txt) +
                         string("example.com. 0 IN RRSIG ") +
                         getCommonRRSIGText("SOA") + "\n" +
                         string(nsec_no_txt) + "\n" +
                         string("&.no.example.com. 3600 IN RRSIG ") +
                         getCommonRRSIGText("NSEC")).c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_F(QueryTestForMockOnly, nxdomainBadNSEC1) {
    // This is a broken data source scenario; works only with mock.

    // ZoneFinder::find() returns NXDOMAIN with non NSEC RR.
    mock_finder->setNSECResult(Name("badnsec.example.com"),
                               ZoneFinder::NXDOMAIN,
                               mock_finder->dname_rrset_);
    EXPECT_THROW(query.process(*list_, Name("badnsec.example.com"),
                               qtype, response, true),
                 std::bad_cast);
}

TEST_F(QueryTestForMockOnly, nxdomainBadNSEC2) {
    // This is a broken data source scenario; works only with mock.

    // ZoneFinder::find() returns NXDOMAIN with an empty NSEC RR.
    mock_finder->setNSECResult(Name("emptynsec.example.com"),
                               ZoneFinder::NXDOMAIN,
                               mock_finder->empty_nsec_rrset_);
    EXPECT_THROW(query.process(*list_, Name("emptynsec.example.com"),
                               qtype, response, true),
                 Query::BadNSEC);
}

TEST_F(QueryTestForMockOnly, nxdomainBadNSEC) {
    // This is a broken data source scenario; works only with mock.

    // "no-wildcard proof" returns SUCCESS.  it should be NXDOMAIN.
    mock_finder->setNSECResult(Name("*.example.com"),
                               ZoneFinder::SUCCESS,
                               mock_finder->dname_rrset_);
    EXPECT_THROW(query.process(*list_, Name("nxdomain.example.com"),
                               qtype, response, true),
                 Query::BadNSEC);
}

TEST_F(QueryTestForMockOnly, nxdomainBadNSEC4) {
    // This is a broken data source scenario; works only with mock.

    // "no-wildcard proof" doesn't return RRset.
    mock_finder->setNSECResult(Name("*.example.com"),
                               ZoneFinder::NXDOMAIN, ConstRRsetPtr());
    EXPECT_THROW(query.process(*list_, Name("nxdomain.example.com"),
                               qtype, response, true),
                 Query::BadNSEC);
}

TEST_F(QueryTestForMockOnly, nxdomainBadNSEC5) {
    // This is a broken data source scenario; works only with mock.

    // "no-wildcard proof" returns non NSEC.
    mock_finder->setNSECResult(Name("*.example.com"),
                               ZoneFinder::NXDOMAIN,
                               mock_finder->dname_rrset_);
    // This is a bit odd, but we'll simply include the returned RRset.
    query.process(*list_, Name("nxdomain.example.com"), qtype,
                  response, true);
    responseCheck(response, Rcode::NXDOMAIN(), AA_FLAG, 0, 6, 0,
                  NULL, (string(soa_minttl_txt) +
                         string("example.com. 0 IN RRSIG ") +
                         getCommonRRSIGText("SOA") + "\n" +
                         string(nsec_nxdomain_txt) + "\n" +
                         string("noglue.example.com. 3600 IN RRSIG ") +
                         getCommonRRSIGText("NSEC") + "\n" +
                         dname_txt + "\n" +
                         string("dname.example.com. 3600 IN RRSIG ") +
                         getCommonRRSIGText("DNAME")).c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_F(QueryTestForMockOnly, nxdomainBadNSEC6) {
    // This is a broken data source scenario; works only with mock.

    // "no-wildcard proof" returns empty NSEC.
    mock_finder->setNSECResult(Name("*.example.com"),
                               ZoneFinder::NXDOMAIN,
                               mock_finder->empty_nsec_rrset_);
    EXPECT_THROW(query.process(*list_, Name("nxdomain.example.com"),
                               qtype, response, true),
                 Query::BadNSEC);
}

TEST_P(QueryTest, nxrrset) {
    EXPECT_NO_THROW(query.process(*list_, Name("www.example.com"),
                                  RRType::TXT(), response));

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 1, 0,
                  NULL, soa_minttl_txt, NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, nxrrsetWithNSEC) {
    // NXRRSET with DNSSEC proof.  We should have SOA, NSEC that proves the
    // NXRRSET and their RRSIGs.
    query.process(*list_, Name("www.example.com"), RRType::TXT(),
                  response, true);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 4, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   string(nsec_www_txt) + "\n" +
                   string("www.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC")).c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, emptyNameWithNSEC) {
    // Empty non terminal with DNSSEC proof.  This is one of the cases of
    // Section 3.1.3.2 of RFC4035.
    // mx.example.com. NSEC &.no.example.com. proves no.example.com. is a
    // non empty terminal node.  Note that it also implicitly proves there
    // should be no closer wildcard match (because the empty name is an
    // exact match), so we only need one NSEC.
    // From the point of the Query::process(), this is actually no different
    // from the other NXRRSET case, but we check that explicitly just in case.
    query.process(*list_, Name("no.example.com"), RRType::A(),
                  response, true);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 4, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   string(nsec_mx_txt) + "\n" +
                   string("mx.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC")).c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, nxrrsetWithoutNSEC) {
    // NXRRSET with DNSSEC proof requested, but there's no NSEC at that node.
    // This is an unexpected event (if the zone is supposed to be properly
    // signed with NSECs), but we accept and ignore the oddity.
    query.process(*list_, Name("nonsec.example.com"), RRType::TXT(),
                  response, true);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 2, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n").c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, wildcardNSEC) {
    // The qname matches *.wild.example.com.  The response should contain
    // an NSEC that proves the non existence of a closer name.
    query.process(*list_, Name("www.wild.example.com"), RRType::A(),
                  response, true);
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 2, 6, 6,
                  (string(wild_txt).replace(0, 1, "www") +
                   string("www.wild.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("A") + "\n").c_str(),
                  (zone_ns_txt + string("example.com. 3600 IN RRSIG NS 5 "
                                        "3 3600 20000101000000 "
                                        "20000201000000 12345 "
                                        "example.com. FAKEFAKEFAKE\n") +
                   string(nsec_wild_txt) +
                   string("*.wild.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC") + "\n").c_str(),
                  NULL, // we are not interested in additionals in this test
                  mock_finder->getOrigin());
}

TEST_P(QueryTest, CNAMEwildNSEC) {
    // Similar to the previous case, but the matching wildcard record is
    // CNAME.
    query.process(*list_, Name("www.cnamewild.example.com"),
                  RRType::A(), response, true);
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 2, 2, 0,
                  (string(cnamewild_txt).replace(0, 1, "www") +
                   string("www.cnamewild.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("CNAME") + "\n").c_str(),
                  (string(nsec_cnamewild_txt) +
                   string("*.cnamewild.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC") + "\n").c_str(),
                  NULL, // we are not interested in additionals in this test
                  mock_finder->getOrigin());
}

TEST_P(QueryTest, wildcardNSEC3) {
    // Similar to wildcardNSEC, but the zone is signed with NSEC3.
    // The next closer is y.wild.example.com, the covering NSEC3 for it
    // is (in our setup) the NSEC3 for the apex.
    //
    // Adding NSEC3 for wild.example.com, which will be used in the middle
    // of identifying the next closer name.
    rrsets_to_add_.push_back(nsec3_atwild_txt);
    enableNSEC3(rrsets_to_add_);

    query.process(*list_, Name("x.y.wild.example.com"), RRType::A(),
                  response, true);
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 2, 6, 6,
                  (string(wild_txt).replace(0, 1, "x.y") +
                   string("x.y.wild.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("A") + "\n").c_str(),
                  // 3 NSes and their RRSIG
                  (zone_ns_txt + string("example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NS") + "\n" +
                   // NSEC3 for the wildcard proof and its RRSIG
                   string(nsec3_apex_txt) +
                   nsec3_hash_.calculate(Name("example.com.")) +
                   string(".example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC3")).c_str(),
                  NULL, // we are not interested in additionals in this test
                  mock_finder->getOrigin());
}

TEST_P(QueryTest, CNAMEwildNSEC3) {
    // Similar to CNAMEwildNSEC, but with NSEC3.
    // The next closer is qname itself, the covering NSEC3 for it
    // is (in our setup) the NSEC3 for the www.example.com.
    rrsets_to_add_.push_back(nsec3_atcnamewild_txt);
    enableNSEC3(rrsets_to_add_);

    query.process(*list_, Name("www.cnamewild.example.com"),
                  RRType::A(), response, true);
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 2, 2, 0,
                  (string(cnamewild_txt).replace(0, 1, "www") +
                   string("www.cnamewild.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("CNAME") + "\n").c_str(),
                  (string(nsec3_www_txt) +
                   nsec3_hash_.calculate(Name("www.example.com.")) +
                   string(".example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC3")).c_str(),
                  NULL, // we are not interested in additionals in this test
                  mock_finder->getOrigin());
}

TEST_F(QueryTestForMockOnly, badWildcardNSEC3) {
    // This is a broken data source scenario; works only with mock.

    // Similar to wildcardNSEC3, but emulating run time collision by
    // returning NULL in the next closer proof for the closest encloser
    // proof.
    mock_finder->setNSEC3Flag(true);
    ZoneFinder::FindNSEC3Result nsec3(true, 0, textToRRset(nsec3_apex_txt),
                                      ConstRRsetPtr());
    mock_finder->setNSEC3Result(&nsec3);

    EXPECT_THROW(query.process(*list_, Name("www.wild.example.com"),
                               RRType::A(), response, true),
                 Query::BadNSEC3);
}

TEST_F(QueryTestForMockOnly, badWildcardProof1) {
    // This is a broken data source scenario; works only with mock.

    // Unexpected case in wildcard proof: ZoneFinder::find() returns SUCCESS
    // when NXDOMAIN is expected.
    mock_finder->setNSECResult(Name("www.wild.example.com"),
                               ZoneFinder::SUCCESS,
                               mock_finder->dname_rrset_);
    EXPECT_THROW(query.process(*list_, Name("www.wild.example.com"),
                               RRType::A(), response, true),
                 Query::BadNSEC);
}

TEST_F(QueryTestForMockOnly, badWildcardProof2) {
    // This is a broken data source scenario; works only with mock.

    // "wildcard proof" doesn't return RRset.
    mock_finder->setNSECResult(Name("www.wild.example.com"),
                               ZoneFinder::NXDOMAIN, ConstRRsetPtr());
    EXPECT_THROW(query.process(*list_, Name("www.wild.example.com"),
                               RRType::A(), response, true),
                 Query::BadNSEC);
}

TEST_F(QueryTestForMockOnly, badWildcardProof3) {
    // This is a broken data source scenario; works only with mock.

    // "wildcard proof" returns empty NSEC.
    mock_finder->setNSECResult(Name("www.wild.example.com"),
                               ZoneFinder::NXDOMAIN,
                               mock_finder->empty_nsec_rrset_);
    EXPECT_THROW(query.process(*list_, Name("www.wild.example.com"),
                               RRType::A(), response, true),
                 Query::BadNSEC);
}

TEST_P(QueryTest, wildcardNxrrsetWithDuplicateNSEC) {
    // NXRRSET on WILDCARD with DNSSEC proof.  We should have SOA, NSEC that
    // proves the NXRRSET and their RRSIGs. In this case we only need one NSEC,
    // which proves both NXDOMAIN and the non existence RRSETs of wildcard.
    query.process(*list_, Name("www.wild.example.com"), RRType::TXT(),
                  response, true);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 4, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   string(nsec_wild_txt) +
                   string("*.wild.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC")+"\n").c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, wildcardNxrrsetWithNSEC) {
    // WILDCARD + NXRRSET with DNSSEC proof.  We should have SOA, NSEC that
    // proves the NXRRSET and their RRSIGs. In this case we need two NSEC RRs,
    // one proves NXDOMAIN and the other proves non existence RRSETs of
    // wildcard.
    query.process(*list_, Name("www1.uwild.example.com"),
                  RRType::TXT(), response, true);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 6, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   string(nsec_wild_txt_nxrrset) +
                   string("*.uwild.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC")+"\n" +
                   string(nsec_wild_txt_next) +
                   string("www.uwild.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC") + "\n").c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, wildcardNxrrsetWithNSEC3) {
    // Similar to the previous case, but providing NSEC3 proofs according to
    // RFC5155 Section 7.2.5.

    rrsets_to_add_.push_back(nsec3_wild_txt);
    rrsets_to_add_.push_back(nsec3_uwild_txt);
    enableNSEC3(rrsets_to_add_);

    query.process(*list_, Name("www1.uwild.example.com"),
                  RRType::TXT(), response, true);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 8, 0, NULL,
                  // SOA + its RRSIG
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   // NSEC3 for the closest encloser + its RRSIG
                   string(nsec3_uwild_txt) +
                   nsec3_hash_.calculate(Name("uwild.example.com.")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3") + "\n" +
                   // NSEC3 for the next closer + its RRSIG
                   string(nsec3_www_txt) +
                   nsec3_hash_.calculate(Name("www.example.com.")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3") + "\n" +
                   // NSEC3 for the wildcard + its RRSIG
                   string(nsec3_wild_txt) +
                   nsec3_hash_.calculate(Name("*.uwild.example.com.")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3")).c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_F(QueryTestForMockOnly, wildcardNxrrsetWithNSEC3Collision) {
    // This is a broken data source scenario; works only with mock.

    // Similar to the previous case, but emulating run time collision by
    // returning NULL in the next closer proof for the closest encloser
    // proof.
    mock_finder->setNSEC3Flag(true);
    ZoneFinder::FindNSEC3Result nsec3(true, 0, textToRRset(nsec3_apex_txt),
                                      ConstRRsetPtr());
    mock_finder->setNSEC3Result(&nsec3);

    EXPECT_THROW(query.process(*list_, Name("www1.uwild.example.com"),
                               RRType::TXT(), response, true),
                 Query::BadNSEC3);
}

TEST_F(QueryTestForMockOnly, wildcardNxrrsetWithNSEC3Broken) {
    // This is a broken data source scenario; works only with mock.

    // Similar to wildcardNxrrsetWithNSEC3, but no matching NSEC3 for the
    // wildcard name will be returned.  This shouldn't happen in a reasonably
    // NSEC3-signed zone, and should result in an exception.
    mock_finder->setNSEC3Flag(true);
    const Name wname("*.uwild.example.com.");
    ZoneFinder::FindNSEC3Result nsec3(false, 0, textToRRset(nsec3_apex_txt),
                                      ConstRRsetPtr());
    mock_finder->setNSEC3Result(&nsec3, &wname);
    mock_finder->addRecord(nsec3_wild_txt);
    mock_finder->addRecord(nsec3_uwild_txt);

    EXPECT_THROW(query.process(*list_, Name("www1.uwild.example.com"),
                               RRType::TXT(), response, true),
                 Query::BadNSEC3);
}

TEST_P(QueryTest, wildcardEmptyWithNSEC) {
    // Empty WILDCARD with DNSSEC proof.  We should have SOA, NSEC that proves
    // the NXDOMAIN and their RRSIGs. In this case we need two NSEC RRs,
    // one proves NXDOMAIN and the other proves non existence wildcard.
    query.process(*list_, Name("a.t.example.com"), RRType::A(),
                  response, true);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 6, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   string(nsec_empty_prev_txt) +
                   string("t.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC")+"\n" +
                   string(nsec_empty_txt) +
                   string("b.*.t.example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC")+"\n").c_str(),
                  NULL, mock_finder->getOrigin());
}

/*
 * This tests that when there's no SOA and we need a negative answer. It should
 * throw in that case.
 */
TEST_F(QueryTestForMockOnly, noSOA) {
    // This is a broken data source scenario; works only with mock.

    // disable zone's SOA RR.
    mock_finder->setSOAFlag(false);

    // The NX Domain
    EXPECT_THROW(query.process(*list_, Name("nxdomain.example.com"),
                               qtype, response), Query::NoSOA);
    // Of course, we don't look into the response, as it throwed

    // NXRRSET
    EXPECT_THROW(query.process(*list_, Name("nxrrset.example.com"),
                               qtype, response), Query::NoSOA);
}

TEST_P(QueryTest, noMatchZone) {
    // there's a zone in the memory datasource but it doesn't match the qname.
    // should result in REFUSED.
    query.process(*list_, Name("example.org"), qtype, response);
    EXPECT_EQ(Rcode::REFUSED(), response.getRcode());
}

/*
 * Test MX additional processing.
 *
 * The MX RRset has two RRs, one pointing to a known domain with
 * A record, other to unknown out of zone one.
 */
TEST_P(QueryTest, MX) {
    query.process(*list_, Name("mx.example.com"), RRType::MX(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 3, 3, 4,
                  mx_txt, NULL,
                  (string(ns_addrs_txt) + string(www_a_txt)).c_str());
}

/*
 * Test when we ask for MX whose exchange is an alias (CNAME in this case).
 *
 * This should not trigger the additional processing for the exchange.
 */
TEST_P(QueryTest, MXAlias) {
    query.process(*list_, Name("cnamemx.example.com"), RRType::MX(),
                  response);

    // there shouldn't be no additional RRs for the exchanges (we have 3
    // RRs for the NS).  The normal MX case is tested separately so we don't
    // bother to examine the answer (and authority) sections.
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 3,
                  NULL, NULL, ns_addrs_txt);
}

/*
 * Tests encountering a cname.
 *
 * There are tests leading to successful answers, NXRRSET, NXDOMAIN and
 * out of the zone.
 *
 * TODO: We currently don't do chaining, so only the CNAME itself should be
 * returned.
 */
TEST_P(QueryTest, CNAME) {
    query.process(*list_, Name("cname.example.com"), RRType::A(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 0, 0,
        cname_txt, NULL, NULL);
}

TEST_P(QueryTest, explicitCNAME) {
    // same owner name as the CNAME test but explicitly query for CNAME RR.
    // expect the same response as we don't provide a full chain yet.
    query.process(*list_, Name("cname.example.com"), RRType::CNAME(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 3,
        cname_txt, zone_ns_txt, ns_addrs_txt);
}

TEST_P(QueryTest, CNAME_NX_RRSET) {
    // Leads to www.example.com, it doesn't have TXT
    // note: with chaining, what should be expected is not trivial:
    // BIND 9 returns the CNAME in answer and SOA in authority, no additional.
    // NSD returns the CNAME, NS in authority, A/AAAA for NS in additional.
    query.process(*list_, Name("cname.example.com"), RRType::TXT(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 0, 0,
        cname_txt, NULL, NULL);
}

TEST_P(QueryTest, explicitCNAME_NX_RRSET) {
    // same owner name as the NXRRSET test but explicitly query for CNAME RR.
    query.process(*list_, Name("cname.example.com"), RRType::CNAME(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 3,
        cname_txt, zone_ns_txt, ns_addrs_txt);
}

TEST_P(QueryTest, CNAME_NX_DOMAIN) {
    // Leads to nxdomain.example.com
    // note: with chaining, what should be expected is not trivial:
    // BIND 9 returns the CNAME in answer and SOA in authority, no additional,
    // RCODE being NXDOMAIN.
    // NSD returns the CNAME, NS in authority, A/AAAA for NS in additional,
    // RCODE being NOERROR.
    query.process(*list_, Name("cnamenxdom.example.com"), RRType::A(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 0, 0,
        cname_nxdom_txt, NULL, NULL);
}

TEST_P(QueryTest, explicitCNAME_NX_DOMAIN) {
    // same owner name as the NXDOMAIN test but explicitly query for CNAME RR.
    query.process(*list_, Name("cnamenxdom.example.com"),
                  RRType::CNAME(), response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 3,
        cname_nxdom_txt, zone_ns_txt, ns_addrs_txt);
}

TEST_P(QueryTest, CNAME_OUT) {
    /*
     * This leads out of zone. This should have only the CNAME even
     * when we do chaining.
     *
     * TODO: We should be able to have two zones in the mock data source.
     * Then the same test should be done with .org included there and
     * see what it does (depends on what we want to do)
     */
    query.process(*list_, Name("cnameout.example.com"), RRType::A(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 0, 0,
        cname_out_txt, NULL, NULL);
}

TEST_P(QueryTest, explicitCNAME_OUT) {
    // same owner name as the OUT test but explicitly query for CNAME RR.
    query.process(*list_, Name("cnameout.example.com"), RRType::CNAME(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 3,
        cname_out_txt, zone_ns_txt, ns_addrs_txt);
}

/*
 * Test a query under a domain with DNAME. We should get a synthetized CNAME
 * as well as the DNAME.
 *
 * TODO: Once we have CNAME chaining, check it works with synthetized CNAMEs
 * as well. This includes tests pointing inside the zone, outside the zone,
 * pointing to NXRRSET and NXDOMAIN cases (similarly as with CNAME).
 */
TEST_P(QueryTest, DNAME) {
    query.process(*list_, Name("www.dname.example.com"), RRType::A(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 2, 0, 0,
        (string(dname_txt) + synthetized_cname_txt).c_str(),
        NULL, NULL);
}

/*
 * Ask an ANY query below a DNAME. Should return the DNAME and synthetized
 * CNAME.
 *
 * ANY is handled specially sometimes. We check it is not the case with
 * DNAME.
 */
TEST_P(QueryTest, DNAME_ANY) {
    query.process(*list_, Name("www.dname.example.com"), RRType::ANY(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 2, 0, 0,
        (string(dname_txt) + synthetized_cname_txt).c_str(), NULL, NULL);
}

// Test when we ask for DNAME explicitly, it does no synthetizing.
TEST_P(QueryTest, explicitDNAME) {
    query.process(*list_, Name("dname.example.com"), RRType::DNAME(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 3,
        dname_txt, zone_ns_txt, ns_addrs_txt);
}

/*
 * Request a RRset at the domain with DNAME. It should not synthetize
 * the CNAME, it should return the RRset.
 */
TEST_P(QueryTest, DNAME_A) {
    query.process(*list_, Name("dname.example.com"), RRType::A(),
                  response);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 1, 3, 3,
        dname_a_txt, zone_ns_txt, ns_addrs_txt);
}

/*
 * Request a RRset at the domain with DNAME that is not there (NXRRSET).
 * It should not synthetize the CNAME.
 */
TEST_P(QueryTest, DNAME_NX_RRSET) {
    EXPECT_NO_THROW(query.process(*list_, Name("dname.example.com"),
                    RRType::TXT(), response));

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 1, 0,
        NULL, soa_minttl_txt, NULL, mock_finder->getOrigin());
}

/*
 * Constructing the CNAME will result in a name that is too long. This,
 * however, should not throw (and crash the server), but respond with
 * YXDOMAIN.
 */
TEST_P(QueryTest, LongDNAME) {
    // A name that is as long as it can be
    Name longname(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
        "dname.example.com.");
    EXPECT_NO_THROW(query.process(*list_, longname, RRType::A(),
                    response));

    responseCheck(response, Rcode::YXDOMAIN(), AA_FLAG, 1, 0, 0,
        dname_txt, NULL, NULL);
}

/*
 * Constructing the CNAME will result in a name of maximal length.
 * This tests that we don't reject valid one by some kind of off by
 * one mistake.
 */
TEST_P(QueryTest, MaxLenDNAME) {
    Name longname(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
        "dname.example.com.");
    EXPECT_NO_THROW(query.process(*list_, longname, RRType::A(),
                    response));

    // Check the answer is OK
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 2, 0, 0,
        NULL, NULL, NULL);

    // Check that the CNAME has the maximal length.
    bool ok(false);
    for (RRsetIterator i(response.beginSection(Message::SECTION_ANSWER));
        i != response.endSection(Message::SECTION_ANSWER); ++ i) {
        if ((*i)->getType() == RRType::CNAME()) {
            ok = true;
            RdataIteratorPtr ci((*i)->getRdataIterator());
            ASSERT_FALSE(ci->isLast()) << "The CNAME is empty";
            /*
             * Does anybody have a clue why, if the Name::MAX_WIRE is put
             * directly inside ASSERT_EQ, it fails to link and complains
             * it is unresolved external?
             */
            const size_t max_len(Name::MAX_WIRE);
            ASSERT_EQ(max_len, dynamic_cast<const rdata::generic::CNAME&>(
                ci->getCurrent()).getCname().getLength());
        }
    }
    EXPECT_TRUE(ok) << "The synthetized CNAME not found";
}

// Test for this test module itself
void
nsec3Check(bool expected_matched, uint8_t expected_labels,
           const string& expected_rrsets_txt,
           const ZoneFinder::FindNSEC3Result& result)
{
    vector<ConstRRsetPtr> actual_rrsets;
    EXPECT_EQ(expected_matched, result.matched);
    // Convert to int so the error messages would be more readable:
    EXPECT_EQ(static_cast<int>(expected_labels),
              static_cast<int>(result.closest_labels));
    if (result.closest_proof) {
        actual_rrsets.push_back(result.closest_proof);
    }
    if (result.next_proof) {
        actual_rrsets.push_back(result.next_proof);
    }
    rrsetsCheck(expected_rrsets_txt, actual_rrsets.begin(),
                actual_rrsets.end());
}

TEST_F(QueryTestForMockOnly, findNSEC3) {
    // This test is intended to test the mock data source behavior; no need
    // to do it for others.

    // In all test cases in the recursive mode, the closest encloser is the
    // apex, and result's closest_labels should be the number of apex labels.
    // (In non recursive mode closest_labels should be the # labels of the
    // query name)
    const uint8_t expected_closest_labels =
        Name("example.com").getLabelCount();

    // Apex name.  It should have a matching NSEC3
    {
        SCOPED_TRACE("apex, non recursive");
        nsec3Check(true, expected_closest_labels,
                   string(nsec3_apex_txt) + "\n" + nsec3_apex_rrsig_txt,
                   mock_finder->findNSEC3(Name("example.com"), false));
    }

    // Recursive mode doesn't change the result in this case.
    {
        SCOPED_TRACE("apex, recursive");
        nsec3Check(true, expected_closest_labels,
                   string(nsec3_apex_txt) + "\n" + nsec3_apex_rrsig_txt,
                   mock_finder->findNSEC3(Name("example.com"), true));
    }

    // Non existent name.  Disabling recursion, a covering NSEC3 should be
    // returned.
    {
        SCOPED_TRACE("nxdomain, non recursive");
        nsec3Check(false, 4,
                   string(nsec3_www_txt) + "\n" + nsec3_www_rrsig_txt,
                   mock_finder->findNSEC3(Name("nxdomain.example.com"),
                                          false));
    }

    // Non existent name.  The closest provable encloser is the apex,
    // and next closer is the query name.
    {
        SCOPED_TRACE("nxdomain, recursive");
        nsec3Check(true, expected_closest_labels,
                   string(nsec3_apex_txt) + "\n" + nsec3_apex_rrsig_txt + "\n" +
                   nsec3_www_txt + "\n" + nsec3_www_rrsig_txt,
                   mock_finder->findNSEC3(Name("nxdomain.example.com"), true));
    }

    // Similar to the previous case, but next closer name is different
    // (is the parent) of the non existent name.
    {
        SCOPED_TRACE("nxdomain, next closer != qname");
        nsec3Check(true, expected_closest_labels,
                   string(nsec3_apex_txt) + "\n" +
                   nsec3_apex_rrsig_txt + "\n" +
                   nsec3_www_txt + "\n" + nsec3_www_rrsig_txt,
                   mock_finder->findNSEC3(Name("nx.domain.example.com"),
                                          true));
    }

    // In the rest of test we check hash comparison for wrap around cases.
    {
        SCOPED_TRACE("largest");
        nsec3Check(false, 4,
                   string(nsec3_apex_txt) + "\n" + nsec3_apex_rrsig_txt,
                   mock_finder->findNSEC3(Name("nxdomain2.example.com"),
                                          false));
    }
    {
        SCOPED_TRACE("smallest");
        nsec3Check(false, 4,
                   string(nsec3_www_txt) + "\n" + nsec3_www_rrsig_txt,
                   mock_finder->findNSEC3(Name("nxdomain3.example.com"),
                                          false));
    }
}

// This tests that the DS is returned above the delegation point as
// an authoritative answer, not a delegation. This is as described in
// RFC 4035, section 3.1.4.1.

// This mock finder is used for some DS-query tests to support the cases
// where the query is expected to be handled in a different zone than our
// main test zone, example.com.  Only limited methods are expected to called
// (and for limited purposes) on this class object in these tests, which
// are overridden below.
class AlternateZoneFinder : public MockZoneFinder {
public:
    // This zone is expected not to have a DS by default and return NXRRSET
    // for a DS query.  If have_ds is set to true on construction, it will
    // return a faked DS answer.
    AlternateZoneFinder(const Name& origin, bool have_ds = false) :
        MockZoneFinder(), origin_(origin), have_ds_(have_ds)
    {}
    virtual bundy::dns::Name getOrigin() const { return (origin_); }
    virtual ZoneFinderContextPtr find(const bundy::dns::Name&,
                                      const bundy::dns::RRType& type,
                                      const FindOptions options)
    {
        if (type == RRType::SOA()) {
            RRsetPtr soa = textToRRset(origin_.toText() + " 3600 IN SOA . . "
                                       "0 0 0 0 0\n", origin_);
            soa->addRRsig(RdataPtr(new generic::RRSIG(
                                       getCommonRRSIGText("SOA"))));
            return (createContext(options, SUCCESS, soa));
        }
        if (type == RRType::NS()) {
            RRsetPtr ns = textToRRset(origin_.toText() + " 3600 IN NS " +
                                      Name("ns").concatenate(origin_).toText());
            ns->addRRsig(RdataPtr(new generic::RRSIG(
                                      getCommonRRSIGText("NS"))));
            return (createContext(options, SUCCESS, ns));
        }
        if (type == RRType::DS()) {
            if (have_ds_) {
                RRsetPtr ds = textToRRset(origin_.toText() +
                                          " 3600 IN DS 57855 5 1 " +
                                          "49FD46E6C4B45C55D4AC69CBD"
                                          "3CD34AC1AFE51DE");
                ds->addRRsig(RdataPtr(new generic::RRSIG(
                                          getCommonRRSIGText("DS"))));
                return (createContext(options, SUCCESS, ds));
            } else {
                RRsetPtr nsec = textToRRset(origin_.toText() +
                                            " 3600 IN NSEC " +
                                            origin_.toText() +
                                            " SOA NSEC RRSIG");
                nsec->addRRsig(RdataPtr(new generic::RRSIG(
                                            getCommonRRSIGText("NSEC"))));
                return (createContext(options, NXRRSET, nsec,
                                      RESULT_NSEC_SIGNED));
            }
        }

        // Returning NXDOMAIN is not correct, but doesn't matter for our tests.
        return (createContext(options, NXDOMAIN, ConstRRsetPtr()));
    }
private:
    const Name origin_;
    const bool have_ds_;
};

TEST_F(QueryTestForMockOnly, dsAboveDelegation) {
    // We could setup the child zone for other data sources, but it won't be
    // simple addition.  For now we test it for mock only.

    // Pretending to have authority for the child zone, too.
    mock_client.addZone(ZoneFinderPtr(new AlternateZoneFinder(
                                           Name("delegation.example.com"))));

    // The following will succeed only if the search goes to the parent
    // zone, not the child one we added above.
    EXPECT_NO_THROW(query.process(*list_,
                                  Name("delegation.example.com"),
                                  RRType::DS(), response, true));

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 2, 4, 6,
                  (string(delegation_ds_txt) + "\n" +
                   "delegation.example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("DS")).c_str(),
                  (string(zone_ns_txt) + "\n" +
                   "example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NS")).c_str(),
                  ns_addrs_and_sig_txt.c_str());
}

TEST_P(QueryTest, dsAboveDelegationNoData) {
    // Similar to the previous case, but the query is for an unsigned zone
    // (which doesn't have a DS at the parent).  The response should be a
    // "no data" error.  The query should still be handled at the parent.
    mock_client.addZone(ZoneFinderPtr(
                             new AlternateZoneFinder(
                                  Name("unsigned-delegation.example.com"))));

    // The following will succeed only if the search goes to the parent
    // zone, not the child one we added above.
    EXPECT_NO_THROW(query.process(*list_,
                                  Name("unsigned-delegation.example.com"),
                                  RRType::DS(), response, true));

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 4, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   string(unsigned_delegation_nsec_txt) +
                   "unsigned-delegation.example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC")).c_str(),
                  NULL, mock_finder->getOrigin());
}

// This one checks that type-DS query results in a "no data" response
// when it happens to be sent to the child zone, as described in RFC 4035,
// section 3.1.4.1. The example is inspired by the B.8. example from the RFC.
TEST_P(QueryTest, dsBelowDelegation) {
    EXPECT_NO_THROW(query.process(*list_, Name("example.com"),
                                  RRType::DS(), response, true));

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 4, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   string(nsec_apex_txt) + "\n" +
                   string("example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC")).c_str(), NULL,
                  mock_finder->getOrigin());
}

// Similar to the previous case, but even more pathological: the DS somehow
// exists in the child zone.  The Query module should still return SOA.
// In our implementation NSEC/NSEC3 isn't attached in this case.
TEST_P(QueryTest, dsBelowDelegationWithDS) {
    rrsets_to_add_.push_back(zone_ds_txt);
    addRRsets(rrsets_to_add_, *list_, base_zone_file);
    EXPECT_NO_THROW(query.process(*list_, Name("example.com"),
                                  RRType::DS(), response, true));

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 2, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA")).c_str(), NULL,
                  mock_finder->getOrigin());
}

// DS query received at a completely irrelevant (neither parent nor child)
// server.  It should just like the "noZone" test case, but DS query involves
// special processing, so we test it explicitly.
TEST_P(QueryTest, dsNoZone) {
    query.process(*list_, Name("example"), RRType::DS(), response,
                  true);
    responseCheck(response, Rcode::REFUSED(), 0, 0, 0, 0, NULL, NULL, NULL);
}

// DS query for a "grandchild" zone.  This should result in normal
// delegation (unless this server also has authority of the grandchild zone).
TEST_P(QueryTest, dsAtGrandParent) {
    query.process(*list_, Name("grand.delegation.example.com"),
                  RRType::DS(), response, true);
    responseCheck(response, Rcode::NOERROR(), 0, 0, 6, 6, NULL,
                  (string(delegation_txt) + string(delegation_ds_txt) +
                   "delegation.example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("DS")).c_str(),
                  ns_addrs_and_sig_txt.c_str());
}

// DS query sent to a "grandparent" server that also has authority for the
// child zone.  In this case the query should be handled in the child
// side and should result in no data with SOA.  Note that the server doesn't
// have authority for the "parent".  Unlike the dsAboveDelegation test case
// the query should be handled in the child zone, not in the grandparent.
TEST_F(QueryTestForMockOnly, dsAtGrandParentAndChild) {
    // We could setup the child zone for other data sources, but it won't be
    // simple addition.  For now we test it for mock only.

    // Pretending to have authority for the child zone, too.
    const Name childname("grand.delegation.example.com");
    mock_client.addZone(ZoneFinderPtr(
                             new AlternateZoneFinder(childname)));
    query.process(*list_, childname, RRType::DS(), response, true);
    // Note that RR TTL of SOA and its RRSIG are set to SOA MINTTL, 0
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 4, 0, NULL,
                  (childname.toText() + " 0 IN SOA . . 0 0 0 0 0\n" +
                   childname.toText() + " 0 IN RRSIG " +
                   getCommonRRSIGText("SOA") + "\n" +
                   childname.toText() + " 3600 IN NSEC " +
                   childname.toText() + " SOA NSEC RRSIG\n" +
                   childname.toText() + " 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC")).c_str(), NULL, childname);
}

// DS query for the root name (quite pathological).  Since there's no "parent",
// the query will be handled in the root zone anyway, and should (normally)
// result in no data.
TEST_F(QueryTestForMockOnly, dsAtRoot) {
    // We could setup the additional zone for other data sources, but it
    // won't be simple addition.  For now we test it for mock only.

    // Pretend to be a root server.
    mock_client.addZone(ZoneFinderPtr(
                             new AlternateZoneFinder(Name::ROOT_NAME())));
    query.process(*list_, Name::ROOT_NAME(), RRType::DS(), response,
                  true);
    // Note that RR TTL of SOA and its RRSIG are set to SOA MINTTL, 0
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 4, 0, NULL,
                  (string(". 0 IN SOA . . 0 0 0 0 0\n") +
                   ". 0 IN RRSIG " + getCommonRRSIGText("SOA") + "\n" +
                   ". 3600 IN NSEC " + ". SOA NSEC RRSIG\n" +
                   ". 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC")).c_str(), NULL);
}

// Even more pathological case: A faked root zone actually has its own DS
// query.  How we respond wouldn't matter much in practice, but check if
// it behaves as it's intended.  This implementation should return the DS.
TEST_F(QueryTestForMockOnly, dsAtRootWithDS) {
    // We could setup the additional zone for other data sources, but it
    // won't be simple addition.  For now we test it for mock only.

    mock_client.addZone(ZoneFinderPtr(
                             new AlternateZoneFinder(Name::ROOT_NAME(), true)));
    query.process(*list_, Name::ROOT_NAME(), RRType::DS(), response,
                  true);
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 2, 2, 0,
                  (string(". 3600 IN DS 57855 5 1 49FD46E6C4B45C55D4AC69CBD"
                          "3CD34AC1AFE51DE\n") +
                   ". 3600 IN RRSIG " + getCommonRRSIGText("DS")).c_str(),
                  (string(". 3600 IN NS ns.\n") +
                   ". 3600 IN RRSIG " + getCommonRRSIGText("NS")).c_str(),
                  NULL);
}

// Check the signature is present when an NXRRSET is returned
TEST_P(QueryTest, nxrrsetWithNSEC3) {
    enableNSEC3(rrsets_to_add_);

    // NXRRSET with DNSSEC proof.  We should have SOA, NSEC3 that proves the
    // NXRRSET and their RRSIGs.
    query.process(*list_, Name("www.example.com"), RRType::TXT(),
                  response, true);

    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 4, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   string(nsec3_www_txt) + "\n" +
                   nsec3_hash_.calculate(Name("www.example.com.")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3") + "\n").c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, nxrrsetDerivedFromOptOutNSEC3) {
    // In this test we emulate the situation where an empty non-terminal name
    // is derived from insecure delegation and covered by an opt-out NSEC3.
    // In the actual test data the covering NSEC3 really has the opt-out
    // bit set, although the implementation doesn't check it anyway.
    enableNSEC3(rrsets_to_add_);
    query.process(*list_, Name("empty.example.com"), RRType::TXT(), response,
                  true);

    // The closest provable encloser is the origin name (example.com.), and
    // the next closer is the empty name itself, which is expected to be
    // covered by an opt-out NSEC3 RR.  The response should contain these 2
    // NSEC3s.
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 6, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   string(nsec3_apex_txt) + "\n" +
                   nsec3_hash_.calculate(Name("example.com.")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3") + "\n" +
                   string(nsec3_www_txt) + "\n" +
                   nsec3_hash_.calculate(Name("www.example.com.")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3") + "\n").c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, nxrrsetWithNSEC3_ds_exact) {
    rrsets_to_add_.push_back(unsigned_delegation_nsec3_txt);
    enableNSEC3(rrsets_to_add_);

    // This delegation has no DS, but does have a matching NSEC3 record
    // (See RFC5155 section 7.2.4)
    query.process(*list_, Name("unsigned-delegation.example.com."),
                  RRType::DS(), response, true);
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 4, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   string(unsigned_delegation_nsec3_txt) + "\n" +
                   nsec3_hash_.calculate(
                       Name("unsigned-delegation.example.com.")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3") + "\n").c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, nxrrsetWithNSEC3_ds_no_exact) {
    rrsets_to_add_.push_back(unsigned_delegation_nsec3_txt);
    enableNSEC3(rrsets_to_add_);

    // This delegation has no DS, and no directly matching NSEC3 record
    // So the response should contain closest encloser proof (and the
    // 'next closer' should have opt-out set, though that is not
    // actually checked)
    // (See RFC5155 section 7.2.4)
    query.process(*list_, Name("unsigned-delegation-optout.example.com."),
                  RRType::DS(), response, true);
    responseCheck(response, Rcode::NOERROR(), AA_FLAG, 0, 6, 0, NULL,
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   string(nsec3_apex_txt) + "\n" +
                   nsec3_hash_.calculate(Name("example.com.")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3") + "\n" +
                   string(unsigned_delegation_nsec3_txt) + "\n" +
                   nsec3_hash_.calculate(
                       Name("unsigned-delegation.example.com.")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3") + "\n").c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_P(QueryTest, nxdomainWithNSEC3Proof) {
    // Name Error (NXDOMAIN) case with NSEC3 proof per RFC5155 Section 7.2.2.

    // This will be the covering NSEC3 for the next closer
    rrsets_to_add_.push_back(nsec3_uwild_txt);
    // This will be the covering NSEC3 for the possible wildcard
    rrsets_to_add_.push_back(unsigned_delegation_nsec3_txt);
    // Enable NSEC3
    enableNSEC3(rrsets_to_add_);

    query.process(*list_, Name("nxdomain.example.com"), qtype,
                  response, true);
    responseCheck(response, Rcode::NXDOMAIN(), AA_FLAG, 0, 8, 0, NULL,
                  // SOA + its RRSIG
                  (string(soa_minttl_txt) +
                   string("example.com. 0 IN RRSIG ") +
                   getCommonRRSIGText("SOA") + "\n" +
                   // NSEC3 for the closest encloser + its RRSIG
                   string(nsec3_apex_txt) + "\n" +
                   nsec3_hash_.calculate(mock_finder->getOrigin()) +
                   string(".example.com. 3600 IN RRSIG ") +
                   getCommonRRSIGText("NSEC3") + "\n" +
                   // NSEC3 for the next closer + its RRSIG
                   string(nsec3_uwild_txt) + "\n" +
                   nsec3_hash_.calculate(Name("uwild.example.com")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3") + "\n" +
                   // NSEC3 for the wildcard + its RRSIG
                   string(unsigned_delegation_nsec3_txt) +
                   nsec3_hash_.calculate(
                       Name("unsigned-delegation.example.com")) +
                   ".example.com. 3600 IN RRSIG " +
                   getCommonRRSIGText("NSEC3")).c_str(),
                  NULL, mock_finder->getOrigin());
}

TEST_F(QueryTestForMockOnly, nxdomainWithBadNextNSEC3Proof) {
    // This is a broken data source scenario; works only with mock.

    // Similar to the previous case, but emulating run time collision by
    // returning NULL in the next closer proof for the closest encloser
    // proof.
    mock_finder->setNSEC3Flag(true);
    ZoneFinder::FindNSEC3Result nsec3(true, 0, textToRRset(nsec3_apex_txt),
                                      ConstRRsetPtr());
    mock_finder->setNSEC3Result(&nsec3);

    EXPECT_THROW(query.process(*list_, Name("nxdomain.example.com"),
                               RRType::TXT(), response, true),
                 Query::BadNSEC3);
}

TEST_F(QueryTestForMockOnly, nxdomainWithBadWildcardNSEC3Proof) {
    // This is a broken data source scenario; works only with mock.

    // Similar to nxdomainWithNSEC3Proof, but let findNSEC3() return a matching
    // NSEC3 for the possible wildcard name, emulating run-time collision.
    // This should result in BadNSEC3 exception.

    mock_finder->setNSEC3Flag(true);
    mock_finder->addRecord(nsec3_uwild_txt);
    mock_finder->addRecord(unsigned_delegation_nsec3_txt);

    const Name wname("*.example.com");
    ZoneFinder::FindNSEC3Result nsec3(true, 0, textToRRset(nsec3_apex_txt),
                                      ConstRRsetPtr());
    mock_finder->setNSEC3Result(&nsec3, &wname);

    EXPECT_THROW(query.process(*list_, Name("nxdomain.example.com"), qtype,
                               response, true),
                 Query::BadNSEC3);
}

// The following are tentative tests until we really add tests for the
// query logic for these cases.  At that point it's probably better to
// clean them up.
TEST_P(QueryTest, emptyNameWithNSEC3) {
    enableNSEC3(rrsets_to_add_);
    const Name qname("no.example.com");
    ASSERT_TRUE(list_->find(qname).finder_);
    ZoneFinderContextPtr result =
        list_->find(qname).finder_->find(qname, RRType::A(),
                                         ZoneFinder::FIND_DNSSEC);
    EXPECT_EQ(ZoneFinder::NXRRSET, result->code);
    EXPECT_FALSE(result->rrset);
    EXPECT_TRUE(result->isNSEC3Signed());
    EXPECT_FALSE(result->isWildcard());
}

// Vector of RRsets used for the test.   Having this external to functions and
// classes used for the testing simplifies the code.
std::vector<RRsetPtr> rrset_vector;

// Callback function for masterLoad.
void
loadRRsetVectorCallback(RRsetPtr rrsetptr) {
    rrset_vector.push_back(rrsetptr);
}

// Load a set of RRsets into a vector for use in the duplicate RRset test.
// They don't make a lot of sense as a zone, they are just different.  The
// variables used in the stringstream input have been chosen so that each
// represents just one RRset.
void
loadRRsetVector() {
    stringstream ss;

    // Comments indicate offset in the rrset_vector (when loaded) and the
    // number of RRs in that RRset.
    ss << soa_txt               // 0(1)
       << zone_ns_txt           // 1(3)
       << delegation_txt        // 2(4)
       << delegation_ds_txt     // 3(1)
       << mx_txt                // 4(3)
       << www_a_txt             // 5(1)
       << cname_txt             // 6(1)
       << cname_nxdom_txt       // 7(1)
       << cname_out_txt;        // 8(1)
    rrset_vector.clear();
    masterLoad(ss, Name("example.com."), RRClass::IN(),
               loadRRsetVectorCallback);
}

// Note: this is an independent test; don't have to be in the QueryTest
// fixture.
TEST(QueryTestSingle, DuplicateNameRemoval) {

    // Load some RRsets into the master vector.
    loadRRsetVector();
    EXPECT_EQ(9, rrset_vector.size());

    // Create an answer, authority and additional vector with some overlapping
    // entries.  The following indicate which elements from rrset_vector
    // go into each section vector.  (The values have been separated to show
    // the overlap.)
    //
    // answer     = 0 1 2 3
    // authority  =     2 3 4 5 6 7...
    //                     ...5 (duplicate in the same section)
    // additional = 0     3       7 8
    //
    // If the duplicate removal works, we should end up with the following in
    // the message created from the three vectors:
    //
    // answer     = 0 1 2 3
    // authority  =         4 5 6 7
    // additional =                 8
    //
    // The expected section into which each RRset is placed is indicated in the
    // array below.
    const Message::Section expected_section[] = {
        Message::SECTION_ANSWER,
        Message::SECTION_ANSWER,
        Message::SECTION_ANSWER,
        Message::SECTION_ANSWER,
        Message::SECTION_AUTHORITY,
        Message::SECTION_AUTHORITY,
        Message::SECTION_AUTHORITY,
        Message::SECTION_AUTHORITY,
        Message::SECTION_ADDITIONAL
    };
    EXPECT_EQ(rrset_vector.size(),
              (sizeof(expected_section) / sizeof(Message::Section)));

    // Create the vectors of RRsets (with the RRsets in a semi-random order).
    std::vector<ConstRRsetPtr> answer;
    answer.insert(answer.end(), rrset_vector.begin() + 2,
                  rrset_vector.begin() + 4);
    answer.insert(answer.end(), rrset_vector.begin() + 0,
                  rrset_vector.begin() + 2);

    std::vector<ConstRRsetPtr> authority;
    authority.insert(authority.end(), rrset_vector.begin() + 3,
                     rrset_vector.begin() + 8);
    authority.push_back(rrset_vector[2]);
    authority.push_back(rrset_vector[5]);

    std::vector<ConstRRsetPtr> additional;
    additional.insert(additional.end(), rrset_vector.begin() + 7,
                      rrset_vector.end());
    additional.push_back(rrset_vector[3]);
    additional.push_back(rrset_vector[0]);

    // Create the message object into which the RRsets are put
    Message message(Message::RENDER);
    EXPECT_EQ(0, message.getRRCount(Message::SECTION_ANSWER));
    EXPECT_EQ(0, message.getRRCount(Message::SECTION_AUTHORITY));
    EXPECT_EQ(0, message.getRRCount(Message::SECTION_ADDITIONAL));

    // ... and fill it.
    Query::ResponseCreator().create(message, answer, authority, additional);

    // Check counts in each section.  Note that these are RR counts,
    // not RRset counts.
    EXPECT_EQ(9, message.getRRCount(Message::SECTION_ANSWER));
    EXPECT_EQ(6, message.getRRCount(Message::SECTION_AUTHORITY));
    EXPECT_EQ(1, message.getRRCount(Message::SECTION_ADDITIONAL));

    // ... and check that the RRsets are in the correct section
    BOOST_STATIC_ASSERT(Message::SECTION_QUESTION == 0);
    BOOST_STATIC_ASSERT(Message::SECTION_ANSWER == 1);
    BOOST_STATIC_ASSERT(Message::SECTION_AUTHORITY == 2);
    BOOST_STATIC_ASSERT(Message::SECTION_ADDITIONAL == 3);
    Message::Section sections[] = {
        Message::SECTION_QUESTION,
        Message::SECTION_ANSWER,
        Message::SECTION_AUTHORITY,
        Message::SECTION_ADDITIONAL
    };
    for (int section = 1; section <= 3; ++section) {
        for (int vecindex = 0; vecindex < rrset_vector.size(); ++vecindex) {
            // Prepare error message in case an assertion fails (as the default
            // message will only refer to the loop indexes).
            stringstream ss;
            ss << "section " << section << ", name "
               << rrset_vector[vecindex]->getName();
            SCOPED_TRACE(ss.str());

            // Check RRset is in the right section and not in the wrong
            // section.
            if (sections[section] == expected_section[vecindex]) {
                EXPECT_TRUE(message.hasRRset(sections[section],
                            rrset_vector[vecindex]));
            } else {
                EXPECT_FALSE(message.hasRRset(sections[section],
                             rrset_vector[vecindex]));
            }
        }
    }
}
}
