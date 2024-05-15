/*
 * Copyright (C) 1996-2023 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 28    Access Control */

#include "squid.h"
#include "acl/FilledChecklist.h"
#include "acl/ServerName.h"
#include "client_side.h"
#include "http/Stream.h"
#include "HttpRequest.h"
#include "ssl/bio.h"
#include "ssl/ServerBump.h"
#include "ssl/support.h"

// Compare function for tree search algorithms
static int
aclHostDomainCompare( char *const &a, char * const &b)
{
    const char *h = static_cast<const char *>(a);
    const char *d = static_cast<const char *>(b);
    debugs(28, 7, "Match:" << h << " <>  " << d);
    return matchDomainName(h, d, mdnHonorWildcards);
}

bool
ACLServerNameData::match(const char *host)
{
    if (host == nullptr)
        return 0;

    debugs(28, 3, "checking '" << host << "'");

    char *h = const_cast<char *>(host);
    char const * const * result = domains->find(h, aclHostDomainCompare);

    debugs(28, 3, "'" << host << "' " << (result ? "found" : "NOT found"));

    return (result != nullptr);

}

/// A helper function to be used with Ssl::matchX509CommonNames().
/// \retval 0 when the name (cn or an alternate name) matches acl data
/// \retval 1 when the name does not match
template<class MatchType>
int
check_cert_domain( void *check_data, ASN1_STRING *cn_data, Ssl::AddressType addr_type)
{
    // addr_type is only declared here to ensure the signature type matches
    // for matchX509CommonNames. Void it here to avoid compiler warnings.
    (void)addr_type;
    char cn[1024];
    ACLData<MatchType> * data = (ACLData<MatchType> *)check_data;

    if (cn_data->length > (int)sizeof(cn) - 1)
        return 1; // ignore data that does not fit our buffer

    char *s = reinterpret_cast<char *>(cn_data->data);
    char *d = cn;
    for (int i = 0; i < cn_data->length; ++i, ++d, ++s) {
        if (*s == '\0')
            return 1; // always a domain mismatch. contains 0x00
        *d = *s;
    }
    cn[cn_data->length] = '\0';
    debugs(28, 4, "Verifying certificate name/subjectAltName " << cn);
    if (data->match(cn))
        return 0;
    return 1;
}

int
Acl::ServerNameCheck::match(ACLChecklist * const ch)
{
    const auto checklist = Filled(ch);

    assert(checklist != nullptr && checklist->request != nullptr);

    const char *serverName = nullptr;
    SBuf clientSniKeeper; // because c_str() is not constant
    if (ConnStateData *conn = checklist->conn()) {
        const char *clientRequestedServerName = nullptr;
        clientSniKeeper = conn->tlsClientSni();
        if (clientSniKeeper.isEmpty()) {
            const char *host = checklist->request->url.host();
            if (host && *host) // paranoid first condition: host() is never nil
                clientRequestedServerName = host;
        } else
            clientRequestedServerName = clientSniKeeper.c_str();

        if (useConsensus) {
            X509 *peer_cert = conn->serverBump() ? conn->serverBump()->serverCert.get() : nullptr;
            // use the client requested name if it matches the server
            // certificate or if the certificate is not available
            if (!peer_cert || Ssl::checkX509ServerValidity(peer_cert, clientRequestedServerName))
                serverName = clientRequestedServerName;
        } else if (useClientRequested)
            serverName = clientRequestedServerName;
        else { // either no options or useServerProvided
            if (X509 *peer_cert = (conn->serverBump() ? conn->serverBump()->serverCert.get() : nullptr))
                return Ssl::matchX509CommonNames(peer_cert, data.get(), check_cert_domain<const char*>);
            if (!useServerProvided)
                serverName = clientRequestedServerName;
        }
    }

    if (!serverName)
        serverName = "none";

    return data->match(serverName);
}

const Acl::Options &
Acl::ServerNameCheck::options()
{
    static const Acl::BooleanOption ClientRequested("--client-requested");
    static const Acl::BooleanOption ServerProvided("--server-provided");
    static const Acl::BooleanOption Consensus("--consensus");
    static const Acl::Options MyOptions = { &ClientRequested, &ServerProvided, &Consensus };
    ClientRequested.linkWith(&useClientRequested);
    ServerProvided.linkWith(&useServerProvided);
    Consensus.linkWith(&useConsensus);
    return MyOptions;
}

bool
Acl::ServerNameCheck::valid() const
{
    int optionCount = 0;

    if (useClientRequested)
        optionCount++;
    if (useServerProvided)
        optionCount++;
    if (useConsensus)
        optionCount++;

    if (optionCount > 1) {
        debugs(28, DBG_CRITICAL, "ERROR: Multiple options given for the server_name ACL");
        return false;
    }
    return true;
}

