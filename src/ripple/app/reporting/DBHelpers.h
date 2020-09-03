//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/reporting/ReportingETL.h>
#include <ripple/basics/Log.h>
#include <ripple/core/Pg.h>
#include <boost/container/flat_set.hpp>

#ifndef RIPPLE_CORE_DBHELPERS_H_INCLUDED
#define RIPPLE_CORE_DBHELPERS_H_INCLUDED

namespace ripple {
bool
writeToLedgersDB(
    LedgerInfo const& info,
    std::shared_ptr<PgQuery>& pgQuery,
    std::shared_ptr<Pg>& conn,
    beast::Journal& j);

struct AccountTransactionsData
{
    boost::container::flat_set<AccountID> accounts;
    uint32_t ledgerSequence;
    uint32_t transactionIndex;
    uint256 txHash;
    uint256 nodestoreHash;

    AccountTransactionsData(TxMeta& meta, uint256 nodestoreHash, beast::Journal& j)
        : accounts(meta.getAffectedAccounts(j))
        , ledgerSequence(meta.getLgrSeq())
        , transactionIndex(meta.getIndex())
        , txHash(meta.getTxID())
        , nodestoreHash(nodestoreHash)
    {
    }

    AccountTransactionsData(
        boost::container::flat_set<AccountID> const& accts,
        std::uint32_t seq,
        std::uint32_t idx,
        uint256 const& hash)
        : accounts(accts)
        , ledgerSequence(seq)
        , transactionIndex(idx)
        , txHash(hash)
    {
    }
};

void
bulkWriteToTable(
    std::shared_ptr<PgQuery>& pgQuery,
    std::shared_ptr<Pg>& conn,
    char const* copyQuery,
    std::string const bufString,
    beast::Journal& j);

bool
writeToPostgres(
    LedgerInfo const& info,
    std::vector<AccountTransactionsData>& accountTxData,
    std::shared_ptr<PgPool> const& pgPool,
    bool useTxTables,
    beast::Journal& j);

}  // namespace ripple
#endif
